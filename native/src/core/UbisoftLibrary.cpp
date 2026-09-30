#include "UbisoftLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>

namespace {

// The uninstall key prefix Ubisoft Connect writes for every game it installs ("Uplay Install 635"). The
// client's OWN uninstall row is named "Uplay" and has no id, so it never parses as a game.
const QString& uninstallPrefix()
{
    static const QString p = QStringLiteral("Uplay Install");
    return p;
}

// Registry values are Windows-style paths whatever the host (Ubisoft itself writes forward slashes into
// InstallDir, the uninstall InstallLocation carries backslashes). One spelling out: forward slashes, no
// trailing separator — except a bare drive root, which keeps it ("C:/" is a directory, "C:" is not).
QString normaliseDir(QString p)
{
    p = p.trimmed();
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
    {
        if (p.size() == 3 && p.at(1) == QLatin1Char(':')) break;   // "C:/"
        p.chop(1);
    }
    return p;
}

// The id a record describes, or empty when the record is not a Ubisoft game key at all.
QString idOf(const UbisoftRegRecord& r)
{
    QString id;
    if (r.source == UbisoftRegRecord::Installs)
        id = r.keyName.trimmed();
    else
    {
        const QString k = r.keyName.trimmed();
        if (!k.startsWith(uninstallPrefix(), Qt::CaseInsensitive)) return QString();
        id = k.mid(uninstallPrefix().size()).trimmed();
    }
    return UbisoftLibrary::isValidId(id) ? id : QString();
}

// One game while its records are merged.
struct Merged
{
    QString id;
    QString installDir;        // Installs\<id>\InstallDir
    QString displayName;
    QString installLocation;   // the uninstall fallback
};

#ifdef Q_OS_WIN
// Read one hive's children as records. `onlyPrefix` keeps an Uninstall walk to Ubisoft's own keys, so a
// machine's several hundred unrelated uninstall rows are never opened one by one.
void readHive(const QString& hive, UbisoftRegRecord::Source source, QVector<UbisoftRegRecord>& out)
{
    QSettings reg(hive, QSettings::NativeFormat);
    for (const QString& g : reg.childGroups())
    {
        if (source == UbisoftRegRecord::Uninstall
            && !g.trimmed().startsWith(uninstallPrefix(), Qt::CaseInsensitive)) continue;
        UbisoftRegRecord r;
        r.source  = source;
        r.keyName = g;
        reg.beginGroup(g);
        if (source == UbisoftRegRecord::Installs)
            r.installDir = reg.value(QStringLiteral("InstallDir")).toString();
        else
        {
            r.displayName     = reg.value(QStringLiteral("DisplayName")).toString();
            r.installLocation = reg.value(QStringLiteral("InstallLocation")).toString();
        }
        reg.endGroup();
        out.push_back(r);
    }
}
#endif

} // namespace

bool UbisoftLibrary::isValidId(const QString& id)
{
    if (id.isEmpty() || id.size() > 10) return false;
    for (const QChar c : id)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;   // ASCII digits only (not isDigit())
    return true;
}

QString UbisoftLibrary::launchUri(const QString& id)
{
    if (!isValidId(id)) return QString();
    return QStringLiteral("uplay://launch/") + id + QStringLiteral("/0");
}

QString UbisoftLibrary::idFrom(const QString& key, const QString& uri)
{
    static const QString keyPrefix = QStringLiteral("ubi:");
    static const QString uriPrefix = QStringLiteral("uplay://launch/");
    QString id;
    if (key.startsWith(keyPrefix))
        id = key.mid(keyPrefix.size());
    else if (uri.startsWith(uriPrefix, Qt::CaseInsensitive))
        id = uri.mid(uriPrefix.size()).section(QLatin1Char('/'), 0, 0).section(QLatin1Char('?'), 0, 0);
    return isValidId(id) ? id : QString();
}

