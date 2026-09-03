// The RetComM catalogue feed — parse, merge, cache. See RecompFeed.h for what this is and why it is a
// second feed rather than a replacement. Everything in this file is QtCore + miniz (portable C), so
// probe_ports drives all of it headlessly.
#include "RecompFeed.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cstring>

#include "AppBrand.h"
#include "AppPaths.h"
#include "NativePorts.h"

#include "miniz.h"

namespace {

// The portable everythingbox.ini, same posture as HashVerify/Settings.
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

constexpr qint64 kRefreshIntervalSecs = 24 * 60 * 60;

// A catalogue member's bytes are read whole into memory, so the same reasoning that bounds the download
// bounds the unpack: a 28 KB zip that declares a 4 GB member is a zip bomb, and the ceiling has to be applied
// to the DECLARED size before anything is allocated for it.
constexpr qint64 kMaxMemberBytes = 1 * 1024 * 1024;
constexpr int    kMaxMembers     = 4096;

} // namespace

namespace RecompFeed {

QString catalogUrl()
{
    // The override exists so a live drive can point the feed at a local fixture and never at the real
    // repository — and so a mirrored machine can be pointed at its mirror. Read every call rather than cached:
    // a cached one could not be changed for a second drive in the same session.
    const QByteArray env = qgetenv("EB_RECOMM_CATALOG_URL");
    if (!env.trimmed().isEmpty()) return QString::fromUtf8(env).trimmed();
    return QStringLiteral(
        "https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip");
}

QString cacheDir()           { return AppPaths::dataDir() + QStringLiteral("/recomps"); }
QString cachedCatalogPath()  { return cacheDir() + QStringLiteral("/catalog.zip"); }

// ---- the engines -------------------------------------------------------------------------------------------
Engine engineInfo(const QString& engineId)
{
    const QString id = engineId.trimmed().toLower();
    Engine e;
    e.id = id;
    if (id.isEmpty()) return e;
    // PolyForm Noncommercial 1.0.0 for all three, read off each project's own LICENSE file. psxrecomp is the
    // one #248 called out by name: it may be INVOKED on a user's machine and never bundled or redistributed,
    // which is exactly why the row has to say so before anybody asks for a build.
    if (id == QStringLiteral("psxrecomp"))
    {
        e.license  = QStringLiteral("PolyForm Noncommercial 1.0.0");
        e.homepage = QStringLiteral("https://github.com/TechnicallyComputers/psxrecomp");
    }
    else if (id == QStringLiteral("snesrecomp"))
    {
        e.license  = QStringLiteral("PolyForm Noncommercial 1.0.0");
        e.homepage = QStringLiteral("https://github.com/mstan/snesrecomp");
    }
    else if (id == QStringLiteral("gbarecomp"))
    {
        e.license  = QStringLiteral("PolyForm Noncommercial 1.0.0");
        e.homepage = QStringLiteral("https://github.com/TechnicallyComputers/gbarecomp");
    }
    // An engine this build has not checked keeps an empty licence and an empty homepage. A guess about
    // somebody else's terms is worse than saying nothing, and the row is built to show nothing.
    return e;
}

// ---- unpack -------------------------------------------------------------------------------------------------
QHash<QString, QByteArray> unpack(const QByteArray& zipBytes, QString* error)
{
    QHash<QString, QByteArray> out;
    auto fail = [&](const QString& m) { if (error) *error = m; return QHash<QString, QByteArray>(); };
    if (zipBytes.isEmpty()) return fail(QStringLiteral("the catalogue download was empty"));

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipBytes.constData(), size_t(zipBytes.size()), 0))
        return fail(QStringLiteral("the catalogue is not a readable zip archive"));

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > mz_uint(kMaxMembers))
    {
        mz_zip_reader_end(&zip);
        return fail(QStringLiteral("the catalogue holds more files than this app will read"));
    }
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (qint64(st.m_uncomp_size) > kMaxMemberBytes) continue;   // declared size, checked before allocating
        size_t sz = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!data) continue;
        // Member names are compared, never used as paths — nothing here writes a file, so there is no
        // zip-slip surface to guard. Normalised to forward slashes because a zip written on Windows can
        // carry either.
        QString name = QString::fromUtf8(st.m_filename);
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
        out.insert(name, QByteArray(static_cast<const char*>(data), int(sz)));
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    if (out.isEmpty()) return fail(QStringLiteral("the catalogue archive held no files"));
    if (error) error->clear();
    return out;
}

