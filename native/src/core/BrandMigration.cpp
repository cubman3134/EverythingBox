// The device-local half of the brand migration: the flags, the ini copy, and the addon-id rewrite. Kept in a
// QtCore-only translation unit on purpose — AddonManager and CloudSync consult done() to decide whether their
// lookups still tolerate the previous brand, and neither should have to link Qt Network to read a flag. run()
// and the Google Drive steps live in BrandMigrationDrive.cpp.
#include "BrandMigration.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

namespace
{

const char* flagKey(BrandMigration::Step s)
{
    switch (s)
    {
        case BrandMigration::Step::LocalIni:    return "device/brandMigrated/localIni";
        case BrandMigration::Step::AddonIds:    return "device/brandMigrated/addonIds";
        case BrandMigration::Step::DriveFolder: return "device/brandMigrated/driveFolder";
        case BrandMigration::Step::DriveFiles:  return "device/brandMigrated/driveFiles";
    }
    return "device/brandMigrated/unknown";
}

QString currentIni(const QString& dir)
{
    return dir + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);
}
QString legacyIni(const QString& dir)
{
    return dir + QStringLiteral("/") + QLatin1String(AppBrand::Legacy::kIniFile);
}

// Deliberately a SHORT-LIVED QSettings rather than a cached static: the very first flag read happens before
// LocalIni has copied the ini into place, and a long-lived QSettings would have snapshotted the file as it was
// BEFORE the copy. Constructing one per access costs a stat; getting this wrong costs the copy.
QString flagStorePath()
{
    return currentIni(AppPaths::dataDir());
}

// True when the destination ini already holds something worth keeping. The migration flags themselves are
// written into this same file, so "the file exists" is NOT the guard — a flag write alone would create the
// file and make a not-yet-run LocalIni believe it had already migrated. Only NON-flag keys count.
bool hasUserContent(const QString& ini)
{
    if (!QFileInfo::exists(ini)) return false;
    QSettings s(ini, QSettings::IniFormat);
    const QStringList keys = s.allKeys();
    for (const QString& k : keys)
        if (!k.startsWith(QStringLiteral("device/brandMigrated/"))) return true;
    return false;
}

// Keys whose SUBJECT is an add-on's SELF-REPORTED id rather than a brand string, and which the rewrite below
// must therefore leave alone.
//
// The distinction is not cosmetic. Everywhere else the previous namespace appears it is OUR string, written
// by this product, and rewriting it is a fact. Here it is a foreign key into whatever `manifest.id` says —
// and this migration cannot observe that. A remote add-on's manifest lives behind a URL (no fetch happens at
// migration time, and on a first launch offline none ever would), and the one bundled here deliberately KEEPS
// the previous namespace forever: for a remote add-on the id and the URL are identity, not branding, so
// renaming it would orphan every add-on URL a user has already saved.
//
// So a rewrite of these keys is a GUESS at an identifier the migration cannot see, and when it guesses wrong
// the user's stored API keys move to a name nothing reads — silently, with the Configure fields simply blank
// afterwards. Left untouched here and reconciled against the ids that ACTUALLY loaded, which is the only
// place the answer exists: see BrandMigration::reconcileAddonConfig.
// NOT listed, and deliberately: addon.update.etag.<id>. It is keyed the same way and the rewrite below does
// rename it — but for that one key the rename is CORRECT, not merely survivable. checkAddonUpdates writes an
// etag only for a JsLocal add-on that carries an updateUrl (AddonManager::checkAddonUpdates), which is to say
// one that lives in a folder on this device — and migrateAddonIds moves every such add-on's manifest id to
// the current prefix. The id this key is keyed by therefore really did move, and the rewrite FOLLOWS it
// rather than guessing at it. The add-on that keeps the previous namespace, which is the one this whole
// exclusion list exists for, is remote: not JsLocal, no updateUrl, so it has no etag to get wrong.
// Said out loud because an unexplained omission from a list like this is indistinguishable from an oversight.
bool isAddonIdKeyed(const QString& key)
{
    return key.startsWith(QStringLiteral("addoncfg/"))        // per-addon config — where Configure puts API keys
        || key.startsWith(QStringLiteral("addon.enabled."));  // per-addon on/off — same foreign key, same miss
}

