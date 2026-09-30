#include "XboxLibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// A real AppxManifest.xml or MicrosoftGame.config is a few KiB, a .GamingRoot a few dozen bytes. Anything past
// these is not one, and is not read into memory.
constexpr qint64 kMaxManifestBytes   = 4 * 1024 * 1024;
constexpr qint64 kMaxGamingRootBytes = 64 * 1024;
// GameFinder refuses a .GamingRoot naming 255 or more folders; so does this.
constexpr quint32 kMaxGamingRootFolders = 254;
constexpr quint32 kGamingRootMagic = 0x58424752u;   // "RGBX" read as a little-endian uint32

// The Crockford base32 alphabet a publisher id is written in (0-9 a-z without i, l, o, u).
const char kPublisherIdAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";

const QString kShellPrefix = QStringLiteral("shell:AppsFolder\\");
const QString kKeyPrefix   = QStringLiteral("xbox:");

// One spelling out: forward slashes, no trailing separator — except a bare drive root, which keeps it ("C:/"
// is a directory, "C:" is not; "/" likewise).
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

bool isAsciiLetter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}
bool isAsciiDigit(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }

bool isMissingName(const QString& n)
{
    const QString t = n.trimmed();
    return t.isEmpty() || t.startsWith(QLatin1String("ms-resource:"), Qt::CaseInsensitive);
}

// The file `name` directly in `dir`, matched case-insensitively (the Xbox app writes "appxmanifest.xml", other
// installers "AppxManifest.xml", and a Linux probe's filesystem is case-sensitive). Empty when there is none.
QString joinPath(const QString& dir, const QString& name)
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

// The entry `name` directly in `dir` (a file, or with `wantDir` a folder), matched case-insensitively: the Xbox
// app writes "appxmanifest.xml", other installers "AppxManifest.xml", and a Linux probe's filesystem is
// case-sensitive. Empty when there is none.
QString findEntryCi(const QString& dir, const QString& name, bool wantDir = false)
{
    const QString exact = joinPath(dir, name);
    const QFileInfo fi(exact);
    if (wantDir ? fi.isDir() : fi.isFile()) return exact;
    const QDir::Filters kind = wantDir ? (QDir::Dirs | QDir::NoDotAndDotDot) : QDir::Files;
    const QStringList hits = QDir(dir).entryList({ name }, kind | QDir::Hidden);   // name filters ignore case
    return hits.isEmpty() ? QString() : joinPath(dir, hits.first());
}

QByteArray readCapped(const QString& path, qint64 cap)
{
    if (path.isEmpty()) return QByteArray();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    if (f.size() > cap) return QByteArray();
    return f.read(cap);
}

bool attrIsNone(const QXmlStreamAttributes& attrs, const QString& name)
{
    for (const QXmlStreamAttribute& a : attrs)
        if (a.name().compare(name, Qt::CaseInsensitive) == 0)
            return a.value().trimmed().compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
    return false;
}

QString attr(const QXmlStreamAttributes& attrs, const QString& name)
{
    for (const QXmlStreamAttribute& a : attrs)
        if (a.name().compare(name, Qt::CaseInsensitive) == 0) return a.value().toString();
    return QString();
}

// The game's own folder name: the install dir's, or its parent's when the package root is the Content\
// subfolder the Xbox app puts it in. Empty for a drive root.
QString folderName(const QString& installDir)
{
    const QFileInfo fi(installDir);
    const QString leaf = fi.fileName();
    if (leaf.compare(QLatin1String("Content"), Qt::CaseInsensitive) == 0)
        return QFileInfo(fi.path()).fileName();
    return leaf;
}

