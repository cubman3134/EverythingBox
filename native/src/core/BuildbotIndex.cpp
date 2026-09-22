#include "BuildbotIndex.h"
#include "SystemCatalog.h"

#include <QFileInfo>

// ---- the platform's buildbot naming ----------------------------------------------------------------------
// Moved here from CoreManager.cpp (which now calls these) so the browser and the catalogue download share ONE
// definition of where this platform's cores live and what they are called. NB: Android is also Q_OS_LINUX, so it
// must be tested first.
QString BuildbotIndex::subpath()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows/x86_64/latest/");
#elif defined(Q_OS_MACOS)
  #if defined(Q_PROCESSOR_ARM)
    return QStringLiteral("apple/osx/arm64/latest/");
  #else
    return QStringLiteral("apple/osx/x86_64/latest/");
  #endif
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("android/latest/arm64-v8a/");
#else
    return QStringLiteral("linux/x86_64/latest/");
#endif
}

QString BuildbotIndex::libExt()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dylib");
#else
    return QStringLiteral(".so"); // Linux + Android
#endif
}

QString BuildbotIndex::coreFileTail()
{
#if defined(Q_OS_ANDROID)
    return QStringLiteral("_libretro_android.so");
#else
    return QStringLiteral("_libretro") + libExt();
#endif
}

QString BuildbotIndex::coreFileName(const QString& coreName)
{
    return coreName + coreFileTail();
}

// ---- the name rule ---------------------------------------------------------------------------------------

bool BuildbotIndex::isValidCoreName(const QString& name)
{
    if (name.isEmpty() || name.size() > 64) return false;
    for (int i = 0; i < name.size(); ++i)
    {
        const ushort u = name.at(i).unicode();
        const bool alnum = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
        if (i == 0 && !alnum) return false;            // no leading '-' / '_' / '+'
        if (!alnum && u != '_' && u != '-' && u != '+') return false;
    }
    return true;
}

QString BuildbotIndex::coreNameFromZip(const QString& zipFile, const QString& tail)
{
    const QString zipTail = tail + QStringLiteral(".zip");
    if (tail.isEmpty() || !zipFile.endsWith(zipTail)) return QString();   // case-sensitive: the index's own spelling
    const QString name = zipFile.left(zipFile.size() - zipTail.size());
    return isValidCoreName(name) ? name : QString();
}

// ---- the parser ------------------------------------------------------------------------------------------

BuildbotIndex::Parsed BuildbotIndex::parse(const QByteArray& body)
{
    return parseWithTail(body, coreFileTail());
}

BuildbotIndex::Parsed BuildbotIndex::parseWithTail(const QByteArray& body, const QString& tail)
{
    Parsed out;
    if (body.size() > kMaxIndexBytes)
    {
        out.tooLarge = true;          // refused whole: nothing of an over-cap body is trusted
        return out;
    }

    const QList<QByteArray> lines = body.split('\n');
    for (const QByteArray& raw : lines)
    {
        const QByteArray line = raw.trimmed();      // tolerates CRLF and trailing blanks
        if (line.isEmpty()) continue;                // blank lines are not lines

        const QList<QByteArray> f = line.simplified().split(' ');
        if (f.size() != 3 && f.size() != 4) { ++out.malformed; continue; }

        const QDate date = QDate::fromString(QString::fromLatin1(f[0]), QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid()) { ++out.malformed; continue; }

        qint64 size = -1;
        if (f.size() == 4)
        {
            bool ok = false;
            size = f[3].toLongLong(&ok);
            if (!ok || size < 0) { ++out.malformed; continue; }
        }

        // The file field is UTF-8 on the wire; anything outside the name rule is refused below anyway.
        const QString file = QString::fromUtf8(f[2]);
        const QString name = coreNameFromZip(file, tail);
        if (name.isEmpty()) { ++out.refused; continue; }

        if (out.entries.size() >= kMaxEntries) { ++out.dropped; continue; }

        Entry e;
        e.name     = name;
        e.file     = file;
        e.coreFile = name + tail;
        e.date     = date;
        const QString crc = QString::fromLatin1(f[1]).toLower();
        e.crc      = crc == QLatin1String("-") ? QString() : crc;
        e.size     = size;
        out.entries.push_back(e);
    }
    return out;
}