// Rewrite the previous brand's addon namespace to the current one, in KEYS (addons/<id>/...) and ONLY in
// keys. Returns false only if the file could not be opened for writing.
//
// IT USED TO REWRITE STRING VALUES TOO, and that was the same mistake isAddonIdKeyed exists to prevent, made
// one layer down (#58). The argument is worth spelling out because "rewrite our own brand string wherever it
// appears" reads as obviously right:
//
//   * The addon prefix does not appear inside an ini VALUE as a brand string of ours. By construction it only
//     ever appears there as part of an ADD-ON ID — a foreign key into whatever that add-on's manifest.id
//     says. Every occurrence swept for was one: FavoritesStore's per-item "addonId" and PlaylistStore's
//     per-entry "addonId", both buried in a JSON blob. Nothing else in the ini carries the prefix in a value
//     at all; the two that come closest, addon.remote.urls and the cached remote manifests, are stored as
//     QByteArray and were skipped by the type test above rather than by anything deliberate — which is to say
//     the old code's safety there was luck, not design.
//   * So every value rewrite this function ever performed was a GUESS at an identifier it cannot observe,
//     exactly as a rewrite of addoncfg/<id> would be. When it guesses wrong — the add-on kept the previous
//     spelling, which the one bundled here does on purpose and forever — the favourite's stored addonId now
//     names an add-on that does not exist, and opening it reports "That favourite's source addon isn't
//     available."
//   * And unlike the config case, the damage is not fully repairable. reconcileAddonRefs only moves an id
//     when it can SEE the destination loaded; an add-on that is uninstalled, or remote with no cached
//     manifest yet, resolves to nothing on that launch and is correctly left alone. A rewrite that has
//     already destroyed the original leaves that user with a wrong id and no way back. Not rewriting leaves
//     the original in place until the answer actually exists.
//     NOT "disabled", which an earlier draft of this comment listed: AddonManager::loadFolder and
//     loadRemoteSources apply no isEnabled test — the enabled flag is consulted at SERVE time (catalog /
//     search / meta fan-out), not at load — so a switched-off installed add-on is in loaded_ and its id is
//     in installedIds like any other. Its favourites are therefore repaired on the very launch it is off,
//     which is better than this paragraph used to claim, not worse.
//
// What this costs: an add-on whose id LEGITIMATELY moved (migrateAddonIds renames local manifest ids) now has
// favourites still naming the previous spelling. That is the price, it is paid deliberately, and it is what
// reconcileAddonRefs' second direction repairs — from the ids that loaded, rather than from a guess.
bool rewriteAddonPrefix(const QString& ini)
{
    QSettings s(ini, QSettings::IniFormat);
    if (s.status() != QSettings::NoError) return false;
    const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);
    const QStringList keys = s.allKeys();
    for (const QString& k : keys)
    {
        if (isAddonIdKeyed(k)) continue;   // not ours to rename — see above
        if (!k.contains(oldNs)) continue;
        QString nk = k; nk.replace(oldNs, newNs);
        s.setValue(nk, s.value(k));
        s.remove(k);
    }
    s.sync();
    return s.status() == QSettings::NoError;
}

// Every key the legacy ini held must be readable from the copy, under its rewritten name. This is the
// "reopen the copy and confirm it reads back" gate: only once it passes is the flag set and the legacy file
// demoted to a backup.
bool copyVerified(const QString& legacy, const QString& fresh)
{
    const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);
    QSettings src(legacy, QSettings::IniFormat);
    QSettings dst(fresh, QSettings::IniFormat);
    dst.sync();
    if (dst.status() != QSettings::NoError) return false;
    const QStringList keys = src.allKeys();
    for (const QString& k : keys)
    {
        QString nk = k;
        // Mirror rewriteAddonPrefix exactly: an addon-id-keyed key was deliberately NOT renamed, so it must
        // verify under its ORIGINAL name. Expecting the rewritten one here would fail every install that has
        // ever configured an add-on — the copy would be deleted and the migration would retry forever.
        if (!isAddonIdKeyed(k) && nk.contains(oldNs)) nk.replace(oldNs, newNs);
        if (!dst.contains(nk)) return false;
    }
    return true;
}

// The same id spelled under the other namespace, or empty when the id belongs to neither (a third party's —
// nothing here has any business touching those).
QString counterpartId(const QString& id)
{
    const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);
    if (id.startsWith(newNs)) return oldNs + id.mid(newNs.size());
    if (id.startsWith(oldNs)) return newNs + id.mid(oldNs.size());
    return QString();
}

