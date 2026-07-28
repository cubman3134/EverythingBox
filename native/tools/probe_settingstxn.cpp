// Headless check of the settings save/discard transaction (issue #26). SettingsTxn snapshots the
// settings-scope keys when the settings area is entered; Discard restores them. The load-bearing part is
// the SCOPE predicate: a whole-ini snapshot would clobber cloud sync, stats accrual and resume positions
// that are written while a panel is open — so this pins which keys the transaction may touch, and proves a
// key outside that scope written mid-transaction survives rollback untouched.
//
// Every case runs against its OWN scratch ini (eb-probe-settingstxn-<n>.ini). One shared path would not be
// enough: QSettings caches a QConfFile per path process-wide, so deleting the file and re-pointing at the
// same name leaves the previous case's keys alive in memory and the cases silently bleed into each other.
// Each case therefore opens a numbered file, and asserts up front that a key an EARLIER case wrote is absent
// — that assertion is the independence proof, not a formality.
//
// Prints SETTINGSTXN-OK on success; any failure prints SETTINGSTXN-FAIL <cond> and exits non-zero.
#include "SettingsTxn.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SETTINGSTXN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static int g_case = 0;

static QString iniPathFor(int n)
{
    return QDir::tempPath() + QStringLiteral("/eb-probe-settingstxn-%1.ini").arg(n);
}

static QString iniPath()
{
    return iniPathFor(g_case);
}

static void freshIni()
{
    ++g_case;
    QFile::remove(iniPath());
    SettingsTxn::setIniPathForTesting(iniPath());
}

static void put(const QString& k, const QString& v)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    s.setValue(k, v); s.sync();
}

static QString get(const QString& k)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    return s.value(k).toString();
}

