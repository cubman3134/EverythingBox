#include "RecentStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "StoredUrl.h"    // issue #200: what a synced store may write down about a signed url
#include "Tombstones.h"   // issue #150: an explicit removal is dated; a cap eviction is not

#include <QDir>
#include <QSettings>
#include <QDateTime>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

static const int kMaxRecents = 40;

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Recents are per-profile so each user's home content is exclusive to them.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

static QString recentsKey()
{
    return QStringLiteral("recent/") + profileId() + QStringLiteral("/items");
}

// The tombstone namespace for THIS profile's recents (issue #150). Per profile, mirroring the store's own
// namespacing exactly as favourites and playlists do, so one profile's removals are invisible under another's.
static QString recentsTombStore()
{
    return QStringLiteral("recent/") + profileId();
}

// The stable identity the merge de-duplicates a recents entry by: its key when it has one (a streamed item's
// path/URL changes per session), else its path. The tombstone key is the SAME identity, so a tombstone lands on
// the entry the union pass is holding.
// Two spellings of one file. A path reaches this store both as the platform writes it and as JSON stored it,
// so the separators differ — and on Windows the case can too. Compared as STRINGS those are different items,
// which is how one book collected three rows: the keyed one, and a twin per spelling.
static bool samePathAs(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

static QString identOf(const RecentItem& it)
{
    return it.key.isEmpty() ? it.path : it.key;
}

// Serialize + persist the list. One spelling for add/remove/clear — they wrote three copies of this block.
static void saveList(const QVector<RecentItem>& items)
{
    QJsonArray arr;
    for (const RecentItem& it : items)
    {
        QJsonObject o;
        o.insert(QStringLiteral("path"), it.path);
        o.insert(QStringLiteral("title"), it.title);
        o.insert(QStringLiteral("kind"), it.kind);
        if (!it.thumb.isEmpty()) o.insert(QStringLiteral("thumb"), it.thumb);
        if (!it.key.isEmpty())   o.insert(QStringLiteral("key"), it.key);
        if (!it.system.isEmpty()) o.insert(QStringLiteral("system"), it.system);
        if (!it.sourceAddonId.isEmpty()) o.insert(QStringLiteral("saddon"), it.sourceAddonId);
        if (!it.sourceItemId.isEmpty())  o.insert(QStringLiteral("sitem"),  it.sourceItemId);
        if (!it.sourceRoute.isEmpty())   o.insert(QStringLiteral("sroute"), it.sourceRoute);
        if (!it.sourceType.isEmpty())    o.insert(QStringLiteral("stype"),  it.sourceType);
        if (it.ts > 0) o.insert(QStringLiteral("ts"), (double)it.ts);
        arr.append(o);
    }
    store().setValue(recentsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

QVector<RecentItem> RecentStore::list()
{
    QVector<RecentItem> out;
    const QByteArray json = store().value(recentsKey()).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RecentItem it;
        it.path  = o.value(QStringLiteral("path")).toString();
        it.title = o.value(QStringLiteral("title")).toString();
        it.kind  = o.value(QStringLiteral("kind")).toString();
        it.thumb = o.value(QStringLiteral("thumb")).toString();
        it.key   = o.value(QStringLiteral("key")).toString();
        it.system = o.value(QStringLiteral("system")).toString();
        it.sourceAddonId = o.value(QStringLiteral("saddon")).toString();
        it.sourceItemId  = o.value(QStringLiteral("sitem")).toString();
        it.sourceRoute   = o.value(QStringLiteral("sroute")).toString();
        it.sourceType    = o.value(QStringLiteral("stype")).toString();
        it.ts    = (qint64)o.value(QStringLiteral("ts")).toDouble();
        if (!it.path.isEmpty()) out.push_back(it);
    }
    return out;
}

// THE ONE PLACE A RECENT IS WRITTEN, AND THEREFORE THE ONE PLACE THE CREDENTIAL IS TAKEN OFF (issue #200).
//
// Twenty-nine call sites hand this function a path; for an addon-resolved stream that path is a SIGNED url
// whose query carries a debrid/provider token, and this store put it in everythingbox.ini verbatim under
// "recent/", which CloudSync::isPerItemStoreKey owns and isDeviceLocalKey does not — so it synced, in
// cleartext, to every device on the account. Found live: seven such rows on a real install.
//
// Scrubbing HERE and not at each call site is the whole point. A per-site fix is one new play route away
// from being incomplete (#193 fixed the two Subsonic sites and this general case stayed open), and a fix at
// the sync boundary would leave the token in the local ini — which is the file bug reports carry.
//
// All four persisted url-shaped fields go through it, each by the rule that fits what it is for:
//   path  -> location(): scheme+host+path. A signed link is a one-shot artefact that has expired by the
//            time anyone clicks the row; the row's IDENTITY is its key, and re-opening still routes by kind.
//   key   -> location(): usually an addon item id and untouched, but a keyless catalog stream records the
//            URL ITSELF as its key (MainWindow's `rkey = item.id.isEmpty() ? url : item.id`), which put a
//            second copy of the token in the same record.
//   title -> title():   the #193 completeBaseName trap, generalised — see StoredUrl.h.
//   thumb -> artwork(): the narrow rule, because a poster url's query is often the poster.
//
// THE FOUR #224 RECIPE FIELDS DELIBERATELY DO NOT GO THROUGH IT. sourceAddonId/sourceItemId/sourceRoute/
// sourceType are ids by construction — an addon manifest id, an item id, and two closed vocabularies
// ("direct"/"imdb", and the MediaItem type) — never links, so there is no query to take off, and running
// location() over them could only corrupt an id that happened to contain a '?'. That they stay id-shaped is
// a property of what WRITES them rather than of this function, so it needs a test rather than a call:
// probe_cloudmerge §38 will hold it, across the sync boundary these fields also ride.
static RecentItem scrubbed(const RecentItem& item)
{
    RecentItem out = item;
    out.path  = StoredUrl::location(item.path);
    out.key   = StoredUrl::location(item.key);
    out.title = StoredUrl::title(item.title, out.path);
    out.thumb = StoredUrl::artwork(item.thumb);
    return out;
}

void RecentStore::add(const RecentItem& item)
{
    if (item.path.isEmpty()) return;
    RecentItem entry = scrubbed(item);
    if (entry.ts == 0) entry.ts = QDateTime::currentSecsSinceEpoch(); // stamp so cross-device sync can merge by recency
    QVector<RecentItem> items = list();
    // De-dup by stable key when present (a streamed item's path/URL changes per session), else by path. This
    // removal is a MOVE-TO-FRONT, not a deletion — the entry is re-inserted on the next line — so it records
    // nothing (issue #150).
    // A re-open that knows only the FILE PATH must not add a second, poorer row beside the rich one it is
    // actually re-opening. Opening from the Recents list goes through the bare-path route — it has a path and
    // nothing else — so its identity is the path, while the row being re-opened identifies by its key. Two
    // different identities over one identical path meant no de-dup: the list grew a hash-named twin at the top
    // and the row someone had just clicked did not move, which is precisely what it looks like from outside.
    //
    // So a keyless entry ADOPTS the identity of an existing entry with the same path. That entry is this item,
    // and it knows strictly more about it — its key, the title someone recognises, its artwork — where a bare
    // path knows only a filename, which for a cached download is a hash.
    //
    // THE RE-MINT RECIPE IS ADOPTED FOR THE SAME REASON (#224). A bare-path re-open knows strictly less than
    // the row it is re-opening — no source addon, no item id, no route, no type — so letting it write those
    // fields through as empty would blank the recipe on the FIRST re-open through the Recents list. #224's
    // fix would then work exactly once per item and die on every open after it, which is the shape of bug
    // that looks like the feature was never there.
    if (entry.key.isEmpty())
        for (const RecentItem& prior : items)
            if (samePathAs(prior.path, entry.path) && !prior.key.isEmpty())
            {
                entry.key = prior.key;
                if (!prior.title.isEmpty()) entry.title = prior.title;
                if (!prior.thumb.isEmpty()) entry.thumb = prior.thumb;
                if (!prior.system.isEmpty()) entry.system = prior.system;
                if (!prior.sourceAddonId.isEmpty()) entry.sourceAddonId = prior.sourceAddonId;
                if (!prior.sourceItemId.isEmpty())  entry.sourceItemId  = prior.sourceItemId;
                if (!prior.sourceRoute.isEmpty())   entry.sourceRoute   = prior.sourceRoute;
                if (!prior.sourceType.isEmpty())    entry.sourceType    = prior.sourceType;
                break;
            }

    const QString ident = identOf(entry);
    for (int i = items.size() - 1; i >= 0; --i)
        // The entry itself, by identity — and ALSO any keyless row for the same file. Such a row is a twin
        // this bug already created, and it would otherwise sit there forever: its identity is the path while
        // the real row's is the key, so nothing would ever collapse the two. Re-opening the item heals it.
        // Its own identity, OR the same FILE. The second is what collapses a twin left by an earlier open —
        // and twins of each other, which differ only in how the path was spelled. Guarded so it never merges
        // two genuinely different items that happen to share one cached file: that needs at least one side to
        // be keyless, or both keys to agree.
        if (identOf(items[i]) == ident
            || (samePathAs(items[i].path, entry.path)
                && (items[i].key.isEmpty() || entry.key.isEmpty() || items[i].key == entry.key)))
            items.remove(i);
    items.prepend(entry);                              // newest first
    // CAP EVICTION, AND IT RECORDS NOTHING (issue #150). Dropping the 41st-oldest entry is the list running out
    // of room, not the user saying "forget this" — tombstoning it would make the cap PERMANENT, so an item that
    // scrolled off could never re-enter on a later re-watch and a peer with a shorter history could never hand
    // it back. It needs no record either way: the merge unions both devices' lists and re-applies the cap, so
    // an evicted entry re-appears only if it is still among the newest 40 overall, which is exactly right.
    while (items.size() > kMaxRecents) items.removeLast();
    saveList(items);

    // Opening something UNDOES an earlier explicit removal of it, so drop any tombstone. Without this the
    // removal would go on suppressing the entry on every peer whose clock reads the re-open at or before the
    // removal's second, and — worse — the tombstone would outlive the entry it describes in the document.
    Tombstones::remove(recentsTombStore(), identOf(entry));
}

void RecentStore::remove(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return;
    QVector<RecentItem> items = list();
    QStringList removed;
    // Match the argument as given AND as it would have been stored (issue #200): a caller that still holds
    // the signed url it played — or a row written before the scrub landed — must still be able to remove the
    // entry, whose stored spelling is now credential-free. Both spellings, so neither direction misses.
    const QString scrubbedArg = StoredUrl::location(pathOrKey);
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].path == pathOrKey || items[i].path == scrubbedArg
            || (!items[i].key.isEmpty() && (items[i].key == pathOrKey || items[i].key == scrubbedArg)))
        { removed.push_back(identOf(items[i])); items.remove(i); }
    if (removed.isEmpty()) return;

    saveList(items);
    // THE USER'S EXPLICIT REMOVE, so it is dated (issue #150). Without a tombstone this deletion looked exactly
    // like "this device has never opened that item", and the merge — which unions the two lists and cannot read
    // a reason out of an absence — handed the entry straight back from any peer that still had it.
    //
    // Tombstoned by the entry's OWN identity, not by the argument: callers pass whichever of path/key they have
    // (HomeView::uninstallGameItem passes both in turn), while the union pass keys on key-else-path. Naming the
    // argument would file the tombstone under something the merge never looks up.
    for (const QString& id : removed) Tombstones::record(recentsTombStore(), id);
}