// Move one stranded key onto the id actually in use. Returns true if a value was carried across.
//
// The two rules are the whole point:
//   * NEVER CLOBBER. A value already stored under the id in use wins, always. Someone whose keys vanished may
//     well have shrugged and typed them in again; putting the stale copy back on top would break them a
//     second time, and this run would be the thing that did it.
//   * The stale key is dropped either way, which is what makes this IDEMPOTENT — the source group empties as
//     it is consumed, so every later run finds nothing and does nothing. Dropping it is not a data risk: it
//     is dead data under a name nothing reads, and the previous brand's ini is still on disk beside the exe
//     (migrateLocalIni copies, never moves) holding the original.
// `touched` records that the ini was modified AT ALL, which is not the same question as the return value:
// the don't-clobber path writes nothing but still drops the stale key, and that removal has to be synced.
//
// AN EMPTY INCUMBENT IS NOT AN INCUMBENT. "A value already stored under the id in use" has to mean a value
// the user could actually be relying on, and a blank is not one — nor is it necessarily something the user
// typed. Both Configure surfaces write blanks: the classic dialog writes EVERY declared field verbatim on
// Save (AddonSettingsDialog.cpp — `value = le->text()`, then an unconditional writeConfig), and the themed
// panel writes any field that was committed. So the exact person this repair exists for — someone already
// broken, whose Configure screen therefore came up blank — only has to have opened that screen and pressed
// Save to end up with "" under the id in use. Testing presence rather than content would let that blank beat
// their real stranded credential AND then drop the credential, which is the very loss the never-clobber rule
// is written to prevent, inflicted by the rule itself. Their only remaining copy would be the legacy ini
// beside the exe, recoverable by hand-editing a file nobody will ever be told to look for.
bool adoptKey(QSettings& s, const QString& from, const QString& to, bool& touched)
{
    if (!s.contains(from)) return false;
    const QVariant stale = s.value(from);
    // Absent and empty are the same answer here, and s.value() already collapses them: a missing key returns
    // an invalid QVariant, whose toString() is empty. A bool `false` is NOT empty ("false"), so the
    // addon.enabled.<id> case — where OFF is the whole point of the setting — still counts as occupied.
    const bool carried = s.value(to).toString().isEmpty();
    if (carried) s.setValue(to, stale);
    s.remove(from);
    touched = true;
    // Only a value with something IN it is reported as restored. A blank moving onto a blank changes nothing
    // a user could see, and counting it would put the "restored N stranded setting(s)" line on screen for a
    // repair that did not happen.
    return carried && !stale.toString().isEmpty();
}

// ---- add-on ids stored inside a VALUE (favourites / playlists) ------------------------------------------

// Re-point ONE stored add-on id at the id that actually loaded. Returns true when `id` was changed.
//
// The whole repair is three answers, and the ORDER of them is the design:
bool repointStoredId(QString& id, const QSet<QString>& installedIds)
{
    if (id.isEmpty()) return false;

    // 1. THE STORED ID ALREADY RESOLVES — leave it exactly as it is. Three separate requirements collapse
    //    into this one line:
    //      * never clobber a blob that is already correct (the overwhelmingly common case: every user who
    //        was never broken takes this branch and nothing is written);
    //      * IDEMPOTENT — the value this function writes satisfies this test, so a second run, and every run
    //        after it, does nothing;
    //      * BOTH SPELLINGS LIVE. reconcileAddonConfig needs an explicit installedIds.contains(other) guard
    //        because it is driven from the id side and would otherwise visit a pair twice and eat a
    //        credential. This function is driven from the STORED id, so when both A and B are loaded,
    //        whichever one the blob names resolves here and is left alone. Nothing is moved, and no value
    //        ping-pongs between the two spellings launch after launch.
    if (installedIds.contains(id)) return false;

    const QString other = counterpartId(id);
    if (other.isEmpty() || other == id) return false;   // a third party's id — never ours to touch

    // 2. IT DOES NOT RESOLVE BUT ITS COUNTERPART DOES — adopt the counterpart. Symmetric, so it repairs BOTH
    //    directions without needing to know which one happened:
    //      * an add-on PINNED to the previous spelling whose blob an earlier build's value rewrite pushed
    //        forward. This is the already-broken user; nothing else gets them back.
    //      * an add-on whose id LEGITIMATELY moved (migrateAddonIds), whose blob still names the previous
    //        spelling — either because it predates the migration, or because the rewrite no longer touches
    //        values at all. Without this half, fixing the first direction would strand every local add-on's
    //        favourites instead.
    if (!installedIds.contains(other)) return false;

    // 3. NEITHER RESOLVES — the line above already returned, and that no-op is deliberate rather than
    //    incidental. An add-on that is simply not loaded ON THIS LAUNCH (uninstalled, or remote with no
    //    cached manifest yet, which on a first launch offline is every remote add-on) is indistinguishable
    //    here from a dead id. "Switched off" is NOT one of them, and used to be listed here in error: the
    //    load paths apply no isEnabled test (it is a serve-time gate), so a disabled add-on's id is in
    //    installedIds and its references resolve normally. Moving the blob to a spelling that does not
    //    resolve either would trade a state that is still recoverable for one that is not. Waiting costs a
    //    launch; guessing costs the favourite. This is also why the value rewrite had to come OUT of
    //    rewriteAddonPrefix — a one-shot guess has no later launch to be right on.
    id = other;
    return true;
}