// A package folder's record from disk: the manifest in the folder or in its Content\ subfolder (GameFinder's
// order), MicrosoftGame.config beside it. No manifest: not a package (false).
bool recordFromFolder(const QString& folder, XboxPackageRecord& out)
{
    QString root = folder;
    QString manifest = findEntryCi(root, QStringLiteral("AppxManifest.xml"));
    if (manifest.isEmpty())
    {
        root = findEntryCi(folder, QStringLiteral("Content"), true);
        if (root.isEmpty()) return false;
        manifest = findEntryCi(root, QStringLiteral("AppxManifest.xml"));
        if (manifest.isEmpty()) return false;
    }
    const QByteArray appx = readCapped(manifest, kMaxManifestBytes);
    const QByteArray cfg  = readCapped(findEntryCi(root, QStringLiteral("MicrosoftGame.config")), kMaxManifestBytes);
    out = XboxLibrary::recordFromManifests(root, appx, cfg);
    return true;
}

struct Merged
{
    QString pfn;
    QString appId;
    QString installDir;
    QString displayName;
};

} // namespace

XboxLibrary::AppxManifest XboxLibrary::parseAppxManifest(const QByteArray& xml)
{
    AppxManifest m;
    if (xml.isEmpty()) return m;
    QXmlStreamReader r(xml);   // the encoding comes from the BOM / declaration: UTF-8 and UTF-16 both read
    QStringList path;          // local names of the open elements, root first
    bool rootIsPackage = false;
    while (!r.atEnd())
    {
        const QXmlStreamReader::TokenType t = r.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            const QString name = r.name().toString();
            if (path.isEmpty())
            {
                rootIsPackage = name.compare(QLatin1String("Package"), Qt::CaseInsensitive) == 0;
                if (!rootIsPackage) return AppxManifest();
            }
            else if (path.size() == 1 && name.compare(QLatin1String("Identity"), Qt::CaseInsensitive) == 0)
            {
                // Only Package/Identity: Dependencies and PackageDependency carry a Name attribute as well.
                m.name      = attr(r.attributes(), QStringLiteral("Name")).trimmed();
                m.publisher = attr(r.attributes(), QStringLiteral("Publisher")).trimmed();
            }
            else if (path.size() == 2 && name.compare(QLatin1String("DisplayName"), Qt::CaseInsensitive) == 0
                     && path.at(1).compare(QLatin1String("Properties"), Qt::CaseInsensitive) == 0)
            {
                m.displayName = r.readElementText().trimmed();
                continue;   // readElementText consumed the end element
            }
            else if (path.size() == 2 && name.compare(QLatin1String("Application"), Qt::CaseInsensitive) == 0
                     && path.at(1).compare(QLatin1String("Applications"), Qt::CaseInsensitive) == 0)
            {
                XboxApp a;
                a.id = attr(r.attributes(), QStringLiteral("Id")).trimmed();
                m.apps.push_back(a);
            }
            else if (path.size() >= 3 && name.compare(QLatin1String("VisualElements"), Qt::CaseInsensitive) == 0
                     && path.at(2).compare(QLatin1String("Application"), Qt::CaseInsensitive) == 0 && !m.apps.isEmpty())
            {
                // uap:VisualElements AppListEntry="none": the app is kept out of Start's list.
                if (attrIsNone(r.attributes(), QStringLiteral("AppListEntry"))) m.apps.last().listed = false;
            }
            path << name;
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            if (!path.isEmpty()) path.removeLast();
        }
    }
    if (r.hasError() || !rootIsPackage) return AppxManifest();   // truncated or malformed: nothing reliable
    m.ok = true;
    return m;
}

XboxLibrary::GameConfig XboxLibrary::parseGameConfig(const QByteArray& xml)
{
    GameConfig c;
    if (xml.isEmpty()) return c;
    QXmlStreamReader r(xml);
    QStringList path;
    bool rootIsGame = false;
    int executables = 0;
    QString displayName;
    while (!r.atEnd())
    {
        const QXmlStreamReader::TokenType t = r.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            const QString name = r.name().toString();
            if (path.isEmpty())
            {
                rootIsGame = name.compare(QLatin1String("Game"), Qt::CaseInsensitive) == 0;
                if (!rootIsGame) return GameConfig();
            }
            else if (path.size() == 2 && name.compare(QLatin1String("Executable"), Qt::CaseInsensitive) == 0
                     && path.at(1).compare(QLatin1String("ExecutableList"), Qt::CaseInsensitive) == 0)
                ++executables;
            else if (path.size() == 1 && name.compare(QLatin1String("ShellVisuals"), Qt::CaseInsensitive) == 0)
                displayName = attr(r.attributes(), QStringLiteral("DefaultDisplayName")).trimmed();
            path << name;
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            if (!path.isEmpty()) path.removeLast();
        }
    }
    if (r.hasError() || !rootIsGame) return GameConfig();
    c.ok          = true;
    c.isGame      = executables > 0;   // a DLC package's config has a <Game> root but nothing to execute
    c.displayName = displayName;
    return c;
}

