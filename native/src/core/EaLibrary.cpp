#include "EaLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <algorithm>

namespace {

// A real installerdata.xml is a few KiB. Anything past this is not one, and is not read into memory.
constexpr qint64 kMaxInstallerDataBytes = 4 * 1024 * 1024;

// Registry values and fixture paths are Windows-style whatever the host. One spelling out: forward slashes,
// no trailing separator — except a bare drive root, which keeps it ("C:/" is a directory, "C:" is not).
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

bool isAsciiAlnum(QChar c)
{
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

// Read <dir>/__Installer/installerdata.xml, capped. Empty when there is none (or it is not a plausible one).
QByteArray readInstallerData(const QString& dir)
{
    if (dir.isEmpty()) return QByteArray();
    QFile f(dir + QStringLiteral("/__Installer/installerdata.xml"));
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    if (f.size() > kMaxInstallerDataBytes) return QByteArray();
    return f.read(kMaxInstallerDataBytes);
}

// One game while its records are merged.
struct Merged
{
    QString id;
    QString installDir;
    QString title;         // the manifest's
    QString displayName;   // the Uninstall entry's
};

#ifdef Q_OS_WIN
// Every Uninstall entry of one registry view, as records. Each subkey is opened (the EA ones are GUID-named,
// so the key name alone says nothing); whether it is an EA game's is the parser's call, on its UninstallString.
void readUninstallView(const QString& hive, QVector<EaInstallRecord>& out)
{
    QSettings reg(hive, QSettings::NativeFormat);
    for (const QString& g : reg.childGroups())
    {
        reg.beginGroup(g);
        const QString un = reg.value(QStringLiteral("UninstallString")).toString();
        if (EaLibrary::isEaUninstallString(un))
        {
            EaInstallRecord r;
            r.source          = EaInstallRecord::Uninstall;
            r.keyName         = g;
            r.uninstallString = un;
            r.displayName     = reg.value(QStringLiteral("DisplayName")).toString();
            r.installDir      = reg.value(QStringLiteral("InstallLocation")).toString();
            out.push_back(r);
        }
        reg.endGroup();
    }
}
#endif

} // namespace

EaLibrary::InstallerData EaLibrary::parseInstallerData(const QByteArray& xml)
{
    InstallerData d;
    if (xml.isEmpty()) return d;
    QXmlStreamReader r(xml);   // the encoding comes from the BOM / declaration: UTF-8 and UTF-16 both read
    QString enTitle, firstTitle;
    QString localeInfoLocale;  // the enclosing <localeInfo locale="…"> of a legacy <title>
    bool inContentIds = false, inLocaleInfo = false;
    while (!r.atEnd())
    {
        const QXmlStreamReader::TokenType t = r.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            const QString name = r.name().toString();
            if (name.compare(QLatin1String("contentIDs"), Qt::CaseInsensitive) == 0) inContentIds = true;
            else if (name.compare(QLatin1String("localeInfo"), Qt::CaseInsensitive) == 0)
            {
                inLocaleInfo = true;
                localeInfoLocale = r.attributes().value(QLatin1String("locale")).toString();
            }
            else if (inContentIds && name.compare(QLatin1String("contentID"), Qt::CaseInsensitive) == 0)
            {
                const QString id = r.readElementText().trimmed();
                if (!id.isEmpty()) d.ids << id;
            }
            else if (name.compare(QLatin1String("gameTitle"), Qt::CaseInsensitive) == 0
                     || (inLocaleInfo && name.compare(QLatin1String("title"), Qt::CaseInsensitive) == 0))
            {
                const bool legacy = name.compare(QLatin1String("title"), Qt::CaseInsensitive) == 0;
                const QString locale = legacy ? localeInfoLocale
                                              : r.attributes().value(QLatin1String("locale")).toString();
                const QString text = r.readElementText().trimmed();
                if (text.isEmpty()) continue;
                if (firstTitle.isEmpty()) firstTitle = text;
                if (enTitle.isEmpty() && locale.compare(QLatin1String("en_US"), Qt::CaseInsensitive) == 0)
                    enTitle = text;
            }
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            const QString name = r.name().toString();
            if (name.compare(QLatin1String("contentIDs"), Qt::CaseInsensitive) == 0) inContentIds = false;
            else if (name.compare(QLatin1String("localeInfo"), Qt::CaseInsensitive) == 0) inLocaleInfo = false;
        }
    }
    if (r.hasError()) return InstallerData();   // a truncated or malformed manifest says nothing reliable
    d.ok    = true;
    d.title = !enTitle.isEmpty() ? enTitle : firstTitle;
    return d;
}