// favorites/<profile>/items — a JSON array of favourite objects, each carrying the id of the add-on it was
// starred from. Returns how many were re-pointed; writes back only if that is non-zero, so an untouched
// profile's blob is not so much as reserialized (which would churn its bytes and arm the Drive push).
int repointFavorites(QSettings& s, const QString& key, const QSet<QString>& installedIds)
{
    const QString blob = s.value(key).toString();
    if (blob.isEmpty()) return 0;
    QJsonArray arr = QJsonDocument::fromJson(blob.toUtf8()).array();
    int n = 0;
    for (int i = 0; i < arr.size(); ++i)
    {
        if (!arr.at(i).isObject()) continue;
        QJsonObject o = arr.at(i).toObject();
        QString id = o.value(QStringLiteral("addonId")).toString();
        if (!repointStoredId(id, installedIds)) continue;
        o.insert(QStringLiteral("addonId"), id);
        arr.replace(i, o);
        ++n;
    }
    if (n) s.setValue(key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    return n;
}

// playlists/<profile>/items — one level deeper: a JSON array of playlists, each with an "items" array whose
// entries carry the per-entry addonId (playlists are category-scoped and may be mixed-source, so the id is on
// the ENTRY and not on the playlist).
//
// The playlist's own "legacyKey" is deliberately NOT touched, though its first '|' segment is also an add-on
// id. It is retained provenance from the v1 per-catalogue schema, and its only reader takes segment 2 (the
// catalog TYPE, for the category oracle) — HomeView::addonForKey, which is the one thing that would read
// segment 0, has no callers. Repairing a field nothing resolves would be motion without an observable effect,
// and it is untestable through any real lookup path, which is how inert assertions get written.
int repointPlaylists(QSettings& s, const QString& key, const QSet<QString>& installedIds)
{
    const QString blob = s.value(key).toString();
    if (blob.isEmpty()) return 0;
    QJsonArray pls = QJsonDocument::fromJson(blob.toUtf8()).array();
    int n = 0;
    for (int p = 0; p < pls.size(); ++p)
    {
        if (!pls.at(p).isObject()) continue;
        QJsonObject pl = pls.at(p).toObject();
        QJsonArray items = pl.value(QStringLiteral("items")).toArray();
        int changed = 0;
        for (int i = 0; i < items.size(); ++i)
        {
            if (!items.at(i).isObject()) continue;
            QJsonObject e = items.at(i).toObject();
            QString id = e.value(QStringLiteral("addonId")).toString();
            if (!repointStoredId(id, installedIds)) continue;
            e.insert(QStringLiteral("addonId"), id);
            items.replace(i, e);
            ++changed;
        }
        if (!changed) continue;
        // updatedAt is NOT bumped. It is the merge clock for whole-object newest-wins (mdsync T2), and this
        // is a local repair of a value the user never edited — dating it now would let a repaired playlist
        // beat a genuinely newer edit made on another device.
        pl.insert(QStringLiteral("items"), items);
        pls.replace(p, pl);
        n += changed;
    }
    if (n) s.setValue(key, QString::fromUtf8(QJsonDocument(pls).toJson(QJsonDocument::Compact)));
    return n;
}

// Flush, and report the repair count ONLY if the flush actually landed.
//
// Both reconcile passes return a count that their caller turns into a line on screen / in the log — "restored
// N stranded setting(s)", "re-pointed N stored reference(s)". A QSettings::sync() that fails (a read-only ini,
// a full disk, a file another process has locked) leaves those writes nowhere, and the un-checked version
// reported the repair anyway: the user is told their favourites were fixed, nothing on disk changed, and the
// next launch says it again. Returning 0 says what is true — nothing persisted, so there is nothing to
// announce — and costs nothing else, because neither pass is flagged: the next launch simply retries.
//
// The qWarning is the only trace left, and deliberately carries a COUNT and a label, never a value: this is
// the path that moves API keys.
int syncedOrZero(QSettings& s, int count, const QString& what)
{
    s.sync();
    if (s.status() == QSettings::NoError) return count;
    qWarning("brand migration: %s repair of %d item(s) could not be written to the settings file",
             qPrintable(what), count);
    return 0;
}

// The profiles that actually have data under `root`, read off the ini's own group structure. Deliberately not
// ProfileStore::list(): a profile deleted from the list can still have a favourites blob behind it, the ini
// is the authority on what is there to repair, and this keeps the TU free of ProfileStore.
QStringList profilesUnder(QSettings& s, const QString& root)
{
    s.beginGroup(root);
    const QStringList groups = s.childGroups();
    s.endGroup();
    return groups;
}

} // namespace

