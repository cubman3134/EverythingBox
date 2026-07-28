#include "SettingsTxn.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QDebug>
#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QVariant>
#include <utility>   // std::move for the installed hook

namespace {

// Test-only redirect (see SettingsTxn.h). The whole seam — these statics, the setter, and the branch in
// store() — is compiled ONLY for probe_settingstxn. In the app build none of it exists, so store() is the
// plain production static every other core unit uses, and there is no way to point this TU at another file.
//
// The setter DELETES the cached store rather than only re-pointing a path string: a function-local static
// QSettings can be constructed exactly once, so a path captured on first use would silently pin every later
// probe case to the FIRST case's ini — the cases would look independent and share one file. This is the
// ThemeChoice.cpp idiom, and probe_settingstxn proves the independence explicitly.
#ifdef EB_SETTINGSTXN_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_SETTINGSTXN_TEST_SEAM
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

bool g_active = false;
QHash<QString, QVariant> g_snapshot;   // key -> value at begin(); absent-at-begin keys are simply not here
QStringList g_snapshotKeys;            // the in-scope keys present at begin(), for removal detection

// Installed by the UI (see the header). Held here rather than passed to rollback() because the caller that
// discards — a Back handler, a prompt's Discard button — is not the layer that knows how to re-apply a form
// factor or re-render a theme.
std::function<void()> g_rollbackHook;

} // namespace

#ifdef EB_SETTINGSTXN_TEST_SEAM
void SettingsTxn::setIniPathForTesting(const QString& path)
{
    delete g_testStore;          // reopen on the next store() call, so a re-seeded scratch file is re-read
    g_testStore   = nullptr;
    g_testIniPath = path;
    // A redirect starts a new world: any snapshot taken against the previous file is meaningless.
    g_snapshot.clear();
    g_snapshotKeys.clear();
    g_active = false;
}
#endif