bool EaLibrary::isValidId(const QString& id)
{
    if (id.isEmpty() || id.size() > 64) return false;
    if (!isAsciiAlnum(id.at(0))) return false;
    for (const QChar c : id)
        if (!isAsciiAlnum(c) && c != QLatin1Char('.') && c != QLatin1Char(':') && c != QLatin1Char('-')
            && c != QLatin1Char('_'))
            return false;
    return true;
}

bool EaLibrary::isEaUninstallString(const QString& uninstallString)
{
    return uninstallString.contains(QLatin1String("EAInstaller"), Qt::CaseInsensitive)
        || uninstallString.contains(QLatin1String("uninstall_game"), Qt::CaseInsensitive);
}

QString EaLibrary::launchUri(const QString& id)
{
    if (!isValidId(id)) return QString();
    return QStringLiteral("origin2://game/launch?offerIds=") + id + QStringLiteral("&autoDownload=1");
}

QString EaLibrary::idFrom(const QString& key, const QString& uri)
{
    static const QString keyPrefix = QStringLiteral("ea:");
    QString id;
    if (key.startsWith(keyPrefix))
        id = key.mid(keyPrefix.size());
    else
    {
        const QUrl u(uri);
        if (u.scheme().compare(QLatin1String("origin2"), Qt::CaseInsensitive) == 0
            && u.host().compare(QLatin1String("game"), Qt::CaseInsensitive) == 0
            && u.path().compare(QLatin1String("/launch"), Qt::CaseInsensitive) == 0)
        {
            const QString ids = QUrlQuery(u).queryItemValue(QStringLiteral("offerIds"), QUrl::FullyDecoded);
            id = ids.section(QLatin1Char(','), 0, 0).trimmed();
        }
    }
    return isValidId(id) ? id : QString();
}

