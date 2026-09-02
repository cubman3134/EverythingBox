#include "LiveTvMigrate.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString activeProfile()
{
    const QString p = ProfileStore::currentId();
    return p.isEmpty() ? QStringLiteral("default") : p;
}

// The lookups one source's channel list offers a legacy row, in the order the header states: url first
// (exact), then name (survives a rotated credential). Built once per call.
struct Index
{
    QHash<QString, QString> byUrl;    // stream url        -> identity
    QHash<QString, QString> byName;   // normalised name   -> identity
    QSet<QString> ids;                // every identity this list derives
};

Index buildIndex(const QVector<LiveTvIdentity::Channel>& channels)
{
    Index ix;
    const QVector<QString> ids = LiveTvIdentity::idsFor(channels);
    for (int i = 0; i < channels.size(); ++i)
    {
        const LiveTvIdentity::Channel& c = channels.at(i);
        const QString id = ids.at(i);
        if (id.isEmpty()) continue;
        ix.ids.insert(id);
        // FIRST wins throughout, matching LiveTvIdentity::urlFor: where a list names one thing twice, the
        // provider's own order is the tie-break, and it must be the same tie-break in both directions.
        if (!c.url.isEmpty() && !ix.byUrl.contains(c.url)) ix.byUrl.insert(c.url, id);
        // BOTH name fields are indexed, not just the one the identity rule reads. A favourite's stored title
        // is the DISPLAY title (that is what liveTvChannelFavorite copied), so matching only against tvg-name
        // would miss every row on a provider that sets both and makes them differ.
        for (const QString& n : { LiveTvIdentity::normalizedName(c.title),
                                  LiveTvIdentity::normalizedName(c.tvgName) })
            if (!n.isEmpty() && !ix.byName.contains(n)) ix.byName.insert(n, id);
    }
    return ix;
}

// The url a legacy row is carrying, whichever of its two fields holds it.
QString legacyUrlOf(const QString& itemId, const QString& path)
{
    if (LiveTvIdentity::isCredentialShaped(itemId))
        return itemId.mid(QStringLiteral("livetv:").size());
    if (path.contains(QStringLiteral("://"))) return path;
    return QString();
}

// Is this row one of ours, and still unrepaired? A row already filed under a durable id is left completely
// alone — that is what makes the pass idempotent without a stamp.
bool needsRepair(const QString& itemId, const QString& type, const QString& kind, const QString& path)
{
    const bool live = type == QStringLiteral("livetv") || kind == QStringLiteral("livetv")
                      || LiveTvIdentity::isLiveTvId(itemId);
    if (!live) return false;
    return !legacyUrlOf(itemId, path).isEmpty();
}

// The identity for a legacy row, or empty for outcome three (no match — left exactly as it is).
QString repairedId(const Index& ix, const QString& itemId, const QString& path, const QString& title)
{
    const QString url = legacyUrlOf(itemId, path);
    if (!url.isEmpty())
    {
        const QString byUrl = ix.byUrl.value(url);
        if (!byUrl.isEmpty()) return byUrl;
    }
    return ix.byName.value(LiveTvIdentity::normalizedName(title));
}

