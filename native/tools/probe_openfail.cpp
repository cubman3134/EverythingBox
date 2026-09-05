// Headless check of THE FAILURE THAT OUTLIVES THE TOAST (issue #239) — src/core/OpenFailStore.
//
// WHY THIS PROBE EXISTS. A press that resolves a release, fetches nothing, and puts up a toast leaves no
// trace once the toast fades: the shelf a moment later looks exactly as it did before the press. That is not
// a hypothetical — #236 was filed as "the shelf's Play never reaches openGame", with the emulator backend
// suspected, when the real event was a download that came back empty and said so for four seconds. So the
// failure is now written down against the item, and the properties that make that trustworthy are the ones
// below: it is keyed by IDENTITY, it expires, it clears, and it never leaves this device.
//
// WHAT IT PINS:
//   §1 THE ROUND TRIP. A recorded failure reads back with its message, its title and its time, and the row
//      marker's predicate says so. An item nobody failed on is clean, and an empty id writes nothing at all
//      (there would be no page to carry it and no row to mark).
//   §2 THE IDENTITY IS THE ID, NOT THE TITLE. Two shelf rows can share a title — two dumps of one game, a
//      film and its remake — and marking the wrong one is worse than marking nothing: it tells the user the
//      copy that works is the broken one.
//   §3 SEVEN DAYS, ON AN INJECTED CLOCK. Live at six days and twenty-three hours, gone at seven days, and
//      gone READ-SIDE — invisible to every accessor BEFORE anything purges the file, so a stale mark cannot
//      surface through a path that forgot to sweep first. An undated row counts as expired, because an
//      undated row is the one shape that could never clear itself.
//   §4 CLEARING. On a successful open and on dismissal (the same primitive, two occasions), and clearing one
//      item never clears another. Re-recording replaces rather than accumulates: the page answers "why did
//      it not open just now", not "here is a history".
//   §5 DEVICE-LOCAL, ASSERTED AGAINST THE REAL CARVE-OUT. A failed open is a fact about THIS device's last
//      attempt; syncing it would put one device's dead link on another device's shelf with a "Try again"
//      that has nothing to retry. Asserted three ways against the live CloudSync: the key is device-local,
//      it is NOT a per-item store key (so the CloudMerge progress document never carries it either), and —
//      the byte scan that catches a carve-out someone loosens later — the exact settings.json bytes the
//      bundle embeds contain no "openfail" anywhere.
//   §6 THE CAP, so an evening of a dead source cannot grow the file without bound, and the newest failure is
//      the one that survives it.
//   §7 THE HOT COPY and its one documented consequence -- an external write is invisible until invalidate().
//
// Prints OPENFAIL-OK on success; any failure prints OPENFAIL-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// it reads and writes starts empty and is never shared with a sibling probe or a previous run.
#include "OpenFailStore.h"
#include "CloudSync.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "OPENFAIL-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A fixed, readable instant to hang the clock arithmetic off: 2026-09-01T00:00:00Z.
static const qint64 kT0 = 1788307200LL;
static const qint64 kDay = 24 * 60 * 60;