bool BrandMigration::done(Step s)
{
    QSettings st(flagStorePath(), QSettings::IniFormat);
    return st.value(QLatin1String(flagKey(s)), false).toBool();
}

void BrandMigration::setDone(Step s, bool done)
{
    QSettings st(flagStorePath(), QSettings::IniFormat);
    if (done) st.setValue(QLatin1String(flagKey(s)), true);
    else      st.remove(QLatin1String(flagKey(s)));
    st.sync();
}

// One-time migration from the ORIGINAL "Goliath" naming: goliath.ini -> mymediavault.ini, rewriting the
// renamed addon ids (com.goliath.* -> com.mymediavault.*) in both keys and values, so profiles, API keys and
// favourites carry over. Idempotent: once mymediavault.ini exists this is skipped.
//
// The target here is deliberately AppBrand::Legacy::kIniFile, NOT AppBrand::kIniFile — this is the
// Goliath->MyMediaVault hop, and pointing it at the current ini would be a data-loss bug, not a rename.
// The original migration used QFile::copy and never rename, so goliath.ini is STILL on disk on every install
// that ever ran it. Retargeting this at everythingbox.ini would make the guard read "if everythingbox.ini is
// absent and goliath.ini is present": on a machine that ran Goliath and then MyMediaVault for years, the
// decade-old file would be resurrected into everythingbox.ini FIRST, and the MyMediaVault->EverythingBox
// migration would then find its destination occupied and skip. The user would boot into Goliath-era settings
// with the entire MyMediaVault era invisible — nothing deleted, so it reads as a wipe rather than looking
// like one. No ordering of the newer migration can repair that; the damage is already done by the time it
// runs.
//
// Contract for the MyMediaVault->EverythingBox migration: this function's output must be indistinguishable
// from a genuine MyMediaVault ini (hence the Legacy addon prefix below), so that hop can treat Goliath-era
// and native MyMediaVault users identically.
//
// ---- WHY THIS HOP REWRITES WHAT THE NEXT HOP REFUSES TO (#121) ------------------------------------------
//
// Read next to isAddonIdKeyed and rewriteAddonPrefix above, this looks like an oversight: that hop excludes
// addoncfg/ and addon.enabled. from its KEY rewrite (#56) and does not rewrite VALUES at all (#58), and this
// one does neither. It is not an oversight. Mirroring those two fixes here is a REGRESSION, and the reason
// is a single checkable fact rather than a judgement call:
//
//   THE GOLIATH RENAME WAS TOTAL. Commit 9e41acb (2026-06-23) moved every com.goliath.* id that has ever
//   existed — com.goliath.aiocatalog and com.goliath.podcasts, the only two add-ons bundled at the time —
//   to com.mymediavault.*. Nothing was pinned, and nothing was left behind. The add-on that DOES keep the
//   previous spelling forever, the remote AIO Worker (com.mymediavault.aiocatalog-worker), which is the
//   entire reason #56 and #58 exist, was created 1h43m AFTER that rename and was born under the
//   MyMediaVault name. It never had a Goliath id, so no goliath.ini can contain one. Neither can one
//   contain a remote add-on at all: remote transport landed 85 minutes after the rename too.
//
// So where the next hop's rewrite is a GUESS at an identifier it cannot observe, this one's is a FACT about
// a namespace that emptied completely. For every add-on id a goliath.ini can actually hold, com.goliath.X
// really did become com.mymediavault.X — and reconcileAddonConfig / reconcileAddonRefs then carry it the
// rest of the way to whatever actually loaded, driven by ids rather than by another guess.
//
// What breaks if you "fix" it:
//   * ADDING isAddonIdKeyed HERE strands the very keys the exclusion exists to protect. The key would stay
//     addoncfg/com.goliath.X/*, and counterpartId knows only the two CURRENT namespaces — the counterpart of
//     com.everythingbox.X is com.mymediavault.X, never the Goliath spelling — so reconcileAddonConfig would
//     never visit it. Today that key lands on com.mymediavault.X and is reconciled from there. Excluding it
//     converts a path that works into a permanent orphan.
//   * REMOVING THE VALUE REWRITE does the same to favourites. repointStoredId bails on a com.goliath.*
//     stored id at its `other.isEmpty()` line, correctly treating it as a third party's. Today the id
//     arrives as com.mymediavault.X and is re-pointed like any other.
// Both directions are asserted end-to-end in probe_brand section 7a, through AddonContext::readConfig and
// FavoritesStore rather than through key names, so neither can be reintroduced quietly.
//
// ---- WHAT IS STILL LOST, AND WHY IT IS NOT REPAIRED ------------------------------------------------------
//
// One case survives, and it is real: an install whose addons/<name>/manifest.json STILL says com.goliath.X
// at the moment this runs. Nothing rewrites a bundled manifest at launch (desktop never copies addons in;
// AssetBootstrap is copy-if-absent on mobile), and migrateAddonIds only knows the previous prefix — so that
// add-on goes on reporting com.goliath.X while the keys below have moved its config to com.mymediavault.X.
// counterpartId cannot name that pair, so nothing reconciles it. Precisely what is lost: addoncfg/<id>/*
// (the API keys typed into Configure), addon.enabled.<id>, and the addonId inside every favourite from that
// add-on. The user sees blank Configure fields and "That favourite's source addon isn't available." Nothing
// is deleted — the values sit in the ini under the previous brand's spelling, and goliath.ini is still
// beside the exe — but nothing will ever offer them back.
//
// Left unrepaired deliberately. Repairing it needs counterpartId to become a three-namespace relation, which
// turns the both-spellings-live guard into a three-way problem and starts moving the config of any genuine
// third-party add-on published under com.goliath.* — which today is correctly left alone end to end. Against
// that cost, the population is empty: no Goliath-branded build was ever released (the first release, v0.1.0
// on 2026-06-24, was already MyMediaVault), so the only goliath.ini files that have ever existed are on this
// project's own development machines. Reaching the case additionally requires dropping a current exe into
// such a folder WITHOUT the release zip's addons/, which overwrites those manifests and makes the chain
// above resolve correctly. Documented rather than fixed; if a Goliath-era install ever does turn up, the
// recovery is to hand-copy the values out of goliath.ini, which migrateLocalIni's copy-never-move rule
// guarantees is still there.
bool BrandMigration::migrateGoliathIni(const QString& dataDir)
{
    const QString oldIni = dataDir + QStringLiteral("/goliath.ini");
    const QString newIni = legacyIni(dataDir);
    if (QFile::exists(newIni) || !QFile::exists(oldIni)) return true;   // nothing to do IS completion
    if (!QFile::copy(oldIni, newIni)) return false;

    QSettings s(newIni, QSettings::IniFormat);
    const QString oldNs = QStringLiteral("com.goliath.");
    const QString newNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QStringList keys = s.allKeys();
    for (const QString& k : keys)
    {
        QVariant v = s.value(k);
        // Rewrite the addon namespace inside string values too (e.g. a favourite's stored addonId) — see the
        // total-rename argument above for why this is right HERE and wrong one hop later.
        if (v.typeId() == QMetaType::QString)
        {
            QString sv = v.toString();
            if (sv.contains(oldNs)) { sv.replace(oldNs, newNs); v = sv; }
        }
        if (k.contains(oldNs))
        {
            QString nk = k; nk.replace(oldNs, newNs);
            s.setValue(nk, v);
            s.remove(k);
        }
        else if (v.typeId() == QMetaType::QString && v.toString() != s.value(k).toString())
        {
            s.setValue(k, v);
        }
    }
    s.sync();
    return true;
}