// ---- parse --------------------------------------------------------------------------------------------------
Feed parseMembers(const QHash<QString, QByteArray>& members)
{
    Feed feed;
    auto bad = [&](const QString& m) { feed.titles.clear(); feed.shapeError = m; return feed; };

    if (!members.contains(QStringLiteral("index.json")))
        return bad(QStringLiteral("the catalogue has no index.json"));

    QJsonParseError perr{};
    const QJsonDocument idxDoc = QJsonDocument::fromJson(members.value(QStringLiteral("index.json")), &perr);
    if (idxDoc.isNull() || !idxDoc.isObject())
        return bad(QStringLiteral("the catalogue's index.json is not a JSON object (%1)")
                       .arg(perr.error == QJsonParseError::NoError ? QStringLiteral("wrong shape")
                                                                   : perr.errorString()));
    const QJsonObject idx = idxDoc.object();
    feed.releaseTag  = idx.value(QStringLiteral("release_tag")).toString().trimmed();
    feed.catalogDate = idx.value(QStringLiteral("catalog_date")).toString().trimmed();

    const QJsonValue titlesVal = idx.value(QStringLiteral("titles"));
    if (!titlesVal.isArray())
        return bad(QStringLiteral("the catalogue's index.json lists no \"titles\""));
    const QJsonArray ids = titlesVal.toArray();

    // An index that lists nothing is a document this reader understood. That is NOT a shapeError: the feed
    // simply contributed no rows, the in-tree catalogue still fills the section, and inventing an error for
    // it would put a red row on screen because somebody published an empty list.
    int skipped = 0;
    for (const QJsonValue& v : ids)
    {
        const QString id = v.toString().trimmed();
        if (id.isEmpty()) { ++skipped; continue; }
        // The id names the file. It is compared against a member name and never touches the filesystem, so a
        // traversing id ("../x") simply fails to find a member — but it is refused explicitly anyway, because
        // an id that is not a plain slug is a document this reader does not understand rather than one it
        // silently tolerates.
        if (id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) || id.contains(QLatin1String("..")))
        { ++skipped; continue; }

        const QString member = QStringLiteral("titles/") + id + QStringLiteral(".json");
        if (!members.contains(member)) { ++skipped; continue; }
        const QJsonDocument tDoc = QJsonDocument::fromJson(members.value(member));
        if (!tDoc.isObject()) { ++skipped; continue; }

        QJsonObject o = tDoc.object();
        // The index is authoritative about the id: a manifest whose own `id` disagrees with its filename is a
        // packaging accident, and the id is what every later lookup uses.
        o.insert(QStringLiteral("id"), id);
        ExternalEmulator e = NativePorts::titleFromJson(o);
        if (e.id.trimmed().isEmpty() || e.port.name.trimmed().isEmpty()
            || e.port.platform.trimmed().isEmpty())
        { ++skipped; continue; }

        // THE LICENCE. A published manifest has no licence field; for a self-compiled entry the terms that
        // govern the thing that gets built are the ENGINE's. Only filled when the manifest did not state one
        // (an in-tree override could), and only from the checked table.
        if (e.port.license.trimmed().isEmpty() && !e.port.buildEngine.trimmed().isEmpty())
            e.port.license = engineInfo(e.port.buildEngine).license;

        feed.titles << e;
    }

    // Every listed title failed to read: the document parsed but describes nothing, which is the "the reader
    // cannot parse this" case #174 is about, not an empty catalogue.
    if (!ids.isEmpty() && feed.titles.isEmpty())
        return bad(QStringLiteral("the catalogue lists %1 titles and none of them could be read")
                       .arg(ids.size()));
    if (skipped > 0 && feed.shapeError.isEmpty())
    {
        // Partial: keep what read. Nothing to say on the row — the rows that exist are correct, and a
        // catalogue mid-publish routinely lists a title whose file has not landed.
    }
    return feed;
}