QString XboxLibrary::publisherId(const QString& publisher)
{
    if (publisher.isEmpty()) return QString();
    // SHA-256 over the publisher string's UTF-16LE bytes; the first 8 bytes, as a big-endian 64-bit number,
    // with one zero bit appended (65 bits), read as 13 five-bit groups from the top.
    QByteArray utf16le;
    utf16le.reserve(publisher.size() * 2);
    for (const QChar ch : publisher)
    {
        const ushort u = ch.unicode();
        utf16le.append(char(u & 0xFF));
        utf16le.append(char((u >> 8) & 0xFF));
    }
    const QByteArray h = QCryptographicHash::hash(utf16le, QCryptographicHash::Sha256);
    quint64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | quint8(h.at(i));
    QString out;
    out.reserve(13);
    for (int i = 0; i < 13; ++i)
    {
        // Group i covers bits [64 - 5i .. 60 - 5i] of the 65-bit value v << 1.
        const int shift = 60 - 5 * i;   // of v<<1, i.e. shift-1 of v
        quint64 group;
        if (shift >= 1) group = (v >> (shift - 1)) & 0x1F;
        else            group = (v << 1) & 0x1F;   // the last group: v's low 4 bits and the appended zero
        out.append(QLatin1Char(kPublisherIdAlphabet[group]));
    }
    return out;
}

QString XboxLibrary::familyName(const QString& name, const QString& publisher)
{
    const QString pid = publisherId(publisher);
    if (pid.isEmpty()) return QString();
    const QString pfn = name.trimmed() + QLatin1Char('_') + pid;
    return isValidPfn(pfn) ? pfn : QString();
}

bool XboxLibrary::isValidPfn(const QString& pfn)
{
    const int us = int(pfn.lastIndexOf(QLatin1Char('_')));
    if (us < 0) return false;
    const QString name = pfn.left(us);
    const QString pid  = pfn.mid(us + 1);
    if (name.size() < 3 || name.size() > 50) return false;
    for (const QChar c : name)
        if (!isAsciiLetter(c) && !isAsciiDigit(c) && c != QLatin1Char('.') && c != QLatin1Char('-')) return false;
    if (pid.size() != 13) return false;
    for (const QChar c : pid)
    {
        const ushort u = c.unicode();
        if (u > 127 || !std::strchr(kPublisherIdAlphabet, char(u)) || u == 0) return false;
    }
    return true;
}

bool XboxLibrary::isValidAppId(const QString& appId)
{
    if (appId.isEmpty() || appId.size() > 64) return false;
    bool partStart = true;
    for (const QChar c : appId)
    {
        if (c == QLatin1Char('.'))
        {
            if (partStart) return false;   // a leading dot, or two in a row
            partStart = true;
            continue;
        }
        if (partStart) { if (!isAsciiLetter(c)) return false; partStart = false; continue; }
        if (!isAsciiLetter(c) && !isAsciiDigit(c)) return false;
    }
    return !partStart;   // not ending on a dot
}

QString XboxLibrary::launchUri(const QString& aumid)
{
    const QStringList parts = aumid.split(QLatin1Char('!'));
    if (parts.size() != 2 || !isValidPfn(parts.at(0)) || !isValidAppId(parts.at(1))) return QString();
    return kShellPrefix + aumid;
}

QString XboxLibrary::idFrom(const QString& key, const QString& uri)
{
    QString id;
    if (key.startsWith(kKeyPrefix))
        id = key.mid(kKeyPrefix.size());
    else if (uri.startsWith(kShellPrefix, Qt::CaseInsensitive))
        id = uri.mid(kShellPrefix.size());
    return launchUri(id).isEmpty() ? QString() : id;
}