static QSettings& ini()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Wipe every openfail row this process wrote, so a section starts from a known empty store without
// depending on the section before it.
static void resetStore()
{
    for (const QString& k : ini().allKeys())
        if (k.startsWith(QStringLiteral("openfail/"))) ini().remove(k);
    ini().sync();
    OpenFailStore::invalidate();   // an external write; the store's hot copy has to be told (see the tail)
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QLatin1String(AppBrand::kDisplayName));

    // ---- §1 THE ROUND TRIP ------------------------------------------------------------------------------
    resetStore();
    CHECK(!OpenFailStore::marked(QStringLiteral("tt0111161"), kT0));      // nothing failed yet
    CHECK(OpenFailStore::lookup(QStringLiteral("tt0111161"), kT0).isNull());

    OpenFailStore::record(QStringLiteral("tt0111161"), QStringLiteral("Tetris"),
                          QStringLiteral("Couldn't get “Tetris” — the source returned no data."), kT0);
    const OpenFailure got = OpenFailStore::lookup(QStringLiteral("tt0111161"), kT0);
    CHECK(!got.isNull());
    CHECK(got.id == QStringLiteral("tt0111161"));
    CHECK(got.title == QStringLiteral("Tetris"));
    CHECK(got.message.contains(QStringLiteral("returned no data")));
    CHECK(got.ts == kT0);
    CHECK(OpenFailStore::marked(QStringLiteral("tt0111161"), kT0));
    CHECK(OpenFailStore::list(kT0).size() == 1);
    // The key it landed under, spelled out: the "openfail/" prefix is what CloudSync's carve-out matches on
    // (§5), and "default" is the no-profile-selected fallback the undated-row case below writes against.
    CHECK(ini().contains(QStringLiteral("openfail/default/items")));

    // PER PROFILE, like recents and favourites: one household member's dead link is not a mark on another's
    // shelf, and the two of them may well be browsing different sources.
    ProfileStore::setCurrent(QStringLiteral("kid"));
    CHECK(!OpenFailStore::marked(QStringLiteral("tt0111161"), kT0));
    ProfileStore::setCurrent(QString());
    CHECK(OpenFailStore::marked(QStringLiteral("tt0111161"), kT0));

    // An empty id is a no-op on every writer: there is no detail page keyed on "" and no row to mark, and a
    // store that accepted one would answer marked("") for every caller that has not resolved an id yet.
    OpenFailStore::record(QString(), QStringLiteral("Nameless"), QStringLiteral("boom"), kT0);
    CHECK(OpenFailStore::list(kT0).size() == 1);
    CHECK(!OpenFailStore::marked(QString(), kT0));

    // ---- §2 THE IDENTITY IS THE ID, NOT THE TITLE -------------------------------------------------------
    resetStore();
    OpenFailStore::record(QStringLiteral("nes:tetris-usa"), QStringLiteral("Tetris"),
                          QStringLiteral("no data"), kT0);
    CHECK(OpenFailStore::marked(QStringLiteral("nes:tetris-usa"), kT0));
    // The same TITLE, a different item — the copy that works must not be marked broken.
    CHECK(!OpenFailStore::marked(QStringLiteral("nes:tetris-jpn"), kT0));
    CHECK(OpenFailStore::lookup(QStringLiteral("nes:tetris-jpn"), kT0).isNull());
    CHECK(!OpenFailStore::marked(QStringLiteral("Tetris"), kT0));   // the title is not an identity

    // ---- §3 SEVEN DAYS, ON AN INJECTED CLOCK ------------------------------------------------------------
    resetStore();
    OpenFailStore::record(QStringLiteral("old"), QStringLiteral("Old"), QStringLiteral("no data"), kT0);
    CHECK(OpenFailStore::marked(QStringLiteral("old"), kT0));                       // the moment it happened
    CHECK(OpenFailStore::marked(QStringLiteral("old"), kT0 + 6 * kDay));            // six days later
    CHECK(OpenFailStore::marked(QStringLiteral("old"), kT0 + 7 * kDay - 3600));     // six days, twenty-three hours
    CHECK(!OpenFailStore::marked(QStringLiteral("old"), kT0 + 7 * kDay));           // seven days: gone
    CHECK(!OpenFailStore::marked(QStringLiteral("old"), kT0 + 30 * kDay));
    // READ-SIDE, and therefore true of every accessor before anything sweeps the file.
    CHECK(OpenFailStore::lookup(QStringLiteral("old"), kT0 + 7 * kDay).isNull());
    CHECK(OpenFailStore::list(kT0 + 7 * kDay).isEmpty());
    CHECK(OpenFailStore::list(kT0 + 6 * kDay).size() == 1);
    // ...and the sweep itself, which is housekeeping rather than correctness: it drops exactly the row every
    // reader was already ignoring, and nothing while the row is still live.
    CHECK(OpenFailStore::purgeExpired(kT0 + 6 * kDay) == 0);
    CHECK(OpenFailStore::purgeExpired(kT0 + 7 * kDay) == 1);
    CHECK(OpenFailStore::purgeExpired(kT0 + 7 * kDay) == 0);

    // An UNDATED row (a hand-edited ini, a record from a build that did not stamp one) reads as expired
    // rather than as eternal — the one shape that could otherwise never clear itself.
    resetStore();
    ini().setValue(QStringLiteral("openfail/default/items"),
                   QStringLiteral("[{\"id\":\"undated\",\"title\":\"U\",\"msg\":\"m\"}]"));
    ini().sync();
    OpenFailStore::invalidate();   // planted by hand, behind the store's back
    CHECK(!OpenFailStore::marked(QStringLiteral("undated"), kT0));
    CHECK(OpenFailStore::list(kT0).isEmpty());

    // ---- §4 CLEARING ------------------------------------------------------------------------------------
    resetStore();
    OpenFailStore::record(QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("no data"), kT0);
    OpenFailStore::record(QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("no data"), kT0);
    CHECK(OpenFailStore::marked(QStringLiteral("a"), kT0));
    CHECK(OpenFailStore::marked(QStringLiteral("b"), kT0));
    OpenFailStore::clear(QStringLiteral("a"));      // "a" opened successfully (or the user dismissed it)
    CHECK(!OpenFailStore::marked(QStringLiteral("a"), kT0));
    CHECK(OpenFailStore::marked(QStringLiteral("b"), kT0));   // clearing one never clears another
    OpenFailStore::clear(QStringLiteral("nobody"));           // unknown id: a no-op, not a wipe
    CHECK(OpenFailStore::marked(QStringLiteral("b"), kT0));
    OpenFailStore::clear(QString());                          // empty id: likewise
    CHECK(OpenFailStore::marked(QStringLiteral("b"), kT0));

    // Re-recording REPLACES: one row per item, carrying the newest attempt's words and time.
    OpenFailStore::record(QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("first"), kT0);
    OpenFailStore::record(QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("second"), kT0 + 60);
    CHECK(OpenFailStore::list(kT0 + 60).size() == 1);
    CHECK(OpenFailStore::lookup(QStringLiteral("b"), kT0 + 60).message == QStringLiteral("second"));
    CHECK(OpenFailStore::lookup(QStringLiteral("b"), kT0 + 60).ts == kT0 + 60);

    // ---- §5 DEVICE-LOCAL, AGAINST THE REAL CARVE-OUT ----------------------------------------------------
    // The key never travels in the heavy settings bundle...
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("openfail/default/items")));
    CHECK(CloudSync::isDeviceLocalKey(QStringLiteral("openfail/someprofile/items")));
    // ...and it is not one of the per-item stores the CloudMerge progress document owns either, so there is
    // no second channel it could ride. (Both halves matter: a key that is neither is simply not synced.)
    CHECK(!CloudSync::isPerItemStoreKey(QStringLiteral("openfail/default/items")));
    // A control, so the two predicates above are demonstrably still doing something.
    CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("recent/default/items")));
    CHECK(!CloudSync::isDeviceLocalKey(QStringLiteral("recent/default/items")));
    // THE BYTE SCAN. With a live failure in the ini, the exact settings.json the bundle embeds must not
    // contain the prefix, the profile-qualified key, or the message of the failure.
    resetStore();
    OpenFailStore::record(QStringLiteral("secretish-id"), QStringLiteral("A Title"),
                          QStringLiteral("Couldn't get it — the source returned no data."), kT0);
    const QByteArray bundle = CloudSync::buildSettingsJson();
    CHECK(!bundle.contains("openfail"));
    CHECK(!bundle.contains("secretish-id"));
    CHECK(!bundle.contains("returned no data"));
    // The scan is only worth anything if the document it scans is non-empty and does carry ordinary settings.
    ini().setValue(QStringLiteral("player/rememberPosition"), true);
    ini().sync();
    const QByteArray bundle2 = CloudSync::buildSettingsJson();
    CHECK(bundle2.contains("player/rememberPosition"));
    CHECK(!bundle2.contains("openfail"));

    // ---- §6 THE CAP -------------------------------------------------------------------------------------
    resetStore();
    for (int i = 0; i < OpenFailStore::kMaxEntries + 25; ++i)
        OpenFailStore::record(QStringLiteral("id-%1").arg(i), QStringLiteral("T"),
                              QStringLiteral("no data"), kT0 + i);
    CHECK(OpenFailStore::list(kT0 + 200).size() == OpenFailStore::kMaxEntries);
    // The NEWEST survives the cap; the oldest is what falls off.
    CHECK(OpenFailStore::marked(QStringLiteral("id-%1").arg(OpenFailStore::kMaxEntries + 24), kT0 + 200));
    CHECK(!OpenFailStore::marked(QStringLiteral("id-0"), kT0 + 200));

    // ---- §7 THE HOT COPY ---------------------------------------------------------------------------------
    // The browse model asks marked() once per row and a console folder holds hundreds, so the store keeps its
    // parsed rows rather than re-reading and re-parsing the ini per row on the GUI thread -- the shape of the
    // shelf-lag fault this codebase has already paid for once. The price is one documented consequence, and
    // it is asserted here rather than trusted: an EXTERNAL write is invisible until invalidate().
    resetStore();
    OpenFailStore::record(QStringLiteral("hot"), QStringLiteral("H"), QStringLiteral("no data"), kT0);
    CHECK(OpenFailStore::marked(QStringLiteral("hot"), kT0));
    ini().setValue(QStringLiteral("openfail/default/items"), QStringLiteral("[]"));  // behind the store's back
    ini().sync();
    CHECK(OpenFailStore::marked(QStringLiteral("hot"), kT0));      // still hot: the cache has not been told
    OpenFailStore::invalidate();
    CHECK(!OpenFailStore::marked(QStringLiteral("hot"), kT0));     // ...and now it has
    // The store's OWN writers never need it -- a write adopts what it wrote, so the repaint that always
    // follows one does not go back to the file.
    OpenFailStore::record(QStringLiteral("hot"), QStringLiteral("H"), QStringLiteral("again"), kT0);
    CHECK(OpenFailStore::lookup(QStringLiteral("hot"), kT0).message == QStringLiteral("again"));
    OpenFailStore::clear(QStringLiteral("hot"));
    CHECK(!OpenFailStore::marked(QStringLiteral("hot"), kT0));

    resetStore();
    if (failures == 0) { std::printf("OPENFAIL-OK\n"); return 0; }
    std::fprintf(stderr, "OPENFAIL: %d failure(s)\n", failures);
    return 1;
}
