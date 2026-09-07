#include "JellyfinDownload.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {

#ifdef EB_JELLYFIN_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_JELLYFIN_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString profileSlug()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

const char* kJobKeyPrefix = "jfdl:";

// Everything a filesystem (or a user reading a folder) would rather not see. Deliberately its own copy
// rather than a reach into MainWindow's file-local helper: this file is QtCore-only and is linked into a
// probe that has no UI at all.
QString sanitize(const QString& in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
    {
        const ushort u = c.unicode();
        if (u < 0x20) { continue; }
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c == QLatin1Char('*')
            || c == QLatin1Char('?') || c == QLatin1Char('"') || c == QLatin1Char('<') || c == QLatin1Char('>')
            || c == QLatin1Char('|'))
        { out += QLatin1Char('_'); continue; }
        out += c;
    }
    out = out.simplified();
    // A trailing dot or space is legal to WRITE on Windows and then unopenable, which is the worst of both.
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    return out.left(120);
}

QString two(int n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); }

// The sort every batch verb agrees on: season, then episode, then title. See the header — a "next 3" that
// depended on the server's own ordering would mean two different things on two servers.
bool beforeInSeries(const Jellyfin::UnionItem& a, const Jellyfin::UnionItem& b)
{
    if (a.parentIndexNumber != b.parentIndexNumber) return a.parentIndexNumber < b.parentIndexNumber;
    if (a.indexNumber != b.indexNumber) return a.indexNumber < b.indexNumber;
    return a.title < b.title;
}

} // namespace

#ifdef EB_JELLYFIN_TEST_SEAM
void JellyfinDownload::setIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif

// ---- The url ---------------------------------------------------------------------------------------

QString JellyfinDownload::downloadPath(const QString& itemId)
{
    if (itemId.isEmpty()) return QString();
    return QStringLiteral("/Items/") + itemId + QStringLiteral("/Download");
}

QString JellyfinDownload::downloadUrl(const QString& root, const QString& itemId, const QString& token)
{
    if (root.isEmpty() || itemId.isEmpty()) return QString();
    QUrl u(root + downloadPath(itemId));
    if (!u.isValid() || u.host().isEmpty()) return QString();
    QUrlQuery q;
    // THE TOKEN IS IN THIS QUERY, for the reason Jellyfin::streamUrl gives about mpv and this header gives
    // about a restart. It is put here, once, on its way into a request — and it is why nothing that holds a
    // Jellyfin download holds a url.
    if (!token.isEmpty()) q.addQueryItem(QStringLiteral("api_key"), token);
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

// ---- Job identity ----------------------------------------------------------------------------------

QString JellyfinDownload::jobKey(const QString& qualifiedId)
{
    if (!Jellyfin::isQualified(qualifiedId)) return QString();
    return QString::fromLatin1(kJobKeyPrefix) + qualifiedId;
}

bool JellyfinDownload::isJobKey(const QString& key)
{
    return !refFromJobKey(key).isEmpty();
}

QString JellyfinDownload::refFromJobKey(const QString& key)
{
    const QString p = QString::fromLatin1(kJobKeyPrefix);
    if (!key.startsWith(p)) return QString();
    const QString ref = key.mid(p.size());
    return Jellyfin::isQualified(ref) ? ref : QString();
}

QString JellyfinDownload::fileNameFor(const Jellyfin::UnionItem& item, const QString& container)
{
    const Jellyfin::Ref ref = Jellyfin::parse(item.id);
    // The id suffix: eight characters of each half, which is plenty to separate two servers' identically
    // named episodes and short enough that the name is still readable. IDs, not credentials.
    const QString suffix = ref.ok
        ? QStringLiteral(" [") + ref.serverId.left(8) + QLatin1Char('-') + ref.itemId.left(8) + QLatin1Char(']')
        : QString();

    QString stem;
    if (item.type == QLatin1String("Episode") && item.indexNumber > 0)
    {
        const QString series = sanitize(item.seriesName.isEmpty() ? item.title : item.seriesName);
        stem = series + QStringLiteral(" S") + two(item.parentIndexNumber)
             + QLatin1Char('E') + two(item.indexNumber);
        const QString ep = sanitize(item.title);
        if (!ep.isEmpty() && ep != series) stem += QLatin1Char(' ') + ep;
    }
    else
    {
        stem = sanitize(item.title);
    }
    if (stem.isEmpty()) stem = QStringLiteral("Jellyfin item");

    // THE CONTAINER IS THE SERVER'S STRING AND IT BECOMES A PATH COMPONENT, so it is not sanitised — it is
    // ACCEPTED OR REFUSED. Letters and digits only, five characters at most, and anything else falls back to
    // "mkv" (which mpv opens by content anyway). Stripping the bad characters instead is what a first draft
    // did, and "../../etc" survived it as the extension "__etc": a value that is not a container at all,
    // arrived at by editing rather than by rejecting.
    const QString raw = container.toLower();
    bool plain = !raw.isEmpty() && raw.size() <= 5;
    for (const QChar c : raw) if (!c.isLetterOrNumber() || c.unicode() > 0x7F) { plain = false; break; }
    const QString ext = plain ? raw : QStringLiteral("mkv");
    return stem + suffix + QLatin1Char('.') + ext;
}

// ---- The batch verbs -------------------------------------------------------------------------------

QVector<Jellyfin::UnionItem> JellyfinDownload::seasonBatch(const QVector<Jellyfin::UnionItem>& episodes,
                                                           const QSet<QString>& alreadyHave)
{
    QVector<Jellyfin::UnionItem> out;
    for (const Jellyfin::UnionItem& e : episodes)
    {
        if (!Jellyfin::isQualified(e.id)) continue;      // a row we could never mint a url for
        if (alreadyHave.contains(e.id)) continue;        // this device already has it: not re-queued
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), beforeInSeries);
    return out;
}

