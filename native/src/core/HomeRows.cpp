#include "HomeRows.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QCoreApplication>
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
const char* const kBareIds[] = { "continue", "favorites", "downloads", "recents", "new", "requests" };
// "jellyfin" is here because #83's shelf went into defaultShelfOrder() without ever being added to this
// list, so the one build-wide "is this a rowId I know?" answer said NO about a shelf the classic home draws
// every day -- and the editor's fallback label for an unknown id is "not on this device" (issue #314).
const char* const kFamilies[] = { "trakt", "playlist", "preset", "category", "source", "jellyfin" };
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

// WHICH HOME DRAWS WHICH (issue #314). Read straight off the two producers, and it is a table rather than a
// guess because both ends of it are in one file each:
//
//   * the CLASSIC home -- HomeView::renderRecents. It walks defaultShelfOrder() for the built-in shelves and
//     the stored list for the opt-in ones (isOptInShelf), and every id below is one of its pushShelf calls.
//   * the THEMED home  -- HomeView::categoryItems() and HomeView::systemItems(), the two lists MainWindow's
//     showThemedXmb / showThemedHome hand the QML. Both run the row list through applyHomeRowList, and the
//     only ids either of them can produce are "category:<key>" and "source:<navKey>".
//
// There is deliberately no third answer. A shelf of forty recently-played items has nowhere to sit on a home
// whose rows ARE the catalogues, and a catalogue tile is not a shelf; giving either family a producer on the
// other home is a redesign of one of the two homes, not a missing case here. What this function buys is that
// the editor can SAY so -- see layoutNote.
int layoutsFor(const QString& rowId)
{
    if (rowId.isEmpty()) return NoLayout;
    // The classic home's shelves. Built-in and opt-in alike: the difference between those two is whether the
    // row appears without being asked for, not which home draws it.
    if (rowId == QLatin1String("continue") || rowId == QLatin1String("jellyfin:continue")
        || rowId == QLatin1String("new") || rowId == QLatin1String("trakt:calendar")
        || rowId == QLatin1String("favorites") || rowId == QLatin1String("requests")
        || rowId == QLatin1String("downloads")
        || rowId.startsWith(QLatin1String("playlist:")) || rowId.startsWith(QLatin1String("preset:")))
        return ClassicHome;
    // The themed home's rows.
    if (rowId.startsWith(QLatin1String("category:")) || rowId.startsWith(QLatin1String("source:")))
        return ThemedHome;
    // "recents", "trakt:missed", and every id this build has never heard of. Accepted vocabulary, kept in the
    // store, skipped at render -- and given no note by layoutNote, because the editor's "not on this device"
    // is the truer sentence for a deleted preset or a peer device's row.
    return NoLayout;
}

QString layoutNote(const QString& rowId, bool themedHomeActive)
{
    const int l = layoutsFor(rowId);
    if (l == NoLayout) return QString();
    const int here = themedHomeActive ? int(ThemedHome) : int(ClassicHome);
    if (l & here) return QString();   // it draws here: say nothing
    return (l & ThemedHome) ? QCoreApplication::translate("homerows", "only on the themed home")
                            : QCoreApplication::translate("homerows", "only on the classic home");
}

const QStringList& defaultShelfOrder()
{
    // Read straight off HomeView::renderRecents as it stood before #161: the recently-played groups, then
    // "You Missed", then "Airing Soon", then "★ Favorites". Changing this line changes what an untouched
    // profile sees, which is the one thing #161 promised not to do.
    //
    // #83 ADDS ONE, AND THE PROMISE ABOVE SURVIVES IT. "jellyfin:continue" is what a connected media server
    // says this user is part-way through. It is built-in rather than opt-in because it is a source of the
    // same kind as the others here - something the app can produce and the user never had to ask for - and
    // it costs an untouched profile NOTHING: HomeView drops an empty producer before it becomes an available
    // row, so a profile with no Jellyfin server sees the identical shelves in the identical order. It sits
    // directly after "continue" because it is the same question one machine along: what were you in the
    // middle of? The Trakt shelf is about what EXISTS rather than about what you were doing.
    //
    // ONE substitution alongside it: issue #155's "new" stands where "trakt:missed" stood. That was not a
    // new row either — the New shelf ABSORBED #25's "You Missed" rows (HomeView's buildNew unions them with
    // the followed-series children), so the position and the content are the ones that were already there
    // and only the header changed. "trakt:missed" stays accepted vocabulary with no producer in this build,
    // which the open-vocabulary rule above already covers: a stored list naming it keeps it and skips it.
    //
    // #109 ADDS ONE MORE, AND THE PROMISE ABOVE SURVIVES IT FOR THE SAME REASON. "requests" is what this
    // profile has ASKED for and does not have yet. Built-in rather than opt-in because a shelf you have to
    // go and add is one nobody finds the minute after they pressed Request — and it costs an untouched
    // profile NOTHING: the producer is empty until somebody makes a request, and HomeView drops an empty
    // producer before it becomes an available row. It sits LAST because it is the only shelf that is not
    // about something you can watch right now.
    static const QStringList kOrder{ QStringLiteral("continue"), QStringLiteral("jellyfin:continue"),
                                     QStringLiteral("new"),
                                     QStringLiteral("trakt:calendar"), QStringLiteral("favorites"),
                                     QStringLiteral("requests") };
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