// Step 1 — the one that can lose every setting the user ever made.
//
// COPY, never rename/move. The legacy ini stays on disk untouched as a backup; it is simply never read again
// once the flag is set. A rename would make a failure anywhere downstream (a bad copy, a crash mid-rewrite, a
// disk full) unrecoverable, and the recovery instruction "your settings are still in the previous brand's
// ini, next to the exe" only exists as long as that file does.
bool BrandMigration::migrateLocalIni(const QString& dataDir)
{
    if (done(Step::LocalIni)) return true;

    const QString legacy = legacyIni(dataDir);
    const QString fresh = currentIni(dataDir);

    // Nothing to carry over (a fresh install, or one that never ran the previous brand). Completion, not
    // failure — the flag retires the check.
    if (!QFileInfo::exists(legacy)) { setDone(Step::LocalIni, true); return true; }

    // Already migrated by an earlier run whose flag was lost (or cleared). The destination's CONTENT is the
    // authority here, not its existence: re-copying would restore a stale snapshot over live settings, which
    // is the same data loss as a failed copy, only quieter.
    if (hasUserContent(fresh)) { setDone(Step::LocalIni, true); return true; }

    // A contentless destination (only flags, or a zero-byte leftover) would make QFile::copy refuse. It holds
    // nothing the user would miss, so clear the way.
    if (QFileInfo::exists(fresh) && !QFile::remove(fresh)) return false;

    if (!QFile::copy(legacy, fresh)) return false;

    // The Goliath -> previous-brand hop in main.cpp writes the LEGACY addon prefix, so its output is
    // indistinguishable from a native previous-brand ini. Both kinds of user therefore need this rewrite, and
    // it has to happen before the verification below — which checks the REWRITTEN key names.
    if (!rewriteAddonPrefix(fresh) || !copyVerified(legacy, fresh))
    {
        QFile::remove(fresh);   // leave no half-written ini behind; the legacy file is still intact
        return false;           // no flag -> the next launch tries again
    }

    setDone(Step::LocalIni, true);
    return true;
}