QVector<EaGame> EaLibrary::parseSnapshot(const QVector<EaInstallRecord>& snapshot)
{
    // Merge by id, in first-seen order, over INSTALLED records only; the first value of each field wins.
    QVector<Merged>     merged;
    QHash<QString, int> byId;
    for (const EaInstallRecord& r : snapshot)
    {
        if (r.source == EaInstallRecord::Uninstall && !isEaUninstallString(r.uninstallString)) continue;
        const QString dir = normaliseDir(r.installDir);
        if (dir.isEmpty()) continue;                    // nowhere on disk: not installed
        if (!QFileInfo(dir).isDir()) continue;          // uninstalled / moved / drive gone: not installed
        const InstallerData d = parseInstallerData(r.installerData);
        if (!d.ok) continue;                            // no manifest: nothing to launch it by
        QString id;
        for (const QString& c : d.ids)
            if (isValidId(c)) { id = c; break; }         // the first VALID content id; odd ones are never cleaned
        if (id.isEmpty()) continue;

        int idx = byId.value(id, -1);
        if (idx < 0)
        {
            Merged m; m.id = id; m.installDir = dir;
            merged.push_back(m);
            idx = int(merged.size()) - 1;
            byId.insert(id, idx);
        }
        Merged& m = merged[idx];
        if (m.title.isEmpty())       m.title       = d.title.trimmed();
        if (m.displayName.isEmpty()) m.displayName = r.displayName.trimmed();
    }

    QVector<EaGame> out;
    for (const Merged& m : merged)
    {
        EaGame g;
        g.id         = m.id;
        g.installDir = m.installDir;
        g.name       = !m.title.isEmpty() ? m.title : m.displayName;
        if (g.name.isEmpty()) g.name = QFileInfo(m.installDir).fileName();   // the install folder's own name
        if (g.name.isEmpty()) g.name = m.installDir;                         // a drive root has no folder name
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const EaGame& a, const EaGame& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

QVector<EaInstallRecord> EaLibrary::gatherSnapshot(const QVector<EaInstallRecord>& uninstall,
                                                    const QStringList& libraryRoots)
{
    QVector<EaInstallRecord> out = uninstall;
    for (const QString& root : libraryRoots)
    {
        const QString r = normaliseDir(root);
        if (r.isEmpty()) continue;
        const QDir d(r);
        if (!d.exists()) continue;
        const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        for (const QString& s : subs)
        {
            EaInstallRecord rec;
            rec.source     = EaInstallRecord::LibraryFolder;
            rec.keyName    = s;
            rec.installDir = r + QLatin1Char('/') + s;
            out.push_back(rec);
        }
    }
    for (EaInstallRecord& rec : out)
        if (rec.installerData.isEmpty())
            rec.installerData = readInstallerData(normaliseDir(rec.installDir));
    return out;
}

QVector<EaInstallRecord> EaLibrary::uninstallFromJson(const QByteArray& json)
{
    QVector<EaInstallRecord> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    for (const QJsonValue v : doc.object().value(QStringLiteral("uninstall")).toArray())
    {
        const QJsonObject o = v.toObject();
        EaInstallRecord r;
        r.source          = EaInstallRecord::Uninstall;
        r.keyName         = o.value(QStringLiteral("key")).toString();
        r.displayName     = o.value(QStringLiteral("DisplayName")).toString();
        r.installDir      = o.value(QStringLiteral("InstallLocation")).toString();
        r.uninstallString = o.value(QStringLiteral("UninstallString")).toString();
        out.push_back(r);
    }
    return out;
}

QStringList EaLibrary::libraryRootsFromJson(const QByteArray& json)
{
    QStringList out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    for (const QJsonValue v : doc.object().value(QStringLiteral("libraryRoots")).toArray())
        if (v.isString()) out << v.toString();
    return out;
}

bool EaLibrary::hasLiveReader()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QVector<EaInstallRecord> EaLibrary::readLiveSnapshot()
{
#ifdef Q_OS_WIN
    // Read-only. The EA installer is 32-bit and writes the WOW6432Node view; the 64-bit view is read as well
    // and the parser folds any duplicate id into one game.
    QVector<EaInstallRecord> uninstall;
    readUninstallView(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
                      uninstall);
    readUninstallView(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
                      uninstall);
    // The EA app's default library folder (Lutris; Steam ROM Manager defaults to the x86 one). A game moved
    // anywhere else is still found through its Uninstall entry above.
    QStringList roots;
    for (const char* env : { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" })
    {
        const QString pf = qEnvironmentVariable(env);
        if (pf.isEmpty()) continue;
        const QString root = normaliseDir(pf) + QStringLiteral("/EA Games");
        if (!roots.contains(root, Qt::CaseInsensitive)) roots << root;
    }
    return gatherSnapshot(uninstall, roots);
#else
    return {};
#endif
}

QVector<EaInstallRecord> EaLibrary::currentSnapshot()
{
    // EB_UITEST_EA_FIXTURE (issue #60's live drive): a UI-test run reads a fixture's Uninstall entries and
    // library roots instead of this machine's, then gathers their manifests from disk exactly as the live
    // reader does — so an EA game can be shown, merged and dispatched with no EA app installed. Honoured only
    // with the test channel on (the #80 / #98 shape) — never in a normal run.
    if (qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_UITEST_EA_FIXTURE"))
    {
        QFile f(qEnvironmentVariable("EB_UITEST_EA_FIXTURE"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QByteArray json = f.readAll();
        return gatherSnapshot(uninstallFromJson(json), libraryRootsFromJson(json));
    }
    return readLiveSnapshot();
}

QVector<EaGame> EaLibrary::installedGames()
{
    return parseSnapshot(currentSnapshot());
}

bool EaLibrary::isAvailable()
{
    return !installedGames().isEmpty();
}