bool SettingsTxn::inScope(const QString& key)
{
    // The CloudMerge-owned per-item stores. Written continuously by playback, marking and stats accrual
    // while a panel is open — rolling these back is data loss. Matches CloudSync::isPerItemStoreKey.
    static const char* kExcludedPrefixes[] = {
        "resume/", "recent/", "marks/", "favorites/", "playlists/", "stats/", "playstats/", "deleted/",
        "cloud/",      // OAuth tokens — signing in is not a setting you discard
        "device/",     // this install's identity + one-shot migration flags
        "pcgames/",    // catalog written by the PC-game importer
        // Addon caches filled from NETWORK REPLIES. AddonManager::refreshRemoteManifests() and
        // checkAddonUpdates() are kicked from the AddonManager constructor and write whenever their reply
        // lands — which is arbitrarily later, including in the middle of a settings visit. In scope they
        // would make the exit prompt claim "N settings changed" that the user never touched, and a Discard
        // would revert the manifest cache AFTER reload() had already rebuilt the source list from it,
        // leaving the loaded sources and the cache disagreeing until the next launch.
        //
        // Both prefixes are deliberately LONG. "addon." alone would sweep up addon.enabled.<id> and
        // addon.remote.urls, which ARE settings rows (the Add-ons screen's per-source toggle and its
        // add/remove URL list) and must stay discardable — see the paired in-scope assertions in
        // probe_settingstxn §1.
        "addon.remote.manifest.",   // <md5 of the source base URL> -> the cached manifest bytes
        "addon.update.etag.",       // <addon id> -> the last self-update package ETag
    };
    for (const char* p : kExcludedPrefixes)
        if (key.startsWith(QLatin1String(p))) return false;
    // "downloads" is a bare key AND a family; both are the background download catalog. Matched exactly or
    // as "downloads/..." so a sibling like "downloadsPanel/x" is NOT swept up.
    if (key == QLatin1String("downloads") || key.startsWith(QLatin1String("downloads/"))) return false;

    // Keys written by an ASYNC CALLBACK rather than by a settings row. Matched EXACTLY, never by prefix:
    // every one of these sits in a group whose OTHER keys are genuine user-entered settings, so a prefix
    // here would silently make those undiscardable. probe_settingstxn §1 pins both halves of each pair.
    static const char* kExcludedExactKeys[] = {
        // Trakt OAuth tokens. Settings::setTraktTokens is called from the QNetworkReply::finished lambda in
        // TraktClient::ensureValidToken — i.e. during scrobbling, which runs while a settings panel is open.
        // Trakt ROTATES the refresh token on every refresh, so restoring the snapshot would put a CONSUMED
        // token back and permanently break the account link, on top of a prompt reporting changes the user
        // never made. trakt/clientId and trakt/clientSecret are typed into Settings and STAY in scope.
        "trakt/access", "trakt/refresh", "trakt/expiry",
        // RetroAchievements session credentials, written from rcheevos' async login callback (loginCb in
        // Achievements.cpp). Sign in, then Discard, and the stored token reverts while the in-memory session
        // stays logged in. ra/apikey — the web-API key typed in Settings — STAYS in scope.
        "ra/user", "ra/token",
        // AddonManager::seedDefaultStremioSources()'s one-shot seed/migration latches. Exactly the shape of
        // the device/ flags above: not settings rows, nothing in the UI writes them, and a Discard that
        // removed one (they are created, not changed, on the run that sets them) would re-run the migration
        // on the next launch — re-seeding Torrentio, or re-removing a source the user had deliberately
        // re-added. They are written from the constructor rather than a reply, so the window is narrow; they
        // are listed because they belong to the same background-written family, not because they race.
        "addon.stremio.seeded", "addon.debridio.removed", "addon.cinemeta.removed",
        "addon.cinemeta.removed2", "addon.torrentio.seeded", "addon.torrentio.host.migrated",
    };
    for (const char* k : kExcludedExactKeys)
        if (key == QLatin1String(k)) return false;

    // player/volume is IN scope and stays that way. It is written from the player page's volume slider, not
    // from a settings row, so in principle it could move mid-transaction — but the player page and the
    // settings area are different surfaces the user cannot be on at once, so the write cannot land during a
    // visit. Worst case a Discard restores a volume the user set earlier in the session; the slider re-reads
    // the stored value on the next player open, so nothing ends up inconsistent. Not worth a carve-out.
    return true;
}

void SettingsTxn::begin()
{
    if (g_active) return;   // nested panels share the outermost transaction — see the header
    g_snapshot.clear();
    g_snapshotKeys.clear();
    for (const QString& k : store().allKeys())
        if (inScope(k)) { g_snapshot.insert(k, store().value(k)); g_snapshotKeys << k; }
    g_active = true;
}

bool SettingsTxn::active() { return g_active; }

int SettingsTxn::dirtyCount()
{
    if (!g_active) return 0;
    int n = 0;
    // Changed or removed since begin().
    for (const QString& k : g_snapshotKeys)
        if (store().value(k) != g_snapshot.value(k)) ++n;
    // Created since begin().
    for (const QString& k : store().allKeys())
        if (inScope(k) && !g_snapshot.contains(k)) ++n;
    return n;
}

bool SettingsTxn::isDirty() { return dirtyCount() > 0; }

void SettingsTxn::commit()
{
    g_snapshot.clear();
    g_snapshotKeys.clear();
    g_active = false;
}