// Step 2 — installed add-on ids. A manifest id under the previous namespace is rewritten in place, and a
// directory NAMED after that id (how AddonManager::installAddon lays a package out: dest = root/<manifest.id>)
// moves with it, or every stored reference resolves to a folder whose manifest no longer matches.
bool BrandMigration::migrateAddonIds(const QString& dataDir)
{
    if (done(Step::AddonIds)) return true;

    const QString root = dataDir + QStringLiteral("/addons");
    const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);

    const QFileInfoList subs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    bool allOk = true;
    for (const QFileInfo& d : subs)
    {
        const QString mfPath = d.absoluteFilePath() + QStringLiteral("/manifest.json");
        QFile mf(mfPath);
        if (!mf.open(QIODevice::ReadOnly)) continue;      // not an addon dir; nothing to migrate
        QJsonObject m = QJsonDocument::fromJson(mf.readAll()).object();
        mf.close();
        const QString id = m.value(QStringLiteral("id")).toString();
        if (!id.startsWith(oldNs)) continue;              // current prefix, or a third party's — leave alone

        QString newId = id; newId.replace(oldNs, newNs);
        m.insert(QStringLiteral("id"), newId);
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { allOk = false; continue; }
        mf.write(QJsonDocument(m).toJson(QJsonDocument::Compact));
        mf.close();

        // Only the id-named layout moves. The bundled add-ons ship in short-named folders (addons/igdb), and
        // renaming those would break the deploy that overwrites them.
        if (d.fileName() != id) continue;
        const QString dest = root + QStringLiteral("/") + newId;
        if (QFileInfo::exists(dest)) continue;            // a migrated copy is already there; don't collide
        if (!QDir().rename(d.absoluteFilePath(), dest)) allOk = false;
    }

    if (allOk) setDone(Step::AddonIds, true);
    return allOk;
}