static bool has(const QString& k)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    return s.contains(k);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. inScope: the families that must be EXCLUDED --------------------------------------------
    // Everything CloudMerge owns. These are written continuously by playback, marking and stats accrual
    // while a settings panel is open; rolling them back would be data loss, not a nuisance.
    for (const char* k : { "resume/abc", "recent/p1", "marks/p1/items", "favorites/p1",
                           "playlists/p1", "stats/p1/dev", "playstats/p1/dev", "deleted/x" })
        CHECK(SettingsTxn::inScope(QString::fromLatin1(k)) == false);
    // OAuth tokens: signing in is not a setting you discard.
    CHECK(SettingsTxn::inScope(QStringLiteral("cloud/refreshToken")) == false);
    // This install's identity and one-shot migration flags.
    CHECK(SettingsTxn::inScope(QStringLiteral("device/id")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("device/themeChoiceMigrated")) == false);
    // Catalogs written by background download / import.
    CHECK(SettingsTxn::inScope(QStringLiteral("downloads")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("downloads/items")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("pcgames/abc/exe")) == false);

    // ---- 2. inScope: DEVICE-LOCAL BUT IN SCOPE ----------------------------------------------------
    // These are the cases a naive "exclude everything CloudSync::isDeviceLocalKey covers" implementation
    // gets WRONG. They are per-device AND they are settings rows a user must be able to discard.
    CHECK(SettingsTxn::inScope(QStringLiteral("display/mode")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("roms/folder")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("library/folder")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("emulators/root")) == true);
    // Ordinary settings.
    CHECK(SettingsTxn::inScope(QStringLiteral("subs/language")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("themedHome/theme/p1")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("playback/autoplayNext")) == true);
    // A prefix that merely STARTS like an excluded one must not be excluded by a sloppy startsWith.
    CHECK(SettingsTxn::inScope(QStringLiteral("statsPanel/lastTab")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("recentlyUsed/x")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("downloadsPanel/sort")) == true);

    // ---- 3. begin / isDirty / dirtyCount ----------------------------------------------------------
    freshIni();
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    CHECK(SettingsTxn::active() == true);
    // Nothing touched -> clean. Leaving without changing anything must NEVER prompt.
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);

    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    CHECK(SettingsTxn::isDirty() == true);
    CHECK(SettingsTxn::dirtyCount() == 1);

    // Changed and changed BACK reads clean: isDirty compares values, it does not count edits.
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);
    SettingsTxn::commit();
    CHECK(SettingsTxn::active() == false);

    // ---- 4. rollback restores, and LEAVES OUT-OF-SCOPE KEYS ALONE ---------------------------------
    // This is the assertion the whole scope predicate exists for.
    freshIni();
    // Independence: case 3 wrote subs/language. If the store were still on case 3's ini (or on a stale
    // cached QConfFile for a reused path), this would be "en" and the snapshot below would be case 3's.
    CHECK(has(QStringLiteral("subs/language")) == false);
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    put(QStringLiteral("playback/autoplayNext"), QStringLiteral("true"));
    put(QStringLiteral("resume/movie1"), QStringLiteral("120"));
    SettingsTxn::begin();
    CHECK(SettingsTxn::dirtyCount() == 0);   // the snapshot came from THIS case's file
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));          // in scope, changed
    put(QStringLiteral("resume/movie1"), QStringLiteral("999"));         // OUT of scope, changed mid-txn
    put(QStringLiteral("stats/p1/dev"), QStringLiteral("42"));           // OUT of scope, created mid-txn
    CHECK(SettingsTxn::dirtyCount() == 1);   // only the in-scope change counts
    SettingsTxn::rollback();
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));   // restored
    CHECK(get(QStringLiteral("playback/autoplayNext")) == QStringLiteral("true")); // untouched
    CHECK(get(QStringLiteral("resume/movie1")) == QStringLiteral("999"));  // SURVIVES — not clobbered
    CHECK(get(QStringLiteral("stats/p1/dev")) == QStringLiteral("42"));    // SURVIVES — not removed
    CHECK(SettingsTxn::active() == false);

    // ---- 4b. removing a created key must not take OUT-OF-SCOPE keys BENEATH it --------------------
    // QSettings::remove(k) removes k AND its whole subtree. No in-scope key is a group-prefix of another in
    // today's ini, so in-scope collateral cannot happen; but a bare in-scope key sitting above an
    // out-of-scope family ("resume" over "resume/<id>") would silently delete the very data §4 protects.
    freshIni();
    CHECK(has(QStringLiteral("subs/language")) == false);      // independence: case 4's ini is not in play
    put(QStringLiteral("resume/movie1"), QStringLiteral("120"));   // out of scope, pre-existing
    SettingsTxn::begin();
    put(QStringLiteral("resume"), QStringLiteral("bare"));         // in scope (no trailing slash), created
    CHECK(SettingsTxn::dirtyCount() == 1);
    SettingsTxn::rollback();
    CHECK(has(QStringLiteral("resume")) == false);                          // created in-scope key removed
    CHECK(get(QStringLiteral("resume/movie1")) == QStringLiteral("120"));    // its child SURVIVES

    // ---- 5. rollback REMOVES an in-scope key that did not exist at begin() ------------------------
    freshIni();
    CHECK(has(QStringLiteral("resume/movie1")) == false);      // independence: case 4b's ini is not in play
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("de"));
    CHECK(SettingsTxn::dirtyCount() == 1);
    SettingsTxn::rollback();
    CHECK(has(QStringLiteral("subs/language")) == false);

    // ---- 6. nested begin() is a NO-OP ------------------------------------------------------------
    // Hub -> Appearance -> theme picker all call begin(); they must share ONE transaction so Discard
    // from any depth reverts the whole visit. A reset would silently make earlier changes permanent.
    freshIni();
    CHECK(has(QStringLiteral("subs/language")) == false);      // independence: case 5's ini is not in play
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    SettingsTxn::begin();                                    // must NOT re-snapshot
    put(QStringLiteral("playback/autoplayNext"), QStringLiteral("false"));
    CHECK(SettingsTxn::dirtyCount() == 2);                   // both changes still tracked
    SettingsTxn::rollback();
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));  // the FIRST change reverted too
    CHECK(has(QStringLiteral("playback/autoplayNext")) == false);

    // ---- 7. idempotence + inactive safety --------------------------------------------------------
    freshIni();
    CHECK(has(QStringLiteral("subs/language")) == false);      // independence: case 6's ini is not in play
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    SettingsTxn::rollback();
    SettingsTxn::rollback();                                 // second rollback is a harmless no-op
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));
    // Calling the mutators with no open transaction must not crash or corrupt.
    SettingsTxn::commit();
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);

    // ---- residue: every scratch ini this run created must be gone ---------------------------------
    // Drop the store's handle first, otherwise its cached QConfFile re-writes the file on destruction.
    SettingsTxn::setIniPathForTesting(QString());
    for (int n = 0; n <= g_case; ++n)
    {
        QFile::remove(iniPathFor(n));
        CHECK(QFile::exists(iniPathFor(n)) == false);
    }

    if (failures == 0) { std::puts("SETTINGSTXN-OK"); return 0; }
    std::fprintf(stderr, "SETTINGSTXN: %d check(s) failed\n", failures);
    return 1;
}