QStringList XboxLibrary::parseGamingRoot(const QByteArray& bytes)
{
    auto u32 = [&bytes](int at) {
        return quint32(quint8(bytes.at(at))) | (quint32(quint8(bytes.at(at + 1))) << 8)
             | (quint32(quint8(bytes.at(at + 2))) << 16) | (quint32(quint8(bytes.at(at + 3))) << 24);
    };
    if (bytes.size() < 8 || u32(0) != kGamingRootMagic) return {};
    const quint32 count = u32(4);
    if (count == 0 || count > kMaxGamingRootFolders) return {};
    QStringList out;
    int at = 8;
    for (quint32 i = 0; i < count; ++i)
    {
        QString folder;
        bool terminated = false;
        while (at + 1 < bytes.size())
        {
            const ushort u = ushort(quint8(bytes.at(at))) | ushort(ushort(quint8(bytes.at(at + 1))) << 8);
            at += 2;
            if (u == 0) { terminated = true; break; }
            folder.append(QChar(u));
        }
        if (!terminated) return {};   // truncated: the file says nothing reliable
        out << folder;
    }
    return out;
}

XboxPackageRecord XboxLibrary::recordFromManifests(const QString& installDir, const QByteArray& appxManifest,
                                                   const QByteArray& gameConfig)
{
    XboxPackageRecord rec;
    rec.installDir = installDir;
    const AppxManifest m = parseAppxManifest(appxManifest);
    if (!m.ok) return rec;   // no PFN: parseSnapshot drops it
    rec.pfn  = familyName(m.name, m.publisher);
    rec.apps = m.apps;
    const GameConfig c = parseGameConfig(gameConfig);
    rec.isGame = c.ok && c.isGame;
    rec.displayName = !isMissingName(m.displayName) ? m.displayName
                    : !isMissingName(c.displayName) ? c.displayName : QString();
    return rec;
}