// Reunite per-add-on state with the id the add-on actually reports.
//
// NOT a numbered Step, and deliberately NOT flagged. The steps above are one-shot because their subject is
// this device's own files; this one's subject is a set of ids that is not fully knowable at migration time —
// a remote add-on contributes its id only once its cached manifest is present, which may be a later launch
// entirely. A flag would retire the repair before the add-on it exists for had ever been seen. It is instead
// idempotent and cheap enough to run on every load (see adoptKey).
//
// Two populations need it, and they are one repair taken from opposite directions:
//   * an add-on that KEPT the previous namespace, whose config an earlier build's blind rewrite pushed into
//     the current one. This is the already-broken user: their API keys are still in the ini, intact, under a
//     name nothing reads. Nothing else recovers them — a forward-only fix leaves them blank forever.
//   * an add-on that MOVED to the current namespace (migrateAddonIds rewrites local manifest ids), whose
//     config the rewrite no longer touches and so is now left behind under the previous one.
//
// Which direction applies is never assumed. `installedIds` is what ACTUALLY loaded, and the counterpart is
// derived from each id rather than guessed at — the whole bug was a migration inventing an identifier it
// could not observe, and repeating that here would just move the breakage around.
//
// Returns the number of values carried across (0 on the overwhelmingly common no-op run).
int BrandMigration::reconcileAddonConfig(const QString& dataDir, const QStringList& installedIds)
{
    QSettings s(currentIni(dataDir), QSettings::IniFormat);
    if (s.status() != QSettings::NoError) return 0;

    int restored = 0;
    bool touched = false;
    for (const QString& id : installedIds)
    {
        const QString other = counterpartId(id);
        if (other.isEmpty() || other == id) continue;   // third-party id, or nothing to reconcile

        // BOTH SPELLINGS ARE LIVE — there is nothing stranded here, so do not touch either of them.
        //
        // Everything below rests on one premise: `other` is a DEAD name, so its config is orphaned and its
        // removal costs nothing. That premise fails outright when the counterpart is itself in installedIds,
        // because then `other` is a loaded add-on's own config, in use, being read right now. The state is
        // reachable without doing anything unusual — the reserved-namespace install guard retires the
        // previous prefix once Step::AddonIds is flagged, so a pre-rebrand package installs cleanly beside
        // its bundled counterpart, and addRemoteSource applies no namespace guard at all, so any remote
        // manifest reporting the counterpart spelling arms it too.
        //
        // Without this line the loop visits the pair twice and destroys a credential. First pass (id=A,
        // other=B): A already holds a value, so nothing is carried — but B's key is removed anyway, and B's
        // value is gone. Second pass (id=B, other=A): B is now vacant, so A's value moves onto B. Net: one
        // credential deleted, the other reading under the wrong add-on, and — because the deleted one may
        // well have been typed in AFTER the migration — no backup anywhere, since the legacy ini beside the
        // exe only holds what existed before it ran. Every later load would shuttle the survivor back and
        // forth and report a restore that never stops happening.
        if (installedIds.contains(other)) continue;

        // addoncfg/<id>/<key> — the per-add-on config group. Enumerated under the SOURCE id: only keys that
        // are actually stranded are considered, so an add-on with nothing to repair costs one group lookup.
        s.beginGroup(QStringLiteral("addoncfg/") + other);
        const QStringList leaves = s.allKeys();
        s.endGroup();
        for (const QString& leaf : leaves)
            if (adoptKey(s, QStringLiteral("addoncfg/") + other + QLatin1Char('/') + leaf,
                            QStringLiteral("addoncfg/") + id    + QLatin1Char('/') + leaf, touched))
                ++restored;

        // addon.enabled.<id> — same foreign key, same silent miss. Milder (its default is "enabled", so the
        // symptom is an add-on the user switched OFF quietly coming back rather than a credential vanishing),
        // but it is the same bug and the repair is the same two lines.
        if (adoptKey(s, QStringLiteral("addon.enabled.") + other, QStringLiteral("addon.enabled.") + id, touched))
            ++restored;
    }

    if (!touched) return restored;   // (restored is 0 here by construction: nothing was written)
    return syncedOrZero(s, restored, QStringLiteral("addon config"));
}

// The same repair, on the surface where the add-on id sits INSIDE the value rather than in the key (#58).
//
// Why this is a separate function rather than more of reconcileAddonConfig: that one is driven from the
// INSTALLED ids, because its subject is a key it can construct from an id. This one is driven from the STORED
// ids, because its subject is a foreign key buried in a JSON blob that has to be walked to be found. The
// direction matters — see the both-live note in repointStoredId — and so does the count they return, which
// AddonManager reports in two different sentences.
//
// EVERY PROFILE. FavoritesStore and PlaylistStore namespace their blobs by profile id, and only one profile is
// ever "current". A repair that used ProfileStore::currentId() would fix whoever launched the app and leave
// every other member of the household broken, with no later run to catch them (this is idempotent, so once
// their own launch found their blob already correct... it never would — but the point stands: nothing would
// ever visit the other profiles). Reading the groups straight off the ini also reaches a profile that has
// since been deleted from profiles/list but whose data is still sitting there.
int BrandMigration::reconcileAddonRefs(const QString& dataDir, const QStringList& installedIds)
{
    QSettings s(currentIni(dataDir), QSettings::IniFormat);
    if (s.status() != QSettings::NoError) return 0;

    // A set, not the list: this is consulted once per stored reference, and a household with a few hundred
    // favourites across a few profiles would otherwise be a linear scan per item.
    const QSet<QString> ids(installedIds.begin(), installedIds.end());

    int repointed = 0;
    for (const QString& profile : profilesUnder(s, QStringLiteral("favorites")))
        repointed += repointFavorites(
            s, QStringLiteral("favorites/") + profile + QStringLiteral("/items"), ids);
    for (const QString& profile : profilesUnder(s, QStringLiteral("playlists")))
        repointed += repointPlaylists(
            s, QStringLiteral("playlists/") + profile + QStringLiteral("/items"), ids);

    if (!repointed) return 0;
    return syncedOrZero(s, repointed, QStringLiteral("addon refs"));
}