void SettingsTxn::rollback()
{
    if (!g_active) return;
    // ONE allKeys() for the whole removal pass. It is O(all keys) and allocates a QString per key (see the
    // cost contract in the header), and the per-created-key descendant scan below used to re-walk it once per
    // created key — O(created x keys) for no benefit. Reusing the list is safe because the only keys removed
    // below and NOT written straight back are in-scope created ones, and the descendant scan skips in-scope
    // keys anyway; every out-of-scope key it does read is restored before the next iteration looks at it.
    const QStringList allAtRollback = store().allKeys();
    // Remove in-scope keys that did not exist at begin(). Collect first — removing while iterating
    // allKeys() would be mutating the container being read.
    QStringList created;
    for (const QString& k : allAtRollback)
        if (inScope(k) && !g_snapshot.contains(k)) created << k;
    for (const QString& k : created)
    {
        // QSettings::remove(k) removes k AND EVERYTHING BENEATH IT. No in-scope key is a group-prefix of
        // another in the current ini, so in-scope collateral cannot happen — and would self-heal below
        // anyway, since the restore loop runs after this and a wiped snapshot key now differs. What would
        // NOT self-heal is an out-of-scope key beneath a created in-scope one (a bare "resume" over
        // "resume/<id>"), which is exactly the data loss this whole predicate exists to prevent. Capture and
        // put back, rather than leaving the guarantee resting on today's key names.
        //
        // The !inScope(d) half of that capture is DEFENCE, not an optimisation, and it is worth knowing that
        // it currently has no observable effect: dropping it re-adds in-scope created descendants right after
        // remove() correctly deleted them, but `created` comes out of allKeys() ancestor-first (allKeys()
        // returns a sorted list, and a key always sorts before key + '/'), so each of those descendants is
        // itself later in `created` and gets removed on its own iteration. Qt documents no ordering for
        // allKeys(), so that self-correction is an implementation detail, not a guarantee — this filter is
        // what keeps Discard's correctness from resting on it. Verified by mutation: dropping the filter
        // alone keeps probe_settingstxn green; dropping it AND reversing `created` fails case 4c on three
        // resurrected keys.
        const QString prefix = k + QLatin1Char('/');
        QHash<QString, QVariant> beneath;
        for (const QString& d : allAtRollback)
            if (d.startsWith(prefix) && !inScope(d)) beneath.insert(d, store().value(d));
        store().remove(k);
        for (auto it = beneath.cbegin(); it != beneath.cend(); ++it) store().setValue(it.key(), it.value());
    }
    // Restore every changed key.
    int changed = 0;
    for (const QString& k : g_snapshotKeys)
        if (store().value(k) != g_snapshot.value(k)) { store().setValue(k, g_snapshot.value(k)); ++changed; }
    // Did this rollback actually undo anything? Counted from the two passes above rather than by re-reading
    // dirtyCount() — that is O(all keys in the ini) (see the header's cost contract) and would be a second
    // full scan for a number both loops already know.
    const bool restored = !created.isEmpty() || changed > 0;
    store().sync();
    // A read-only or locked ini makes sync() a no-op: without this, Discard would silently keep every change
    // while the UI reported success. There is nothing useful to DO about it here — the values are already
    // live and the snapshot is the only copy of the old ones — but it must not vanish, so it goes to the log
    // the same way every other core unit reports a store it could not write.
    if (store().status() != QSettings::NoError)
        qWarning("SettingsTxn: rollback could not write the settings file (QSettings status %d) — the "
                 "discarded changes are still live and may persist", int(store().status()));
    commit();
    // AFTER commit(), deliberately: the hook re-applies side effects (a FormFactor refresh, a re-render) and
    // those paths can call back into settings code. Running it here means it observes active() == false, so a
    // hook that touches a settings row cannot mutate a still-open snapshot — and a hook that called
    // rollback() would hit the inactive-guard no-op instead of recursing.
    //
    // Invoked through a LOCAL COPY, never as g_rollbackHook() directly. The hook exists to re-apply UI side
    // effects, and a re-render that rebuilds the settings surface can reinstall the hook as part of that
    // rebuild — assigning to g_rollbackHook would then destroy the callable whose operator() is still on the
    // stack, and the lambda would run out its body over freed or overwritten capture storage. That is UB and
    // it crashes inside the hook's OWN code, nowhere near the assignment that caused it. The copy costs one
    // std::function copy per rollback that restored something, on a navigation event. Note what it does NOT
    // buy: a hook whose captured object was already destroyed is still a dangling call — the header's
    // lifetime contract (uninstall in the owner's destructor) is the only fix for that half.
    if (restored && g_rollbackHook)
    {
        const std::function<void()> hook = g_rollbackHook;
        hook();
    }
}

void SettingsTxn::setRollbackHook(std::function<void()> hook)
{
    g_rollbackHook = std::move(hook);
}