Feed parseCatalogZip(const QByteArray& zipBytes)
{
    QString err;
    const QHash<QString, QByteArray> members = unpack(zipBytes, &err);
    if (members.isEmpty())
    {
        Feed f;
        f.shapeError = err.isEmpty() ? QStringLiteral("the catalogue could not be unpacked") : err;
        return f;
    }
    return parseMembers(members);
}

// ---- merge --------------------------------------------------------------------------------------------------
QList<ExternalEmulator> mergeByTitleIdentity(const QList<ExternalEmulator>& inTree,
                                             const QList<ExternalEmulator>& feed)
{
    QList<ExternalEmulator> out = inTree;
    for (const ExternalEmulator& f : feed)
    {
        bool shadowed = false;
        for (const ExternalEmulator& m : inTree)
        {
            if (!m.id.isEmpty() && m.id == f.id) { shadowed = true; break; }
            if (m.port.platform.trimmed().compare(f.port.platform.trimmed(), Qt::CaseInsensitive) != 0)
                continue;
            // Same console. Now: do the two entries name the same game? Asked through the SAME title keys the
            // library match is made on, so the catalogue and the ROM gate cannot disagree about identity.
            const QStringList mine = NativePorts::titleKeys(m);
            for (const QString& k : NativePorts::titleKeys(f))
                if (!k.isEmpty() && mine.contains(k)) { shadowed = true; break; }
            if (shadowed) break;
        }
        if (!shadowed) out << f;
    }
    return out;
}

// ---- the cached copy ------------------------------------------------------------------------------------------
Feed cached()
{
    QFile f(cachedCatalogPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
    {
        Feed empty;
        empty.shapeError = QStringLiteral("no copy of the recomp catalogue has been downloaded yet");
        return empty;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    return parseCatalogZip(bytes);
}

Feed storeIfParses(const QByteArray& zipBytes)
{
    const Feed parsed = parseCatalogZip(zipBytes);
    if (!parsed.ok()) return parsed;   // the good copy on disk is left exactly as it was

    QDir().mkpath(cacheDir());
    // Written through a temp file and renamed, so a process that dies mid-write leaves the previous good copy
    // rather than a truncated one that would read as "the catalogue is broken" on every launch afterwards.
    const QString tmp = cachedCatalogPath() + QStringLiteral(".part");
    QFile::remove(tmp);
    QFile t(tmp);
    if (!t.open(QIODevice::WriteOnly | QIODevice::Truncate)) return parsed;
    const qint64 wrote = t.write(zipBytes);
    t.close();
    if (wrote == zipBytes.size())
    {
        QFile::remove(cachedCatalogPath());
        QFile::rename(tmp, cachedCatalogPath());
    }
    QFile::remove(tmp);
    return parsed;
}

// ---- what the section browses -----------------------------------------------------------------------------
QList<ExternalEmulator> catalogue(QString* feedError)
{
    const Feed f = cached();
    if (feedError) *feedError = f.ok() ? QString() : f.shapeError;
    if (!f.ok()) return NativePorts::all();
    return mergeByTitleIdentity(NativePorts::all(), f.titles);
}

bool findById(const QString& id, ExternalEmulator* out)
{
    if (id.trimmed().isEmpty()) return false;
    if (const ExternalEmulator* e = NativePorts::byId(id)) { if (out) *out = *e; return true; }
    for (const ExternalEmulator& e : cached().titles)
        if (e.id == id) { if (out) *out = e; return true; }
    return false;
}

// ---- the schedule ----------------------------------------------------------------------------------------
bool dueForRefresh()
{
    const qint64 last = store().value(QStringLiteral("recomps/feedCheckedAt"), 0).toLongLong();
    if (last <= 0) return true;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // A stamp in the FUTURE is a clock that moved backwards (or a synced ini from another machine), and
    // treating it as "checked recently" would freeze the feed until the clock caught up.
    if (last > now) return true;
    return (now - last) >= kRefreshIntervalSecs;
}

void markRefreshed()
{
    store().setValue(QStringLiteral("recomps/feedCheckedAt"), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

void forgetRefreshStamp()
{
    store().remove(QStringLiteral("recomps/feedCheckedAt"));
    store().sync();
}

} // namespace RecompFeed