bool repairFavorites(const Index& ix, const QString& profile)
{
    const QString key = QStringLiteral("favorites/") + profile + QStringLiteral("/items");
    const QString raw = store().value(key).toString();
    if (raw.isEmpty()) return false;
    const QJsonArray all = QJsonDocument::fromJson(raw.toUtf8()).array();
    if (all.isEmpty()) return false;

    // Every identity the list already holds, so a repair can never mint a second row under one itemId —
    // FavoritesStore's add/remove/isFavorite all key on it, and two rows sharing one would make un-starring
    // remove an arbitrary half. When that would happen the legacy row is LEFT (never deleted): the channel is
    // already starred under its durable name, and this row is a duplicate the user can un-star themselves.
    QSet<QString> taken;
    for (const QJsonValue& v : all) taken.insert(v.toObject().value(QStringLiteral("itemId")).toString());

    bool changed = false;
    QJsonArray out;
    for (const QJsonValue& v : all)
    {
        if (!v.isObject()) { out.append(v); continue; }
        QJsonObject o = v.toObject();
        const QString itemId = o.value(QStringLiteral("itemId")).toString();
        const QString path   = o.value(QStringLiteral("path")).toString();
        if (!needsRepair(itemId, o.value(QStringLiteral("type")).toString(),
                         o.value(QStringLiteral("kind")).toString(), path))
        { out.append(o); continue; }
        const QString id = repairedId(ix, itemId, path, o.value(QStringLiteral("title")).toString());
        if (id.isEmpty() || id == itemId || taken.contains(id)) { out.append(o); continue; }
        // The path becomes the identity too: what re-opens a channel is a lookup in the CURRENT channel list,
        // never a replay of the url it had when it was starred (LiveTvResolver, MainWindow::openRecent).
        o.insert(QStringLiteral("itemId"), id);
        o.insert(QStringLiteral("path"), id);
        taken.insert(id);
        changed = true;
        out.append(o);
    }
    if (!changed) return false;
    store().setValue(key, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    return true;
}

bool repairPlaylists(const Index& ix, const QString& profile)
{
    const QString key = QStringLiteral("playlists/") + profile + QStringLiteral("/items");
    const QString raw = store().value(key).toString();
    if (raw.isEmpty()) return false;
    const QJsonArray all = QJsonDocument::fromJson(raw.toUtf8()).array();
    if (all.isEmpty()) return false;

    bool changed = false;
    QJsonArray outLists;
    for (const QJsonValue& pv : all)
    {
        if (!pv.isObject()) { outLists.append(pv); continue; }
        QJsonObject p = pv.toObject();
        const QJsonArray items = p.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) { outLists.append(p); continue; }

        // Scoped to THIS playlist: PlaylistEntry::itemId is the entry's identity within one playlist (see
        // PlaylistStore.h), and addItem/removeItem/contains all key on it. Same rule as favourites — a repair
        // that would collide leaves the row alone rather than creating a pair one of them would later lose.
        QSet<QString> taken;
        for (const QJsonValue& v : items) taken.insert(v.toObject().value(QStringLiteral("itemId")).toString());

        QJsonArray outItems;
        for (const QJsonValue& v : items)
        {
            if (!v.isObject()) { outItems.append(v); continue; }
            QJsonObject e = v.toObject();
            const QString itemId = e.value(QStringLiteral("itemId")).toString();
            const QString path   = e.value(QStringLiteral("path")).toString();
            // AN ENTRY THAT ARRIVED FROM A PEER WEARING CloudMerge's `livetvUnresolved` marker. Its identity
            // is already credential-free, so nothing below would look at it — but the marker says "this is a
            // row nobody could name", and once THIS device's channel list names it that is no longer true.
            // Clearing it here is what lets a repaired copy go on to protect itself in a later merge.
            if (e.value(QStringLiteral("livetvUnresolved")).toBool() && ix.ids.contains(itemId))
            { e.remove(QStringLiteral("livetvUnresolved")); changed = true; outItems.append(e); continue; }
            if (!needsRepair(itemId, e.value(QStringLiteral("type")).toString(),
                             e.value(QStringLiteral("kind")).toString(), path))
            { outItems.append(e); continue; }
            const QString id = repairedId(ix, itemId, path, e.value(QStringLiteral("title")).toString());
            if (id.isEmpty() || (id == itemId && path == id)) { outItems.append(e); continue; }
            if (id != itemId && taken.contains(id)) { outItems.append(e); continue; }
            e.insert(QStringLiteral("itemId"), id);
            e.insert(QStringLiteral("path"), id);
            // `kind` is what playlistItemsCatalog stamps onto the tile's mime so openResolvedItem re-opens the
            // row by its own record. A pre-#203 Live TV entry has none (the add path only set one for a local
            // file), so it is given one here — without it a repaired entry would be an unopenable tile.
            e.insert(QStringLiteral("kind"), QStringLiteral("livetv"));
            taken.insert(id);
            changed = true;
            outItems.append(e);
        }
        p.insert(QStringLiteral("items"), outItems);
        // `updatedAt` untouched — see the header.
        outLists.append(p);
    }
    if (!changed) return false;
    store().setValue(key, QString::fromUtf8(QJsonDocument(outLists).toJson(QJsonDocument::Compact)));
    return true;
}

} // namespace

bool LiveTvMigrate::withChannels(const QVector<LiveTvIdentity::Channel>& channels)
{
    if (channels.isEmpty()) return false;
    // The ACTIVE profile only. Sources are per-profile, so this channel list is evidence about this profile's
    // rows and nothing else; matching another profile's favourite against it would file it under a channel
    // that profile may not have. Theirs are repaired when they are the active one, which costs nothing —
    // nothing is stamped, so every load tries again.
    const QString profile = activeProfile();
    const Index ix = buildIndex(channels);
    bool wrote = false;
    if (repairFavorites(ix, profile)) wrote = true;
    if (repairPlaylists(ix, profile)) wrote = true;
    if (wrote) store().sync();
    return wrote;
}