RecentItem RecentStore::find(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return {};
    // The argument as given AND as it would have been STORED, exactly as remove() matches it (issue #200):
    // a caller still holding the signed url it played must resolve to the same row remove() would drop, or
    // openRecent would fail to find the recipe for the very rows #224 exists to re-mint.
    const QString scrubbedArg = StoredUrl::location(pathOrKey);
    for (const RecentItem& it : list())
        if (it.key == pathOrKey || (!it.key.isEmpty() && it.key == scrubbedArg)
            || samePathAs(it.path, pathOrKey) || samePathAs(it.path, scrubbedArg)) return it;
    return {};
}

void RecentStore::clear()
{
    // Clear is "remove everything", one explicit user action per entry, so every entry gets a tombstone —
    // otherwise emptying the list on one device just re-downloads it from the next peer to sync.
    const QVector<RecentItem> items = list();
    store().remove(recentsKey());
    store().sync();
    for (const RecentItem& it : items) Tombstones::record(recentsTombStore(), identOf(it));
}

RecentStore::Relaunch RecentStore::relaunchFor(const QString& kind)
{
    if (kind == QStringLiteral("steamgame")) return Relaunch::SteamGame;
    if (kind == QStringLiteral("epicgame"))  return Relaunch::EpicGame;
    if (kind == QStringLiteral("goggame"))   return Relaunch::GogGame;
    if (kind == QStringLiteral("battlenetgame")) return Relaunch::BattleNetGame;
    if (kind == QStringLiteral("pcgame"))    return Relaunch::PcGame;
    if (kind == QStringLiteral("video"))     return Relaunch::Video;
    if (kind == QStringLiteral("audio"))     return Relaunch::Audio;
    if (kind == QStringLiteral("document"))  return Relaunch::Document;
    if (kind == QStringLiteral("game"))      return Relaunch::Game;
    return Relaunch::Unknown;
}

RecentStore::Reopen RecentStore::reopenFor(const RecentItem& it, bool addonAvailable)
{
    // Both halves or neither. A route with no id would call resolve with an empty id, which every provider
    // answers "no source" — the same dead end as before, wearing a message that blames the source instead.
    if (it.sourceItemId.isEmpty() || it.sourceRoute.isEmpty()) return Reopen::ReplayPath;
    if (it.sourceRoute == QLatin1String("imdb")) return Reopen::ResolveImdb;
    if (it.sourceRoute == QLatin1String("direct"))
        return addonAvailable ? Reopen::ResolveDirect : Reopen::SourceMissing;
    return Reopen::ReplayPath;   // an unknown route: a newer build wrote this row. Replay, never guess.
}