// ---- classification --------------------------------------------------------------------------------------

QSet<QString> BuildbotIndex::catalogueCoreNames()
{
    QSet<QString> out;
    for (const GameSystem& sys : SystemCatalog::systems())
        for (const QString& c : sys.cores)
            out.insert(c);
    return out;
}

bool BuildbotIndex::updateAvailable(const CustomCore& core, const Entry& e)
{
    if (core.source != CustomCores::sourceBuildbot()) return false;   // loaded by hand: nothing to compare with
    if (core.sourceFile != e.file) return false;
    const QDate had = QDate::fromString(core.sourceDate, QStringLiteral("yyyy-MM-dd"));
    if (!had.isValid() || !e.date.isValid()) return false;
    return e.date > had;
}

namespace {
bool sameFileName(const QString& a, const QString& b)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return a.compare(b, Qt::CaseInsensitive) == 0;    // the platforms whose file systems fold case
#else
    return a == b;
#endif
}
} // namespace

QList<BuildbotIndex::Row> BuildbotIndex::classify(const QList<Entry>& entries, const QSet<QString>& catalogue,
                                                  const QList<CustomCore>& customs)
{
    QList<Row> out;
    out.reserve(entries.size());
    for (const Entry& e : entries)
    {
        Row r;
        r.entry = e;
        if (catalogue.contains(e.name))
        {
            r.status = Status::Catalogue;      // never offered here: it installs the normal way
            out.push_back(r);
            continue;
        }
        // A buildbot-sourced record for this very file first (it is the one an update is measured against),
        // then any custom core whose installed file is the file this entry produces (loaded by hand).
        const CustomCore* match = nullptr;
        for (const CustomCore& c : customs)
            if (c.source == CustomCores::sourceBuildbot() && c.sourceFile == e.file) { match = &c; break; }
        if (!match)
            for (const CustomCore& c : customs)
                if (sameFileName(QFileInfo(c.path).fileName(), e.coreFile)) { match = &c; break; }
        if (match)
        {
            r.status          = Status::InstalledCustom;
            r.customId        = match->id;
            r.fromBuildbot    = match->source == CustomCores::sourceBuildbot() && match->sourceFile == e.file;
            r.updateAvailable = updateAvailable(*match, e);
        }
        out.push_back(r);
    }
    return out;
}

QList<BuildbotIndex::Row> BuildbotIndex::browsable(const QList<Row>& rows, const QString& query)
{
    const QString q = query.trimmed();
    QList<Row> out;
    for (const Row& r : rows)
    {
        if (r.status == Status::Catalogue) continue;
        if (!q.isEmpty() && !r.entry.name.contains(q, Qt::CaseInsensitive)) continue;
        out.push_back(r);
    }
    return out;
}

// ---- where the browser may fetch from --------------------------------------------------------------------

QString BuildbotIndex::host() { return QStringLiteral("buildbot.libretro.com"); }

QUrl BuildbotIndex::productionBase() { return QUrl(QStringLiteral("https://buildbot.libretro.com/nightly/")); }

bool BuildbotIndex::isAllowedUrl(const QUrl& url)
{
    if (!url.isValid()) return false;
    if (url.scheme() != QLatin1String("https")) return false;           // never http, never file:, never anything else
    if (url.host() != host()) return false;                             // exact: no suffix/prefix games, no IP
    if (url.port(-1) != -1 && url.port() != 443) return false;          // the default https port only
    if (!url.userInfo().isEmpty()) return false;                        // no credentials riding in the URL
    return true;
}

QUrl BuildbotIndex::indexUrl(const QUrl& base)
{
    return base.resolved(QUrl(subpath() + QStringLiteral(".index-extended")));
}

QUrl BuildbotIndex::zipUrl(const QUrl& base, const Entry& e)
{
    // e.file passed the name rule on the way in (no '/', no '..', no ':'), so it cannot leave the directory.
    return base.resolved(QUrl(subpath() + e.file));
}

QString BuildbotIndex::catalogueZipUrl(const QString& coreName)
{
    if (coreName.isEmpty() || !CustomCores::mayDownload(coreName)) return QString();
    return productionBase().toString() + subpath() + coreFileName(coreName) + QStringLiteral(".zip");
}