QVector<Jellyfin::UnionItem> JellyfinDownload::nextUnwatched(const QVector<Jellyfin::UnionItem>& episodes,
                                                             int n, const QSet<QString>& alreadyHave)
{
    if (n <= 0) return {};
    QVector<Jellyfin::UnionItem> pool;
    for (const Jellyfin::UnionItem& e : episodes)
    {
        if (!Jellyfin::isQualified(e.id)) continue;
        if (e.played) continue;                          // the server's own UserData
        if (alreadyHave.contains(e.id)) continue;
        pool.push_back(e);
    }
    std::sort(pool.begin(), pool.end(), beforeInSeries);
    if (pool.size() > n) pool.resize(n);
    return pool;
}

// ---- The cap ---------------------------------------------------------------------------------------

JellyfinDownload::CapVerdict JellyfinDownload::evictionSuggestion(const QVector<StoredItem>& items,
                                                                 qint64 capBytes)
{
    CapVerdict v;
    v.capBytes = capBytes > 0 ? capBytes : 0;
    for (const StoredItem& it : items) v.usedBytes += it.bytes > 0 ? it.bytes : 0;
    if (capBytes <= 0 || v.usedBytes <= capBytes) return v;   // no cap, or nothing to say

    v.over = true;
    QVector<StoredItem> byAge = items;
    std::sort(byAge.begin(), byAge.end(), [](const StoredItem& a, const StoredItem& b) {
        const qint64 ta = a.lastPlayedMs > 0 ? a.lastPlayedMs : a.downloadedMs;
        const qint64 tb = b.lastPlayedMs > 0 ? b.lastPlayedMs : b.downloadedMs;
        if (ta != tb) return ta < tb;                    // least recently useful first
        return a.qualifiedId < b.qualifiedId;            // stable across two runs; see the header
    });

    qint64 remaining = v.usedBytes;
    for (const StoredItem& it : byAge)
    {
        if (remaining <= capBytes) break;
        v.victims << it.qualifiedId;
        v.freedBytes += it.bytes > 0 ? it.bytes : 0;
        remaining -= it.bytes > 0 ? it.bytes : 0;
    }
    return v;
}

QStringList JellyfinDownload::watchedCandidates(const QVector<StoredItem>& items)
{
    QStringList out;
    for (const StoredItem& it : items)
        if (it.watched && !it.qualifiedId.isEmpty()) out << it.qualifiedId;
    return out;
}

// ---- Settings --------------------------------------------------------------------------------------

QString JellyfinDownload::capKey()
{
    return QStringLiteral("downloads/") + profileSlug() + QStringLiteral("/capGb");
}

QString JellyfinDownload::removeWatchedKey()
{
    return QStringLiteral("downloads/") + profileSlug() + QStringLiteral("/removeAfterWatched");
}

int JellyfinDownload::capGb()
{
    const int v = store().value(capKey(), 0).toInt();
    return v > 0 ? v : 0;
}

void JellyfinDownload::setCapGb(int gb)
{
    store().setValue(capKey(), gb > 0 ? gb : 0);
    store().sync();
}

bool JellyfinDownload::removeAfterWatched()
{
    return store().value(removeWatchedKey(), false).toBool();
}

void JellyfinDownload::setRemoveAfterWatched(bool on)
{
    store().setValue(removeWatchedKey(), on);
    store().sync();
}
