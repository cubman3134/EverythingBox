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

// One registry fallback lookup for a candidate: the hive + subkey path (real hive, or the group used in a
// fake-registry ini) and the value name. `appendExe` is the exe filename to append when the value holds an
// install *directory* (VLC's InstallDir); leave it empty when the value already *is* the full exe path
// (MPC-HC's ExePath). Different players record their install differently — and under different hives.
struct RegLookup {
    QString hive;       // "HKEY_LOCAL_MACHINE" / "HKEY_CURRENT_USER" for the real hive (ignored for a fake ini)
    QString subKey;     // slash-separated key path, e.g. "SOFTWARE/VideoLAN/VLC" (also the fake-ini group)
    QString value;      // value name under subKey
    QString appendExe;  // exe to append to a directory value; "" => the value is already a full exe path
};

// One known desktop player: where its exe sits under a Program-Files-style root, plus an ordered list of
// registry fallbacks (real hive or the injected fake) that record its install for a non-default location.
// The fs candidate is checked first; the registry lookups are tried in order only if the fs check misses.
struct Candidate {
    ExternalPlayer::Kind kind;
    QString display;
    QString relExe;             // path of the exe relative to a Program Files root, e.g. "VideoLAN/VLC/vlc.exe"
    QVector<RegLookup> regs;    // registry fallbacks, tried in order
};

QVector<Candidate> candidates()
{
    return {
        // VLC records InstallDir under HKLM; a 32-bit VLC on 64-bit Windows lands under WOW6432Node.
        { ExternalPlayer::Kind::Vlc, QStringLiteral("VLC media player"),
          QStringLiteral("VideoLAN/VLC/vlc.exe"),
          { { QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("SOFTWARE/VideoLAN/VLC"),
              QStringLiteral("InstallDir"), QStringLiteral("vlc.exe") },
            { QStringLiteral("HKEY_LOCAL_MACHINE"), QStringLiteral("SOFTWARE/WOW6432Node/VideoLAN/VLC"),
              QStringLiteral("InstallDir"), QStringLiteral("vlc.exe") } } },
        // MPC-HC writes its own full exe path to ExePath under HKCU (NOT HKLM) — the value is the exe itself,
        // so nothing is appended.
        { ExternalPlayer::Kind::Mpc, QStringLiteral("MPC-HC"),
          QStringLiteral("MPC-HC/mpc-hc64.exe"),
          { { QStringLiteral("HKEY_CURRENT_USER"), QStringLiteral("Software/MPC-HC/MPC-HC"),
              QStringLiteral("ExePath"), QString() } } },
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

// Resolve one registry lookup to a candidate exe path. A non-empty regProbeRoot makes this read a fake
// registry (an INI file, keyed by the lookup's subKey/value) instead of the live hive — so tests (and CI)
// never touch the real registry. The hive (HKLM vs HKCU) is honoured only for the real read. Returns "" when
// the value is absent; otherwise the value itself (ExePath-style) or value+"/"+appendExe (InstallDir-style).
QString registryExe(const QString& regProbeRoot, const RegLookup& r)
{
    QString raw;
    if (!regProbeRoot.isEmpty()) {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        raw = ini.value(r.subKey + QLatin1Char('/') + r.value).toString();
    } else {
#ifdef Q_OS_WIN
        QSettings reg(r.hive + QLatin1Char('\\') + QString(r.subKey).replace(QLatin1Char('/'), QLatin1Char('\\')),
                      QSettings::NativeFormat);
        raw = reg.value(r.value).toString();
#else
        return QString();
#endif
    }
    if (raw.isEmpty()) return QString();
    return r.appendExe.isEmpty() ? raw : (raw + QLatin1Char('/') + r.appendExe);
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

        // Registry fallback for a non-default install location (real hive, or the injected fake). Each
        // candidate carries an ordered list of lookups (e.g. VLC's HKLM + WOW6432Node); the first that
        // resolves to an existing exe wins.
        if (hit.isEmpty()) {
            for (const RegLookup& r : c.regs) {
                const QString cand = registryExe(regProbeRoot, r);
                if (cand.isEmpty()) continue;
                const QFileInfo fi(cand);
                if (fi.exists() && fi.isFile()) { hit = fi.absoluteFilePath(); break; }
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

bool ExternalPlayer::anyTarget(const QString& fsProbeRoot, const QString& regProbeRoot)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(fsProbeRoot);
    Q_UNUSED(regProbeRoot);
    return true; // the ACTION_VIEW intent is always a possible handoff target
#else
    if (!Settings::externalPlayerPath().isEmpty()) return true;     // a Custom exe is configured
    return !detect(fsProbeRoot, regProbeRoot).isEmpty();            // or a player is installed/detected
#endif
}

QString ExternalPlayer::resolveForceTarget(const QString& fsProbeRoot, const QString& regProbeRoot)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(fsProbeRoot);
    Q_UNUSED(regProbeRoot);
    return QString();   // no exe on Android; launchExe() falls back to the intent handoff
#else
    const QVector<Detected> found = detect(fsProbeRoot, regProbeRoot);
    const Kind k = configuredKind();
    // 1. The configured external kind, if it resolves.
    if (k == Kind::Custom) {
        const QString custom = Settings::externalPlayerPath();
        if (!custom.isEmpty()) return custom;
    } else if (k == Kind::Vlc || k == Kind::Mpc) {
        for (const Detected& d : found) if (d.kind == k) return d.path;
    }
    // 2. A Custom path if one is set (regardless of the configured kind).
    const QString custom = Settings::externalPlayerPath();
    if (!custom.isEmpty()) return custom;
    // 3. Otherwise the first detected desktop player.
    if (!found.isEmpty()) return found.first().path;
    return QString();
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
    return launchExe(urlOrPath, configuredPath());
#endif
}

bool ExternalPlayer::launchExe(const QString& urlOrPath, const QString& exe)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(exe);
    return launch(urlOrPath); // Android has no exe target; the ACTION_VIEW intent is the handoff
#elif defined(Q_OS_IOS)
    // iOS: no QProcess and no exe handoff target — external players are unavailable (in-app libmpv only).
    Q_UNUSED(urlOrPath); Q_UNUSED(exe);
    return false;
#else
    if (exe.isEmpty()) return false;
    return QProcess::startDetached(exe, QStringList{ urlOrPath });
#endif
}
