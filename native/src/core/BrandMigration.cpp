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
