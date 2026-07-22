#include "ExternalPlayer.h"
#include "Settings.h"

#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QProcessEnvironment>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>   // QNativeInterface::QAndroidApplication::context()
#include <QJniObject>         // JNI: ACTION_VIEW intent handoff to a system video player
#endif

namespace {

// One known desktop player: where its exe sits under a Program-Files-style root, plus the registry vendor
// key/value that (on the real Windows system) records its install directory. The fs candidate is checked
// first; the registry is a fallback for non-default install locations.
struct Candidate {
    ExternalPlayer::Kind kind;
    QString display;
    QString relExe;     // path of the exe relative to a Program Files root, e.g. "VideoLAN/VLC/vlc.exe"
    QString regKey;     // HKLM\SOFTWARE\<regKey> group (also the group used in a fake-registry ini)
    QString regValue;   // value under regKey holding the install directory
    QString regExe;     // exe filename to append to the registry install dir
};

QVector<Candidate> candidates()
{
    return {
        { ExternalPlayer::Kind::Vlc, QStringLiteral("VLC media player"),
          QStringLiteral("VideoLAN/VLC/vlc.exe"), QStringLiteral("VideoLAN/VLC"),
          QStringLiteral("InstallDir"), QStringLiteral("vlc.exe") },
        { ExternalPlayer::Kind::Mpc, QStringLiteral("MPC-HC"),
          QStringLiteral("MPC-HC/mpc-hc64.exe"), QStringLiteral("MPC-HC"),
          QStringLiteral("ExePath"), QStringLiteral("mpc-hc64.exe") },
    };
}

// Program-Files roots to scan when no fs probe root is injected. Empty on non-Windows.
QStringList realFsRoots()
{
    QStringList roots;
#ifdef Q_OS_WIN
    const auto env = QProcessEnvironment::systemEnvironment();
    for (const char* var : { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" }) {
        const QString v = env.value(QString::fromLatin1(var));
        if (!v.isEmpty() && !roots.contains(v)) roots << v;
    }
#endif
    return roots;
}

// Read a candidate's install dir from the registry. A non-empty regProbeRoot makes this read a fake registry
// (an INI file) instead of the live hive — so tests (and CI) never touch HKLM. Returns "" when unavailable.
QString registryInstallDir(const QString& regProbeRoot, const Candidate& c)
{
    if (!regProbeRoot.isEmpty()) {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        return ini.value(c.regKey + QLatin1Char('/') + c.regValue).toString();
    }
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\") + QString(c.regKey).replace(QLatin1Char('/'), QLatin1Char('\\')),
                  QSettings::NativeFormat);
    return reg.value(c.regValue).toString();
#else
    return QString();
#endif
}

} // namespace

QVector<ExternalPlayer::Detected> ExternalPlayer::detect(const QString& fsProbeRoot,
                                                         const QString& regProbeRoot)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(fsProbeRoot);
    Q_UNUSED(regProbeRoot);
    // No exe paths on Android — the only external target is a system video app via ACTION_VIEW intent.
    return { { Kind::AndroidIntent, QString(), QStringLiteral("External app") } };
#else
    QVector<Detected> out;

    // A non-empty fs probe root confines the scan to that tree (hermetic tests, no real Program Files).
    const QStringList fsRoots = fsProbeRoot.isEmpty() ? realFsRoots() : QStringList{ fsProbeRoot };

    for (const Candidate& c : candidates()) {
        QString hit;

        // Filesystem: the exe at its default location under any scanned root wins.
        for (const QString& root : fsRoots) {
            const QFileInfo fi(root + QLatin1Char('/') + c.relExe);
            if (fi.exists() && fi.isFile()) { hit = fi.absoluteFilePath(); break; }
        }

        // Registry fallback for a non-default install location (real hive, or the injected fake).
        if (hit.isEmpty()) {
            const QString dir = registryInstallDir(regProbeRoot, c);
            if (!dir.isEmpty()) {
                const QFileInfo fi(dir + QLatin1Char('/') + c.regExe);
                if (fi.exists() && fi.isFile()) hit = fi.absoluteFilePath();
            }
        }

        if (!hit.isEmpty()) out.push_back({ c.kind, hit, c.display });
    }

    return out;
#endif
}

ExternalPlayer::Kind ExternalPlayer::configuredKind()
{
    const QString s = Settings::externalPlayer();
    if (s == QStringLiteral("vlc"))     return Kind::Vlc;
    if (s == QStringLiteral("mpc"))     return Kind::Mpc;
    if (s == QStringLiteral("custom"))  return Kind::Custom;
    if (s == QStringLiteral("android")) return Kind::AndroidIntent;
    return Kind::Builtin; // "builtin", empty, or any unknown/corrupt string
}

QString ExternalPlayer::configuredPath()
{
    switch (configuredKind()) {
    case Kind::Custom:
        return Settings::externalPlayerPath();
    case Kind::Vlc:
    case Kind::Mpc: {
        const Kind want = configuredKind();
        for (const Detected& d : detect())
            if (d.kind == want) return d.path;
        return QString();
    }
    case Kind::AndroidIntent: // an intent has no exe path
    case Kind::Builtin:
    default:
        return QString();
    }
}

bool ExternalPlayer::available()
{
    const Kind k = configuredKind();
#ifdef Q_OS_ANDROID
    // On Android the only meaningful external target is the intent handoff; treat it (and a Custom pick that
    // maps to nothing here) uniformly: available iff the configured kind is the intent.
    return k == Kind::AndroidIntent;
#else
    if (k == Kind::Builtin || k == Kind::AndroidIntent) return false; // no intent target on desktop
    const QString exe = configuredPath();
    return !exe.isEmpty() && QFileInfo::exists(exe);
#endif
}

bool ExternalPlayer::launch(const QString& urlOrPath)
{
#ifdef Q_OS_ANDROID
    // ACTION_VIEW intent with a video/* type, handed to whatever app the user has for video. Every JNI step
    // is isValid()-guarded; any failure (no context, no resolvable activity) returns false so the caller can
    // notify — we never throw across the JNI boundary.
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return false;

    QJniObject jUrl = QJniObject::fromString(urlOrPath);
    QJniObject uri = QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", jUrl.object<jstring>());
    if (!uri.isValid()) return false;

    QJniObject action = QJniObject::getStaticObjectField<jstring>(
        "android/content/Intent", "ACTION_VIEW");
    if (!action.isValid()) return false;
    QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object<jstring>());
    if (!intent.isValid()) return false;

    QJniObject mime = QJniObject::fromString(QStringLiteral("video/*"));
    intent.callObjectMethod("setDataAndType",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;",
        uri.object(), mime.object<jstring>());
    // FLAG_ACTIVITY_NEW_TASK (0x10000000) — required to start an activity from a non-activity context.
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", 0x10000000);

    // No activity can handle it -> false (the caller falls back / notifies) rather than crashing.
    QJniObject pm = context.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
    if (!pm.isValid()) return false;
    QJniObject resolved = intent.callObjectMethod("resolveActivity",
        "(Landroid/content/pm/PackageManager;)Landroid/content/ComponentName;", pm.object());
    if (!resolved.isValid()) return false;

    context.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    return true;
#else
    const QString exe = configuredPath();
    if (exe.isEmpty()) return false;
    return QProcess::startDetached(exe, QStringList{ urlOrPath });
#endif
}