QVector<XboxGame> XboxLibrary::parseSnapshot(const QVector<XboxPackageRecord>& snapshot)
{
    QVector<Merged>     merged;
    QHash<QString, int> byPfn;
    for (const XboxPackageRecord& r : snapshot)
    {
        if (!r.isGame) continue;                        // a Store app, a DLC: not something the folder lists
        const QString dir = normaliseDir(r.installDir);
        if (dir.isEmpty()) continue;                    // nowhere on disk: not installed
        if (!QFileInfo(dir).isDir()) continue;          // uninstalled / moved / drive gone: not installed
        const QString pfn = r.pfn.trimmed();
        if (!isValidPfn(pfn)) continue;                 // odd PFNs are refused, never cleaned
        // The AppId: the first LISTED app with a valid id, else the first valid one at all.
        QString appId;
        for (const XboxApp& a : r.apps)
            if (a.listed && isValidAppId(a.id)) { appId = a.id; break; }
        if (appId.isEmpty())
            for (const XboxApp& a : r.apps)
                if (isValidAppId(a.id)) { appId = a.id; break; }
        if (appId.isEmpty()) continue;                  // nothing to launch it by

        int idx = byPfn.value(pfn, -1);
        if (idx < 0)
        {
            Merged m; m.pfn = pfn; m.appId = appId; m.installDir = dir;
            merged.push_back(m);
            idx = int(merged.size()) - 1;
            byPfn.insert(pfn, idx);
        }
        Merged& m = merged[idx];
        if (m.displayName.isEmpty() && !isMissingName(r.displayName)) m.displayName = r.displayName.trimmed();
    }

    QVector<XboxGame> out;
    for (const Merged& m : merged)
    {
        XboxGame g;
        g.pfn        = m.pfn;
        g.appId      = m.appId;
        g.id         = m.pfn + QLatin1Char('!') + m.appId;
        g.installDir = m.installDir;
        g.name       = m.displayName;
        if (g.name.isEmpty()) g.name = folderName(m.installDir);
        if (g.name.isEmpty()) g.name = m.pfn;   // a package at a drive root has no folder name
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const XboxGame& a, const XboxGame& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

QStringList XboxLibrary::libraryFoldersForRoots(const QStringList& driveRoots)
{
    QStringList out;
    auto add = [&out](const QString& p) {
        const QString n = normaliseDir(p);
        if (!n.isEmpty() && QFileInfo(n).isDir() && !out.contains(n, Qt::CaseInsensitive)) out << n;
    };
    for (const QString& rootIn : driveRoots)
    {
        QString root = normaliseDir(rootIn);
        if (root.isEmpty() || !QFileInfo(root).isDir()) continue;
        const QString base = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        const QByteArray gr = readCapped(findEntryCi(root, QStringLiteral(".GamingRoot")), kMaxGamingRootBytes);
        for (QString rel : parseGamingRoot(gr))
        {
            rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
            // A folder is RELATIVE to its drive root; one that climbs out of it is refused.
            if (rel.isEmpty() || rel.split(QLatin1Char('/')).contains(QStringLiteral(".."))) continue;
            if (rel.size() > 1 && rel.at(1) == QLatin1Char(':')) continue;   // an absolute path: not this drive's
            add(base + rel);
        }
        add(base + QStringLiteral("Program Files/ModifiableWindowsApps"));
    }
    return out;
}

QVector<XboxPackageRecord> XboxLibrary::gatherSnapshot(const QStringList& libraryFolders)
{
    QVector<XboxPackageRecord> out;
    for (const QString& lf : libraryFolders)
    {
        const QString l = normaliseDir(lf);
        if (l.isEmpty()) continue;
        const QDir d(l);
        if (!d.exists()) continue;
        const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        for (const QString& s : subs)
        {
            XboxPackageRecord rec;
            if (recordFromFolder(l + QLatin1Char('/') + s, rec)) out.push_back(rec);
        }
    }
    return out;
}

QStringList XboxLibrary::driveRootsFromJson(const QByteArray& json)
{
    QStringList out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    for (const QJsonValue v : doc.object().value(QStringLiteral("driveRoots")).toArray())
        if (v.isString()) out << v.toString();
    return out;
}

bool XboxLibrary::hasLiveReader()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QVector<XboxPackageRecord> XboxLibrary::readLiveSnapshot()
{
#ifdef Q_OS_WIN
    // Read-only, no admin, no package API: each local drive's .GamingRoot (and its ModifiableWindowsApps
    // folder), then the manifests on disk. Only fixed and removable drives are looked at — the Xbox app installs
    // to neither a network share nor an optical disc, and touching a disconnected share can stall.
    QStringList roots;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(mask & (DWORD(1) << i))) continue;
        const QString root = QString(QLatin1Char(char('A' + i))) + QStringLiteral(":/");
        const std::wstring w = QString(root).replace(QLatin1Char('/'), QLatin1Char('\\')).toStdWString();
        const UINT type = GetDriveTypeW(w.c_str());
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) roots << root;
    }
    return gatherSnapshot(libraryFoldersForRoots(roots));
#else
    return {};
#endif
}

QVector<XboxPackageRecord> XboxLibrary::currentSnapshot()
{
    // EB_UITEST_XBOX_FIXTURE (issue #60's live drive): a UI-test run reads the drive roots a fixture names
    // instead of this machine's, then gathers their .GamingRoot files and manifests from disk exactly as the
    // live reader does — so an Xbox game can be shown, merged and dispatched with no Xbox app installed.
    // Honoured only with the test channel on (the #80 / #98 shape) — never in a normal run.
    if (qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_UITEST_XBOX_FIXTURE"))
    {
        QFile f(qEnvironmentVariable("EB_UITEST_XBOX_FIXTURE"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        return gatherSnapshot(libraryFoldersForRoots(driveRootsFromJson(f.readAll())));
    }
    return readLiveSnapshot();
}

QVector<XboxGame> XboxLibrary::installedGames()
{
    return parseSnapshot(currentSnapshot());
}

bool XboxLibrary::isAvailable()
{
    return !installedGames().isEmpty();
}