QVector<UbisoftGame> UbisoftLibrary::parseSnapshot(const QVector<UbisoftRegRecord>& snapshot)
{
    // Merge by id, in first-seen order; the first non-empty value of each field wins.
    QVector<Merged>     merged;
    QHash<QString, int> byId;
    for (const UbisoftRegRecord& r : snapshot)
    {
        const QString id = idOf(r);
        if (id.isEmpty()) continue;   // not a Ubisoft game key, or an id we refuse to paste into a URL
        int idx = byId.value(id, -1);
        if (idx < 0)
        {
            Merged m; m.id = id;
            merged.push_back(m);
            idx = int(merged.size()) - 1;
            byId.insert(id, idx);
        }
        Merged& m = merged[idx];
        if (m.installDir.isEmpty())      m.installDir      = normaliseDir(r.installDir);
        if (m.displayName.isEmpty())     m.displayName     = r.displayName.trimmed();
        if (m.installLocation.isEmpty()) m.installLocation = normaliseDir(r.installLocation);
    }

    QVector<UbisoftGame> out;
    for (const Merged& m : merged)
    {
        // InstallDir is the authority; the uninstall InstallLocation only stands in when it is EMPTY. A stale
        // InstallDir is not rescued by a different location — that is Ubisoft saying the game is somewhere it
        // is not, and a tile for it would launch nothing.
        const QString dir = !m.installDir.isEmpty() ? m.installDir : m.installLocation;
        if (dir.isEmpty()) continue;                         // nowhere on disk: not installed
        if (!QFileInfo(dir).isDir()) continue;               // uninstalled / moved / drive gone: not installed
        UbisoftGame g;
        g.id         = m.id;
        g.installDir = dir;
        g.name       = m.displayName;
        if (g.name.isEmpty()) g.name = QFileInfo(dir).fileName();   // the install folder's own name
        if (g.name.isEmpty()) g.name = dir;                         // a drive root has no folder name
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const UbisoftGame& a, const UbisoftGame& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

QVector<UbisoftRegRecord> UbisoftLibrary::snapshotFromJson(const QByteArray& json)
{
    QVector<UbisoftRegRecord> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    const QJsonObject top = doc.object();
    for (const QJsonValue v : top.value(QStringLiteral("installs")).toArray())
    {
        const QJsonObject o = v.toObject();
        UbisoftRegRecord r;
        r.source     = UbisoftRegRecord::Installs;
        r.keyName    = o.value(QStringLiteral("key")).toString();
        r.installDir = o.value(QStringLiteral("InstallDir")).toString();
        out.push_back(r);
    }
    for (const QJsonValue v : top.value(QStringLiteral("uninstall")).toArray())
    {
        const QJsonObject o = v.toObject();
        UbisoftRegRecord r;
        r.source          = UbisoftRegRecord::Uninstall;
        r.keyName         = o.value(QStringLiteral("key")).toString();
        r.displayName     = o.value(QStringLiteral("DisplayName")).toString();
        r.installLocation = o.value(QStringLiteral("InstallLocation")).toString();
        out.push_back(r);
    }
    return out;
}

bool UbisoftLibrary::hasLiveReader()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QVector<UbisoftRegRecord> UbisoftLibrary::readRegistrySnapshot()
{
    QVector<UbisoftRegRecord> out;
#ifdef Q_OS_WIN
    // Read-only. The 32-bit (WOW6432Node) views are where the 32-bit Ubisoft Connect writes; the 64-bit views
    // are read as well, as Playnite does for Installs, and the parser folds any duplicate id into one game.
    static const char* const installs[] = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Ubisoft\\Launcher\\Installs",
    };
    static const char* const uninstall[] = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };
    for (const char* h : installs)  readHive(QString::fromLatin1(h), UbisoftRegRecord::Installs, out);
    for (const char* h : uninstall) readHive(QString::fromLatin1(h), UbisoftRegRecord::Uninstall, out);
#endif
    return out;
}

QVector<UbisoftRegRecord> UbisoftLibrary::currentSnapshot()
{
    // EB_UITEST_UBISOFT_FIXTURE (issue #60's live drive): a UI-test run reads a fixture snapshot instead of this
    // machine's registry, so a Ubisoft game can be shown, merged and dispatched with no Ubisoft Connect
    // installed. Honoured only with the test channel on (the #80 / #98 shape) — never in a normal run.
    if (qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_UITEST_UBISOFT_FIXTURE"))
    {
        QFile f(qEnvironmentVariable("EB_UITEST_UBISOFT_FIXTURE"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        return snapshotFromJson(f.readAll());
    }
    return readRegistrySnapshot();
}

QVector<UbisoftGame> UbisoftLibrary::installedGames()
{
    return parseSnapshot(currentSnapshot());
}

bool UbisoftLibrary::isAvailable()
{
    return !installedGames().isEmpty();
}
