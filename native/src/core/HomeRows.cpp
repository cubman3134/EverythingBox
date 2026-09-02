#include "HomeRows.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>

namespace
{
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The active profile leaf, matching FavoritesStore::favKey()'s per-profile shape (both fall back to "default").
QString rowsProfile()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

QString rowsKey() { return QStringLiteral("homerows/") + rowsProfile() + QStringLiteral("/list"); }

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

// The prefixed families. A rowId is either one of the bare ids below or "<family>:<value>".
const char* const kBareIds[] = { "continue", "favorites", "downloads", "recents", "new" };
const char* const kFamilies[] = { "trakt", "playlist", "preset", "category", "source" };
} // namespace

namespace homerows
{

bool isKnownRowId(const QString& id)
{
    if (id.isEmpty()) return false;
    for (const char* b : kBareIds)
        if (id == QLatin1String(b)) return true;
    const int colon = id.indexOf(QLatin1Char(':'));
    if (colon <= 0 || colon + 1 >= id.size()) return false;
    const QString fam = id.left(colon);
    for (const char* f : kFamilies)
        if (fam == QLatin1String(f)) return true;
    return false;
}

const QStringList& defaultShelfOrder()
{
    // Read straight off HomeView::renderRecents as it stood before #161: the recently-played groups, then
    // "You Missed", then "Airing Soon", then "★ Favorites". Changing this line changes what an untouched
    // profile sees, which is the one thing #161 promised not to do.
    static const QStringList kOrder{ QStringLiteral("continue"), QStringLiteral("trakt:missed"),
                                     QStringLiteral("trakt:calendar"), QStringLiteral("favorites") };
    return kOrder;
}

bool isOptInShelf(const QString& rowId)
{
    return rowId == QStringLiteral("downloads")
        || rowId.startsWith(QStringLiteral("playlist:"))
        || rowId.startsWith(QStringLiteral("preset:"));
}

QVector<Planned> plan(const QVector<Available>& available, const QVector<Row>& list)
{
    QVector<Planned> out;
    // THE DEFAULT. No stored list -> exactly what the app produces today, in its own order, uncapped. This
    // branch is the whole compatibility guarantee and it must stay a verbatim copy: anything computed here
    // (a sort, a filter, a cap) would apply to every untouched profile in the world.
    if (list.isEmpty())
    {
        out.reserve(available.size());
        for (const Available& a : available) out.push_back({ a.rowId, 0 });
        return out;
    }

    QSet<QString> producible;
    for (const Available& a : available) producible.insert(a.rowId);

    QSet<QString> named;   // every id the LIST mentions, hidden ones included: the append pass must not
    QSet<QString> emitted; // re-add a row the user deliberately hid, and must not duplicate one it placed.
    out.reserve(available.size());
    for (const Row& r : list)
    {
        if (r.rowId.isEmpty()) continue;
        named.insert(r.rowId);
        if (!r.visible) continue;
        if (!producible.contains(r.rowId)) continue; // kept in the store, skipped here (see the header)
        if (emitted.contains(r.rowId)) continue;     // a duplicated id renders once
        emitted.insert(r.rowId);
        out.push_back({ r.rowId, r.cap > 0 ? r.cap : 0 });
    }
    // Producers the list has never heard of, in the app's default order (see the header for why they land
    // at the end rather than at their old position).
    for (const Available& a : available)
        if (!named.contains(a.rowId) && !emitted.contains(a.rowId))
        { emitted.insert(a.rowId); out.push_back({ a.rowId, 0 }); }
    return out;
}

Doc fromJson(const QJsonObject& o)
{
    Doc d;
    d.updatedAt = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    for (const QJsonValue& v : o.value(QStringLiteral("rows")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject ro = v.toObject();
        Row r;
        r.rowId = ro.value(QStringLiteral("rowId")).toString();
        if (r.rowId.isEmpty()) continue;
        // A row written by a build that did not have the field defaults to visible/uncapped, which is what
        // the row would have done there — never to hidden, which would silently remove content on upgrade.
        r.visible = ro.value(QStringLiteral("visible")).toBool(true);
        r.cap     = ro.value(QStringLiteral("cap")).toInt(0);
        if (r.cap < 0) r.cap = 0;
        d.rows.push_back(r);
    }
    return d;
}

QJsonObject toJson(const Doc& d)
{
    QJsonArray arr;
    for (const Row& r : d.rows)
    {
        QJsonObject o;
        o.insert(QStringLiteral("rowId"), r.rowId);
        o.insert(QStringLiteral("visible"), r.visible);
        o.insert(QStringLiteral("cap"), r.cap);
        arr.append(o);
    }
    QJsonObject out;
    out.insert(QStringLiteral("updatedAt"), double(d.updatedAt));
    out.insert(QStringLiteral("rows"), arr);
    return out;
}

Doc merge(const Doc& local, const Doc& remote)
{
    // Who wins the ORDER. Equal stamps are broken on the canonical JSON bytes — the same order-independent
    // comparator the rest of the merge document uses — so merge(a,b) == merge(b,a) even on a same-second edit.
    auto bytes = [](const Doc& d) { return QJsonDocument(toJson(d)).toJson(QJsonDocument::Compact); };
    bool remoteWins;
    if (remote.updatedAt != local.updatedAt) remoteWins = remote.updatedAt > local.updatedAt;
    else                                     remoteWins = bytes(remote) > bytes(local);

    const Doc& win  = remoteWins ? remote : local;
    const Doc& lose = remoteWins ? local  : remote;

    Doc out;
    out.updatedAt = qMax(local.updatedAt, remote.updatedAt);
    // A RESET (an empty list) is a husk that clears. Unioning here would let the loser put back the list the
    // user just reset, and the reset could then never propagate at all — see the header.
    if (win.rows.isEmpty()) return out;

    QSet<QString> have;
    for (const Row& r : win.rows)
    {
        if (r.rowId.isEmpty() || have.contains(r.rowId)) continue;
        have.insert(r.rowId);
        out.rows.push_back(r);
    }
    // Never a lost row: everything the loser knows and the winner does not, in the loser's own order.
    for (const Row& r : lose.rows)
    {
        if (r.rowId.isEmpty() || have.contains(r.rowId)) continue;
        have.insert(r.rowId);
        out.rows.push_back(r);
    }
    return out;
}

} // namespace homerows

// ---- the per-profile store --------------------------------------------------------------------------------

QVector<homerows::Row> HomeRowStore::list()
{
    const QByteArray json = store().value(rowsKey()).toString().toUtf8();
    return homerows::fromJson(QJsonDocument::fromJson(json).object()).rows;
}

void HomeRowStore::save(const QVector<homerows::Row>& rows)
{
    homerows::Doc d;
    d.rows = rows;
    d.updatedAt = QDateTime::currentSecsSinceEpoch();
    store().setValue(rowsKey(), QString::fromUtf8(
        QJsonDocument(homerows::toJson(d)).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

void HomeRowStore::reset()
{
    // A RESET IS A DATED EMPTY DOCUMENT, not a removed key. A removed key is indistinguishable from "this
    // device never had a list", so the next merge with a peer still holding the old one would put it straight
    // back. Written as a husk, the reset is the newest record and propagates (see homerows::merge).
    save({});
}

bool HomeRowStore::isCustomised() { return !list().isEmpty(); }

void HomeRowStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
