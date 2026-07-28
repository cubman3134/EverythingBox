#include "SettingsTxn.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QVariant>

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
    };
    for (const char* p : kExcludedPrefixes)
        if (key.startsWith(QLatin1String(p))) return false;
    // "downloads" is a bare key AND a family; both are the background download catalog. Matched exactly or
    // as "downloads/..." so a sibling like "downloadsPanel/x" is NOT swept up.
    if (key == QLatin1String("downloads") || key.startsWith(QLatin1String("downloads/"))) return false;
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
    // Remove in-scope keys that did not exist at begin(). Collect first — removing while iterating
    // allKeys() would be mutating the container being read.
    QStringList created;
    for (const QString& k : store().allKeys())
        if (inScope(k) && !g_snapshot.contains(k)) created << k;
    for (const QString& k : created)
    {
        // QSettings::remove(k) removes k AND EVERYTHING BENEATH IT. No in-scope key is a group-prefix of
        // another in the current ini, so in-scope collateral cannot happen — and would self-heal below
        // anyway, since the restore loop runs after this and a wiped snapshot key now differs. What would
        // NOT self-heal is an out-of-scope key beneath a created in-scope one (a bare "resume" over
        // "resume/<id>"), which is exactly the data loss this whole predicate exists to prevent. Capture and
        // put back, rather than leaving the guarantee resting on today's key names.
        const QString prefix = k + QLatin1Char('/');
        QHash<QString, QVariant> beneath;
        for (const QString& d : store().allKeys())
            if (d.startsWith(prefix) && !inScope(d)) beneath.insert(d, store().value(d));
        store().remove(k);
        for (auto it = beneath.cbegin(); it != beneath.cend(); ++it) store().setValue(it.key(), it.value());
    }
    // Restore every changed key.
    for (const QString& k : g_snapshotKeys)
        if (store().value(k) != g_snapshot.value(k)) store().setValue(k, g_snapshot.value(k));
    store().sync();
    commit();
}
