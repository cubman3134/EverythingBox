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
#include <QJsonDocument>
#include <QJsonObject>
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
// NOT listed, and deliberately: addon.update.etag.<id>. It is keyed the same way and it does get renamed by
// the rewrite below, so a pinned-id add-on loses it — but the entire consequence is that the next update check
// misses its 304 and re-downloads one package, after which the etag is rewritten under the right id and the
// problem has repaired itself. Excluding it would mean reconciling it too (or leaving a dead key behind for
// every add-on whose id legitimately moved), which is more moving parts than a redundant download is worth.
// Said out loud because an unexplained omission from a list like this is indistinguishable from an oversight.
bool isAddonIdKeyed(const QString& key)
{
    return key.startsWith(QStringLiteral("addoncfg/"))        // per-addon config — where Configure puts API keys
        || key.startsWith(QStringLiteral("addon.enabled."));  // per-addon on/off — same foreign key, same miss
}

// Rewrite the previous brand's addon namespace to the current one, in both KEYS (addons/<id>/... ) and inside
// string VALUES (a favourite's stored addonId, a playlist entry's source). Returns false only if the file
// could not be opened for writing.
bool rewriteAddonPrefix(const QString& ini)
{
    QSettings s(ini, QSettings::IniFormat);
    if (s.status() != QSettings::NoError) return false;
    const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);
    const QStringList keys = s.allKeys();
    for (const QString& k : keys)
    {
        // Not ours to rename. The VALUE is skipped as well as the key: an addoncfg value is whatever the user
        // typed into Configure (an API key, a base URL), and a blind substring substitution inside a
        // credential is at best meaningless and at worst corrupts it.
        if (isAddonIdKeyed(k)) continue;
        QVariant v = s.value(k);
        bool valueChanged = false;
        if (v.typeId() == QMetaType::QString)
        {
            QString sv = v.toString();
            if (sv.contains(oldNs)) { sv.replace(oldNs, newNs); v = sv; valueChanged = true; }
        }
        if (k.contains(oldNs))
        {
            QString nk = k; nk.replace(oldNs, newNs);
            s.setValue(nk, v);
            s.remove(k);
        }
        else if (valueChanged)
        {
            s.setValue(k, v);
        }
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

    if (touched) s.sync();
    return restored;
}
