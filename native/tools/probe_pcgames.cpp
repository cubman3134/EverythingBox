// Headless check of PC-game identity and source selection. The PC folders were per-launcher; this is
// the unit that lets ONE entry carry Steam, GOG, Epic, Battle.net and a downloaded copy as sources.
//
// Two properties matter more than everything else here and are pinned hardest:
//   * normalizeTitle must NOT strip sequel numerals. Merging "Hades" with "Hades II" LOSES a game from
//     the user's library; failing to merge two editions merely shows it twice. The asymmetry is the
//     whole reason this is a probe and not a hand-wave.
//   * pickAutoSource must never return a NOT-ready source. Play is one keypress; silently starting a
//     multi-gigabyte download from it is the failure this function exists to prevent.
//
// Prints PCGAMES-OK on success; any failure prints PCGAMES-FAIL <cond> and exits non-zero.
#include "PcGameId.h"
#include "PcGameRemap.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PCGAMES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace pcgame;

static PcGameSource src(PcGameSource::Kind k, const QString& launcher, bool ready)
{
    PcGameSource s; s.kind = k; s.launcher = launcher; s.ready = ready; return s;
}

// ---- record-store fixture (§8-§11) ------------------------------------------------------------
// The remap moves records between HASHED storage keys, so the probe has to build those keys too. It
// recomputes each hash from the STORE it mirrors rather than calling a shared helper: the whole risk
// in this migration is PcGameRemap hashing a key differently from ItemMarks / ConsumptionStats /
// PlayStats / PlaybackSession, and a probe that reused the production helper could not see that — it
// would agree with the bug. These three are the three DIFFERENT shapes in play, which is itself the
// point: full MD5, SHA-1, and MD5 truncated to ten characters.
static QString gRecIni;

static QString md5Full(const QString& k)     // ItemMarks.cpp:46, ConsumptionStats.cpp:96
{ return QString::fromLatin1(QCryptographicHash::hash(k.toUtf8(), QCryptographicHash::Md5).toHex()); }
static QString sha1Full(const QString& k)    // PlayStats.cpp:29
{ return QString::fromLatin1(QCryptographicHash::hash(k.toUtf8(), QCryptographicHash::Sha1).toHex()); }
static QString md5Short(const QString& k)    // PlaybackSession.cpp:14 / HomeView.cpp:137
{ return QString::fromLatin1(QCryptographicHash::hash(k.toUtf8(), QCryptographicHash::Md5).toHex().left(10)); }

// A fresh QSettings per call. Deliberately wasteful: it makes every read a real read of the file on
// disk, so nothing here can pass on a stale in-memory copy of what the probe itself wrote.
static void     recSet(const QString& k, const QVariant& v)
{ QSettings s(gRecIni, QSettings::IniFormat); s.setValue(k, v); s.sync(); }
static QVariant recGet(const QString& k)
{ QSettings s(gRecIni, QSettings::IniFormat); return s.value(k); }
static bool     recHas(const QString& k)
{ QSettings s(gRecIni, QSettings::IniFormat); return s.contains(k); }
static QJsonObject recObj(const QString& k)
{ return QJsonDocument::fromJson(recGet(k).toString().toUtf8()).object(); }
static QJsonArray  recArr(const QString& k)
{ return QJsonDocument::fromJson(recGet(k).toString().toUtf8()).array(); }

static QString marksBlob(bool hidden, const QString& completion, const QStringList& tags, qint64 updatedAt)
{
    QJsonObject o;
    o.insert(QStringLiteral("hidden"), hidden);
    o.insert(QStringLiteral("completion"), completion);
    QJsonArray a; for (const QString& t : tags) a.append(t);
    o.insert(QStringLiteral("tags"), a);
    o.insert(QStringLiteral("updatedAt"), double(updatedAt));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

static QString statsBlob(qint64 secs, qint64 pages, qint64 lastActivity, const QString& title)
{
    QJsonObject o;
    o.insert(QStringLiteral("mediaSeconds"), double(secs));
    o.insert(QStringLiteral("pagesRead"),    double(pages));
    o.insert(QStringLiteral("lastActivity"), double(lastActivity));
    o.insert(QStringLiteral("title"),        title);
    o.insert(QStringLiteral("category"),     QStringLiteral("video"));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

static QString favJson(const QVector<QPair<QString, qint64>>& idAndTs)
{
    QJsonArray arr;
    for (const QPair<QString, qint64>& e : idAndTs)
    {
        QJsonObject o;
        o.insert(QStringLiteral("itemId"), e.first);
        o.insert(QStringLiteral("title"),  e.first);
        o.insert(QStringLiteral("ts"),     double(e.second));
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

static int favIndexOf(const QJsonArray& a, const QString& id)
{
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).toObject().value(QStringLiteral("itemId")).toString() == id) return i;
    return -1;
}

// Point the remap's store at the scratch ini AND drop its cached QSettings, so the pass that follows
// reads exactly what the probe just wrote. (QSettings notices an external change by file timestamp,
// whose resolution is coarse enough on some filesystems to miss a write made milliseconds earlier —
// re-seating the seam removes that from the equation entirely.)
static void reseatRemapStore() { setRemapIniPathForTesting(gRecIni); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // NOTE (issue #42): this probe now also gets a per-process scratch data dir, so AppPaths::dataDir() is
    // already empty at the start of every run and the redirect below is no longer what makes the run
    // hermetic. It is kept because EB_PCGAMEID_TEST_SEAM is still needed for PcGameRemap's sync counter,
    // and unpicking just the ini half from two production translation units would change no behaviour. The
    // history is worth keeping either way:
    //
    // The override store PERSISTS. Point it at a scratch ini and delete any leftover first, so a run is
    // hermetic: without this the probe writes into the app ini next to the exe and the NEXT run reads
    // what this one wrote. That is not hypothetical — it fired during the mutation pass here. A build
    // whose normalizeTitle wrongly ate "II" wrote an override key under a title no correct build ever
    // produces; the reverted, CORRECT build then read that key back and failed §4, which reads exactly
    // like "the revert did not take". Test isolation is what tells the two apart.
    const QString ini = QDir::temp().filePath(QStringLiteral("eb-probe-pcgames.ini"));
    QFile::remove(ini);
    setIniPathForTesting(ini);

    // ---- 1. normalizeTitle: noise that SHOULD collapse -----------------------------------------
    CHECK(normalizeTitle(QStringLiteral("Hades")) == normalizeTitle(QStringLiteral("HADES")));
    CHECK(normalizeTitle(QStringLiteral("Prey")) == normalizeTitle(QStringLiteral("Prey (2017)")));
    CHECK(normalizeTitle(QStringLiteral("Tomb Raider"))
          == normalizeTitle(QStringLiteral("Tomb Raider: Game of the Year Edition")));
    CHECK(normalizeTitle(QStringLiteral("Dishonored"))
          == normalizeTitle(QStringLiteral("Dishonored - Definitive Edition")));
    CHECK(normalizeTitle(QStringLiteral("BioShock"))
          == normalizeTitle(QStringLiteral("BioShock™ Remastered")));
    CHECK(normalizeTitle(QStringLiteral("Deus Ex"))
          == normalizeTitle(QStringLiteral("Deus Ex:  Director's Cut ")));

    // ---- 2. normalizeTitle: SEQUELS MUST NOT COLLAPSE ------------------------------------------
    // Each pair below is a DIFFERENT game. A merge here removes one of them from the library.
    CHECK(normalizeTitle(QStringLiteral("Hades")) != normalizeTitle(QStringLiteral("Hades II")));
    CHECK(normalizeTitle(QStringLiteral("Portal")) != normalizeTitle(QStringLiteral("Portal 2")));
    CHECK(normalizeTitle(QStringLiteral("Diablo II")) != normalizeTitle(QStringLiteral("Diablo III")));
    CHECK(normalizeTitle(QStringLiteral("Fallout 3")) != normalizeTitle(QStringLiteral("Fallout 4")));
    CHECK(normalizeTitle(QStringLiteral("The Witcher 2")) != normalizeTitle(QStringLiteral("The Witcher 3")));
    CHECK(normalizeTitle(QStringLiteral("Civilization V")) != normalizeTitle(QStringLiteral("Civilization VI")));
    // A numeral is NOT edition noise even next to edition noise.
    CHECK(normalizeTitle(QStringLiteral("Diablo II: Resurrected"))
          != normalizeTitle(QStringLiteral("Diablo III: Reaper of Souls")));
    // ...but the SAME sequel across two editions still collapses.
    CHECK(normalizeTitle(QStringLiteral("Portal 2"))
          == normalizeTitle(QStringLiteral("Portal 2 - Game of the Year Edition")));

    // ---- 2b. the edition strip is a SUFFIX rule, not a search-and-destroy ----------------------
    // Both shapes below were measured against the unanchored strip and both were wrong.
    //
    // Under-merge: "Portal 2: The Definitive Edition Remastered" normalised to "portal 2 the" — the
    // inner phrase went, the article it was attached to did not, and the result then failed to merge
    // with plain "Portal 2". Safe direction, but it is a real store title shape, so it is pinned.
    CHECK(normalizeTitle(QStringLiteral("Portal 2: The Definitive Edition Remastered"))
          == normalizeTitle(QStringLiteral("Portal 2")));
    CHECK(normalizeTitle(QStringLiteral("Portal 2: The Definitive Edition Remastered"))
          == QStringLiteral("portal 2"));
    // Over-merge (the dangerous mirror): "Remastered" sitting MID-title is part of the product name,
    // not noise on the end of it. Eating it made "Command & Conquer Remastered Collection" collide
    // with the genuinely different "Command & Conquer Collection" — a merge, i.e. a lost game.
    CHECK(normalizeTitle(QStringLiteral("Command & Conquer Remastered Collection"))
          == QStringLiteral("command conquer remastered collection"));
    CHECK(normalizeTitle(QStringLiteral("Command & Conquer Remastered Collection"))
          != normalizeTitle(QStringLiteral("Command & Conquer Collection")));
    // The suffix anchor must not cost the ordinary cases: noise at the end still goes, including two
    // phrases stacked up, and a title whose ONLY content is noise still collapses to empty (which §5d
    // then has to give a private key to).
    CHECK(normalizeTitle(QStringLiteral("Dishonored - Definitive Edition Remastered"))
          == QStringLiteral("dishonored"));
    // The dangling-article drop is CONDITIONAL on a phrase actually having been stripped, so a title
    // that simply ends in an article keeps it. Without the condition this rule would start editing
    // ordinary titles, which is the same over-merge direction as the unanchored strip.
    CHECK(normalizeTitle(QStringLiteral("Danganronpa: Trigger Happy Havoc The"))
          == QStringLiteral("danganronpa trigger happy havoc the"));
    // ...and the sequel numeral is still untouched by any of it, including with noise stacked on.
    CHECK(normalizeTitle(QStringLiteral("Diablo III: The Definitive Edition"))
          != normalizeTitle(QStringLiteral("Diablo II: The Definitive Edition")));

    // ---- 3. degenerate input ------------------------------------------------------------------
    CHECK(normalizeTitle(QString()).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("   ")).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("!!!")).isEmpty());
    // A title made ENTIRELY of edition noise normalises to nothing at all. That is correct for this
    // function and lethal for grouping — see §5d.
    CHECK(normalizeTitle(QStringLiteral("GOTY")).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("Enhanced Edition")).isEmpty());

    // ---- 4. sameGame: IGDB wins when BOTH sides have one ---------------------------------------
    // Same igdb id, wildly different titles -> same game (a regional rename).
    CHECK(sameGame(QStringLiteral("Rockman"), QStringLiteral("igdb:100"),
                   QStringLiteral("Mega Man"), QStringLiteral("igdb:100")) == true);
    // DIFFERENT igdb ids -> NOT the same game, even when the titles normalise identically. This is the
    // case where trusting the title would merge two genuinely distinct releases.
    CHECK(sameGame(QStringLiteral("Prey"), QStringLiteral("igdb:1"),
                   QStringLiteral("Prey"), QStringLiteral("igdb:2")) == false);
    // Only ONE side has an id -> fall back to the title, do not treat the missing id as a mismatch.
    CHECK(sameGame(QStringLiteral("Hades"), QStringLiteral("igdb:7"),
                   QStringLiteral("Hades"), QString()) == true);
    CHECK(sameGame(QStringLiteral("Hades"), QString(),
                   QStringLiteral("Hades II"), QString()) == false);

    // ---- 5. the override store beats BOTH heuristics -------------------------------------------
    {
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), true);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == true);
        // symmetric — the user said these two are the same, in either order
        CHECK(overrideSaysSame(QStringLiteral("hades ii"), QStringLiteral("hades")) == true);
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), false);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == false);
    }

    // ---- 5b. ...and sameGame consults it FIRST, ahead of both heuristics -----------------------
    // Added beyond the brief's table: the §5 block above exercises the store directly, so it cannot
    // observe WHERE sameGame consults it. Without these four checks, moving the override lookup to the
    // END of sameGame (mutation #6) changes no observable behaviour and the precedence rule that makes
    // the escape hatch usable is untested. Both directions are pinned: an override that says SAME must
    // beat disagreeing titles, and one that says NOT-SAME must beat two matching igdb ids.
    {
        // The titles say "different game"; the user says otherwise, and the user wins.
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), true);
        CHECK(sameGame(QStringLiteral("Hades"), QString(),
                       QStringLiteral("Hades II"), QString()) == true);
        // Symmetric through sameGame too, not just through the raw store.
        CHECK(sameGame(QStringLiteral("Hades II"), QString(),
                       QStringLiteral("Hades"), QString()) == true);
        // And a negative verdict beats even two IDENTICAL igdb ids — this is the user undoing a merge
        // the metadata provider itself got wrong, which is the case the ids can never fix themselves.
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), false);
        CHECK(sameGame(QStringLiteral("Hades"), QStringLiteral("igdb:7"),
                       QStringLiteral("Hades II"), QStringLiteral("igdb:7")) == false);
        // The stored NO also has to survive the title heuristic agreeing with it (belt and braces:
        // proves the lookup ran, not that the fallback happened to return the same answer).
        CHECK(sameGame(QStringLiteral("Hades"), QString(),
                       QStringLiteral("Hades II"), QString()) == false);
    }

    // ---- 5e. the verdict STORE: listing, the self-pair, and clearing vs storing "no" ------------
    // §5/§5b prove sameGame consults a verdict. They cannot see the two things the UI needs of the
    // store itself: that a verdict can be ENUMERATED (a "same" verdict is a graph edge, so the id
    // builder has to find what a title was fused WITH, which no pair lookup can answer) and that
    // FORGETTING a verdict is a distinct state from storing a negative one. Without the second, a user
    // who separates the wrong pair has no way back — "undo" would store "not the same" forever.
    {
        // Start from a known state: §5b left a stored verdict on (hades, hades ii).
        clearOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"));
        CHECK(overrideSaysSeparate(QStringLiteral("prey")) == false);

        // THE SELF-PAIR. Two copies of a wrongly merged key normalise to the SAME string — that is why
        // they fused — so "these are not the same game" has no second key to name and is recorded
        // against (norm, norm). setOverride normalises both sides, so the caller passes raw titles.
        setOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"), false);
        CHECK(overrideSaysSeparate(QStringLiteral("prey")) == true);
        CHECK(overrideSaysSeparate(QStringLiteral("Prey (2017)")) == true);   // normalises its argument
        // ...and it does NOT leak to any other key. A separate verdict that quietly applied to the whole
        // library would split every game, which passes any "this entry appears once" check while
        // destroying the folder.
        CHECK(overrideSaysSeparate(QStringLiteral("hades")) == false);
        CHECK(overrideSaysSeparate(QStringLiteral("portal 2")) == false);
        CHECK(overrideSaysSeparate(QString()) == false);

        // The listing carries it, with both sides normalised and the verdict itself readable.
        {
            int preyRows = 0;
            for (const MergeVerdict& v : overrides())
                if (v.a == QStringLiteral("prey") && v.b == QStringLiteral("prey"))
                { ++preyRows; CHECK(v.same == false); }
            CHECK(preyRows == 1);
            // The listing is not empty for the WRONG reason: §5b's (hades, hades ii) row is in it too,
            // so "found it" above is a real find and not a builder that returns every possible pair.
            bool sawHades = false;
            for (const MergeVerdict& v : overrides())
                if (v.a == QStringLiteral("hades") || v.b == QStringLiteral("hades")) sawHades = true;
            CHECK(sawHades);
            for (const MergeVerdict& v : overrides()) { CHECK(!v.a.isEmpty()); CHECK(!v.b.isEmpty()); }
        }

        // CLEARING is not storing "no". After a clear the pair is absent from the listing entirely and
        // the heuristic decides again; after storing "no" it is present and keeps winning.
        clearOverride(QStringLiteral("prey"), QStringLiteral("prey"));
        CHECK(overrideSaysSeparate(QStringLiteral("prey")) == false);
        for (const MergeVerdict& v : overrides())
            CHECK(!(v.a == QStringLiteral("prey") && v.b == QStringLiteral("prey")));
        // The clear is SCOPED — it did not empty the store.
        {
            bool sawHades = false;
            for (const MergeVerdict& v : overrides())
                if (v.a == QStringLiteral("hades") || v.b == QStringLiteral("hades")) sawHades = true;
            CHECK(sawHades);
        }
    }

    // ---- 5f. separationTag: the year comes back, the edition noise does NOT ---------------------
    // The key a copy keeps once its group is separated. Getting this wrong in the obvious way — keying
    // on the raw title — turns a two-way over-merge into a three-entry library, because the edition
    // variant separates too. That is the same class of harm (a game the user cannot find where they
    // expect it) arriving through the cure, so it is pinned here rather than left to the id checks.
    {
        // The year the merge key threw away is exactly what comes back.
        CHECK(separationTag(QStringLiteral("Prey (2017)")) != separationTag(QStringLiteral("Prey")));
        CHECK(separationTag(QStringLiteral("Prey")) == normalizeTitle(QStringLiteral("Prey")));
        CHECK(separationTag(QStringLiteral("Prey (2017)")).contains(QStringLiteral("2017")));
        // Edition noise still collapses — otherwise separating "Prey" would also split its GOTY edition.
        CHECK(separationTag(QStringLiteral("Prey: Game of the Year Edition"))
              == separationTag(QStringLiteral("Prey")));
        CHECK(separationTag(QStringLiteral("Prey (2017) Definitive Edition"))
              == separationTag(QStringLiteral("Prey (2017)")));
        // Sequels are still different games here too (the numeral rule survives the year being kept).
        CHECK(separationTag(QStringLiteral("Hades")) != separationTag(QStringLiteral("Hades II")));
        // Case and padding are not identity, same as every other key in this unit.
        CHECK(separationTag(QStringLiteral("  PREY (2017) ")) == separationTag(QStringLiteral("Prey (2017)")));
    }

    // ---- 5g. rankMergeCandidates: the picker's order, and what it must NEVER do ------------------
    // The "this is the same game as…" picker's honest list is every OTHER entry in the library, and the two
    // spellings being joined are similar by definition — alphabetical order buries the one row the action
    // is about. This orders; it must not filter, because a user joining two titles that look nothing alike
    // still has to be able to reach the row.
    {
        // The fixture is chosen so ALPHABETICAL ORDER AND RANKED ORDER DISAGREE. The obvious list —
        // Zork / Hades / Final Fantasy VII / Final Fantasy X — sorts alphabetically into exactly the ranked
        // order, so every check below passed against a build that ignored the score entirely (measured: it
        // survived the mutation). "Alpha Protocol" and "Baldur's Gate" are here for that and nothing else:
        // they sort BEFORE the Final Fantasy rows and must still rank after them.
        const QStringList others{ QStringLiteral("Zork"), QStringLiteral("Final Fantasy VII"),
                                  QStringLiteral("Alpha Protocol"), QStringLiteral("Final Fantasy X"),
                                  QStringLiteral("Baldur's Gate") };
        const QStringList r = rankMergeCandidates(QStringLiteral("Final Fantasy VII Remake"), others);
        // NOTHING IS DROPPED. Checked first and by count AND by membership: a build that returned only the
        // close matches would pass every ordering check below while hiding rows the user needs.
        CHECK(r.size() == others.size());
        for (const QString& o : others) CHECK(r.contains(o));
        // The closest spelling leads.
        CHECK(r.at(0) == QStringLiteral("Final Fantasy VII"));
        // More shared leading words beats fewer: "Final Fantasy VII" (3) over "Final Fantasy X" (2).
        CHECK(r.indexOf(QStringLiteral("Final Fantasy VII")) < r.indexOf(QStringLiteral("Final Fantasy X")));
        // ...and ANY shared prefix beats none EVEN WHEN THE ALPHABET DISAGREES — "Alpha Protocol" and
        // "Baldur's Gate" both sort before "Final Fantasy", and both must still come after it.
        CHECK(r.indexOf(QStringLiteral("Final Fantasy X")) < r.indexOf(QStringLiteral("Alpha Protocol")));
        CHECK(r.indexOf(QStringLiteral("Final Fantasy X")) < r.indexOf(QStringLiteral("Baldur's Gate")));
        // Ties (all score 0) fall back to the alphabet.
        CHECK(r.indexOf(QStringLiteral("Alpha Protocol")) < r.indexOf(QStringLiteral("Baldur's Gate")));
        CHECK(r.indexOf(QStringLiteral("Baldur's Gate")) < r.indexOf(QStringLiteral("Zork")));
        // TOTAL order, so an unchanged library gives an unchanged picker: the same set in a different input
        // order comes back identical.
        QStringList shuffled; for (int i = others.size() - 1; i >= 0; --i) shuffled << others.at(i);
        CHECK(rankMergeCandidates(QStringLiteral("Final Fantasy VII Remake"), shuffled) == r);
        // A title with nothing in common with anything still returns every row, in alphabetical order.
        const QStringList none = rankMergeCandidates(QStringLiteral("!!!"), others);
        CHECK(none.size() == others.size());
        CHECK(none.at(0) == QStringLiteral("Alpha Protocol"));   // alphabetical, no prefix bonus anywhere
    }

    // ---- 5c. mergeKey: the igdb id when there is one, else the normalised title -----------------
    {
        CHECK(mergeKey(QStringLiteral("Hades"), QStringLiteral("igdb:7")) == QStringLiteral("igdb:7"));
        CHECK(mergeKey(QStringLiteral("Hades II"), QString()) == normalizeTitle(QStringLiteral("Hades II")));
        // Two editions of one game group together; a sequel does NOT join them.
        CHECK(mergeKey(QStringLiteral("Portal 2"), QString())
              == mergeKey(QStringLiteral("Portal 2 - Game of the Year Edition"), QString()));
        CHECK(mergeKey(QStringLiteral("Portal"), QString()) != mergeKey(QStringLiteral("Portal 2"), QString()));
    }

    // ---- 5d. mergeKey: an EMPTY normalised title must not become one giant bucket ---------------
    // sameGame already refuses to match two empty normalised titles (`!na.isEmpty() && na == nb`).
    // mergeKey had no such guard, and the header defines grouping as "two entries group together iff
    // their mergeKey matches" — so every id-less entry that normalises to empty shared ONE key and the
    // catalog builder would fuse them all into a single tile. That is the same class of harm as the
    // sequel-numeral rule prevents (games vanish from the library), arriving by a different door, so
    // the two functions are made to agree here rather than in the caller.
    {
        // Punctuation-only titles: both normalise to empty, and must still be distinct entries.
        CHECK(normalizeTitle(QStringLiteral("!!!")) == normalizeTitle(QStringLiteral("?!?")));  // both empty
        CHECK(mergeKey(QStringLiteral("!!!"), QString()) != mergeKey(QStringLiteral("?!?"), QString()));
        // All-edition-noise titles: same story via a completely different route.
        CHECK(mergeKey(QStringLiteral("GOTY"), QString())
              != mergeKey(QStringLiteral("Enhanced Edition"), QString()));
        CHECK(mergeKey(QStringLiteral("GOTY"), QString()) != mergeKey(QStringLiteral("!!!"), QString()));
        // The fallback key is still a KEY: the same entry, keyed twice, groups with itself. (A random
        // or counter-based fallback would pass the two checks above and break this one.)
        CHECK(mergeKey(QStringLiteral("GOTY"), QString()) == mergeKey(QStringLiteral("GOTY"), QString()));
        // ...and it can never be mistaken for a real normalised title or a provider id.
        CHECK(!mergeKey(QStringLiteral("GOTY"), QString()).isEmpty());
        CHECK(mergeKey(QStringLiteral("GOTY"), QString()) != mergeKey(QStringLiteral("Hades"), QString()));
        CHECK(mergeKey(QStringLiteral("GOTY"), QString()) != normalizeTitle(QStringLiteral("GOTY")));
        // An entry WITH an igdb id is untouched by all of this — the id still wins outright, even when
        // the title is pure noise, so two noise-titled entries sharing an id still group.
        CHECK(mergeKey(QStringLiteral("GOTY"), QStringLiteral("igdb:7")) == QStringLiteral("igdb:7"));
        CHECK(mergeKey(QStringLiteral("!!!"), QStringLiteral("igdb:7"))
              == mergeKey(QStringLiteral("?!?"), QStringLiteral("igdb:7")));
    }

    // ---- 6. pickAutoSource: NEVER returns a not-ready source ------------------------------------
    {
        // exactly one ready -> launch it
        QVector<PcGameSource> one{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                   src(PcGameSource::Downloaded, QString(), true),
                                   src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(one) == 1);
        CHECK(one.value(pickAutoSource(one)).ready == true);

        // several ready -> ask
        QVector<PcGameSource> many{ src(PcGameSource::LauncherInstalled, QStringLiteral("steam"), true),
                                    src(PcGameSource::Downloaded, QString(), true) };
        CHECK(pickAutoSource(many) == -1);

        // none ready -> ask, NEVER auto-start a download
        QVector<PcGameSource> none{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                    src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(none) == -1);

        // empty -> ask (and must not index out of range)
        CHECK(pickAutoSource({}) == -1);
    }

    // ---- 7. the remap table: old per-launcher id -> merged id ----------------------------------
    {
        QVector<QPair<QString, QString>> lib;      // (old id, title)
        lib << qMakePair(QStringLiteral("steam:1145360"), QStringLiteral("Hades"))
            << qMakePair(QStringLiteral("gog:1207658930"), QStringLiteral("Hades"))
            << qMakePair(QStringLiteral("steam:2074920"), QStringLiteral("Hades II"));
        const QHash<QString, QString> t = remapTable(lib);

        // Both Hades entries land on the SAME merged id...
        CHECK(t.value(QStringLiteral("steam:1145360")) == t.value(QStringLiteral("gog:1207658930")));
        CHECK(!t.value(QStringLiteral("steam:1145360")).isEmpty());
        // ...and Hades II lands on a DIFFERENT one. A table that merges these loses a game.
        CHECK(t.value(QStringLiteral("steam:2074920")) != t.value(QStringLiteral("steam:1145360")));

        // IDEMPOTENT: feeding the already-merged ids back yields ids that map to themselves.
        QVector<QPair<QString, QString>> again;
        again << qMakePair(t.value(QStringLiteral("steam:1145360")), QStringLiteral("Hades"));
        const QHash<QString, QString> t2 = remapTable(again);
        CHECK(t2.value(t.value(QStringLiteral("steam:1145360")))
              == t.value(QStringLiteral("steam:1145360")));

        // An id the table cannot map is ABSENT from the table — never mapped to empty, which a caller
        // would happily write as a key and thereby destroy the record.
        CHECK(!t.contains(QStringLiteral("steam:999999")));
        CHECK(t.value(QStringLiteral("steam:999999")).isEmpty());   // value() default, not an entry

        // The merged id is the one the CATALOG builds, and it is built by the SAME function the catalog
        // calls — pcgame::itemId. Records moved to any other spelling are records nothing will ever look
        // for again, so this equality is the whole point of that function existing.
        CHECK(t.value(QStringLiteral("steam:2074920")) == itemId(QStringLiteral("Hades II")));
        CHECK(!t.value(QStringLiteral("steam:2074920")).startsWith(QStringLiteral("pcgame:pcgame:")));
        // (probe_browse pins the other half: that the catalog builder's tile ids and these destinations
        // are the same strings for the same library. It is the only probe that can build both sides.)
    }

    // ---- 7c. pcgame::itemId is the ONE id builder, and it is TITLE-ONLY on purpose ------------------
    // The remap used to take a title->igdb map and prefer the id it supplied, while the catalog keyed on
    // the title alone; a populated map would have moved every record onto an id no lookup ever performs.
    // The parameter is gone — a caller with metadata gets a compile error, not silent data loss — and the
    // decision that the id is title-only is recorded HERE so that reversing it has to reckon with a test.
    {
        // Exactly "pcgame:" + the title-only merge key, with no second prefix when mergeKey already
        // returned its namespaced raw-title fallback.
        CHECK(itemId(QStringLiteral("Hades II"))
              == QStringLiteral("pcgame:") + mergeKey(QStringLiteral("Hades II"), QString()));
        CHECK(itemId(QStringLiteral("GOTY")) == mergeKey(QStringLiteral("GOTY"), QString()));
        CHECK(!itemId(QStringLiteral("GOTY")).startsWith(QStringLiteral("pcgame:pcgame:")));
        // Editions collapse, sequels do not — the same rules as the merge key, reached through the id.
        CHECK(itemId(QStringLiteral("Portal 2"))
              == itemId(QStringLiteral("Portal 2 - Game of the Year Edition")));
        CHECK(itemId(QStringLiteral("Hades")) != itemId(QStringLiteral("Hades II")));
        // Whitespace is not identity: the catalog trims before grouping, so the id function must too, or
        // one launcher's padded spelling becomes a second tile with its own records.
        CHECK(itemId(QStringLiteral("  Hades  ")) == itemId(QStringLiteral("Hades")));
        // Nothing to group on -> NO id (rule 1), never the bare "pcgame:rawtitle/" bucket that every
        // nameless entry would otherwise share.
        CHECK(itemId(QString()).isEmpty());
        CHECK(itemId(QStringLiteral("   ")).isEmpty());
        // And the remap agrees, because it does not compute this itself.
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(QStringLiteral("steam:620"), QStringLiteral("Portal 2"))
            << qMakePair(QStringLiteral("gog:42"),    QStringLiteral("  Portal 2  "));
        const QHash<QString, QString> t = remapTable(lib);
        CHECK(t.value(QStringLiteral("steam:620")) == itemId(QStringLiteral("Portal 2")));
        CHECK(t.value(QStringLiteral("gog:42"))    == itemId(QStringLiteral("Portal 2")));
    }

    // ---- 7b. the two mutations the §7 fixture cannot actually feel -----------------------------
    // Measured, not assumed. Neither mutation in the brief's table fails against §7 as written:
    //   #10 "map empty-title entries to ''" — §7's library contains no empty-title entry at all, so the
    //       mutated branch is never reached and every §7 check still passes. The brief's unmappable-id
    //       check ("steam:999999") tests an id that was never IN the library, which is a different
    //       thing: it can only ever pass. So the library below actually contains the empty titles.
    //   #11 "group on the raw title" — §7's three raw titles are already pairwise different, so raw-title
    //       grouping happens to separate Hades from Hades II too. The brief flags this; the fixture that
    //       does fail is TWO EDITIONS of one game, whose raw titles differ but whose merged id must not.
    // An inert mutation is not coverage, so both cases are pinned here rather than left implied.
    {
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(QStringLiteral("steam:5000"), QString())                 // no title at all
            << qMakePair(QStringLiteral("steam:5001"), QStringLiteral("   "))     // whitespace only
            << qMakePair(QStringLiteral("steam:620"),  QStringLiteral("Portal 2"))
            << qMakePair(QStringLiteral("gog:1207659110"),
                         QStringLiteral("Portal 2 - Game of the Year Edition"));
        const QHash<QString, QString> t = remapTable(lib);

        // #10: an entry with nothing to group on is ABSENT — not present-with-an-empty-value. The
        // distinction is the whole safety property: applyRemap would hash "" and rewrite the record
        // under a key shared by every other nameless entry.
        CHECK(!t.contains(QStringLiteral("steam:5000")));
        CHECK(!t.contains(QStringLiteral("steam:5001")));
        // ...and no entry ANYWHERE in the table carries an empty destination.
        for (auto it = t.cbegin(); it != t.cend(); ++it) CHECK(!it.value().isEmpty());

        // #11: two editions of ONE game — different raw titles, SAME merged id. Grouping on the raw
        // title splits these, which leaves the second copy's play time stranded under an id the catalog
        // no longer builds.
        CHECK(t.value(QStringLiteral("steam:620")) == t.value(QStringLiteral("gog:1207659110")));
        CHECK(!t.value(QStringLiteral("steam:620")).isEmpty());
        // (The old "an igdb id wins outright" check lived here. It was the bug: it asserted the remap
        // building an id — "pcgame:igdb:7" — that pcGamesCatalog never builds, so passing it meant the
        // records were unreachable. See §7c for what replaced it, and the header for why.)
    }

    // ---- 7d. effectiveItemId: the escape hatch, spent in the ONE place identity is minted ---------
    // itemId is the pure base and stays title-only (§7c). This is the function the CATALOG groups on and
    // the REMAP moves records onto, and the reason it is one function is unchanged from §7c: two
    // implementations of "what id does this title get" disagree silently, and every record then lands on
    // a key no lookup performs.
    //
    // THE TRAP THIS SECTION IS WRITTEN AGAINST: "the game appears once" passes just as happily under a
    // build that fuses the WHOLE library into one entry. Every check below therefore names the identity
    // it expects and pins the games that must NOT move — the sequels most of all, because merging
    // "Hades" with "Hades II" deletes a game from the library, which is worse than showing one twice.
    {
        // 1. With no verdict about them, effectiveItemId IS itemId — for a normal title, for one that
        //    normalises to nothing (mergeKey's private fallback), and for the two no-id cases.
        CHECK(effectiveItemId(QStringLiteral("Hades")) == itemId(QStringLiteral("Hades")));
        CHECK(effectiveItemId(QStringLiteral("Portal 2")) == itemId(QStringLiteral("Portal 2")));
        CHECK(effectiveItemId(QStringLiteral("GOTY")) == itemId(QStringLiteral("GOTY")));
        CHECK(effectiveItemId(QString()).isEmpty());
        CHECK(effectiveItemId(QStringLiteral("   ")).isEmpty());

        // 2. SEPARATE — the case the design named. The premise first: these two really do fuse today.
        CHECK(itemId(QStringLiteral("Prey")) == itemId(QStringLiteral("Prey (2017)")));
        setOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"), false);
        CHECK(effectiveItemId(QStringLiteral("Prey")) != effectiveItemId(QStringLiteral("Prey (2017)")));
        // Both halves are REAL ids, not one game plus an empty string (rule 1: an empty id is dropped
        // from the folder and absent from the remap, so "they differ" alone would pass on a build that
        // deletes one of them).
        CHECK(!effectiveItemId(QStringLiteral("Prey")).isEmpty());
        CHECK(!effectiveItemId(QStringLiteral("Prey (2017)")).isEmpty());
        CHECK(effectiveItemId(QStringLiteral("Prey")).startsWith(QStringLiteral("pcgame:")));
        CHECK(effectiveItemId(QStringLiteral("Prey (2017)")).startsWith(QStringLiteral("pcgame:")));
        // STABLE per title — the id is re-derived from scratch on every scan, so a copy whose id moved
        // between two refreshes would strand its own records.
        CHECK(effectiveItemId(QStringLiteral("Prey (2017)"))
              == effectiveItemId(QStringLiteral("  Prey (2017)  ")));
        CHECK(effectiveItemId(QStringLiteral("Prey")) == effectiveItemId(QStringLiteral("prey")));
        // Two EDITIONS of the separated copy still fuse. Keying a separated group on the raw title would
        // split these too, which is the over-merge cured by turning it into an over-split.
        CHECK(effectiveItemId(QStringLiteral("Prey: Game of the Year Edition"))
              == effectiveItemId(QStringLiteral("Prey")));
        // SCOPED to the key the user pointed at. Everything else is byte-identical to its base id — this
        // is the check that fails on a build which separates (or fuses) the whole library.
        CHECK(effectiveItemId(QStringLiteral("Hades")) == itemId(QStringLiteral("Hades")));
        CHECK(effectiveItemId(QStringLiteral("Hades II")) == itemId(QStringLiteral("Hades II")));
        CHECK(effectiveItemId(QStringLiteral("Portal")) == itemId(QStringLiteral("Portal")));
        CHECK(effectiveItemId(QStringLiteral("Portal 2")) == itemId(QStringLiteral("Portal 2")));
        // ...and the sequel pairs are still two games, reached through the overridden builder.
        CHECK(effectiveItemId(QStringLiteral("Hades")) != effectiveItemId(QStringLiteral("Hades II")));
        CHECK(effectiveItemId(QStringLiteral("Portal")) != effectiveItemId(QStringLiteral("Portal 2")));
        // Neither separated half collides with an unrelated game.
        CHECK(effectiveItemId(QStringLiteral("Prey")) != effectiveItemId(QStringLiteral("Hades")));
        CHECK(effectiveItemId(QStringLiteral("Prey (2017)")) != effectiveItemId(QStringLiteral("Hades")));

        // 3. THE REMAP FOLLOWS. This is the half that makes the override safe rather than merely
        //    visible: the folder now builds two tiles, and the table has to send each copy's records to
        //    the tile that copy is actually on. A remap left on the pure base id would put both on a
        //    tile the catalog no longer builds — the silent stranding this whole unit exists to prevent.
        {
            QVector<QPair<QString, QString>> lib;
            lib << qMakePair(QStringLiteral("steam:3970"),  QStringLiteral("Prey"))
                << qMakePair(QStringLiteral("steam:480490"), QStringLiteral("Prey (2017)"))
                << qMakePair(QStringLiteral("steam:1145360"), QStringLiteral("Hades"));
            const QHash<QString, QString> t = remapTable(lib);
            CHECK(t.value(QStringLiteral("steam:3970"))   == effectiveItemId(QStringLiteral("Prey")));
            CHECK(t.value(QStringLiteral("steam:480490")) == effectiveItemId(QStringLiteral("Prey (2017)")));
            CHECK(t.value(QStringLiteral("steam:3970")) != t.value(QStringLiteral("steam:480490")));
            // The untouched game's destination did not move at all.
            CHECK(t.value(QStringLiteral("steam:1145360")) == itemId(QStringLiteral("Hades")));
        }
        clearOverride(QStringLiteral("Prey"), QStringLiteral("Prey (2017)"));
        // Cleared -> the heuristic decides again and the two fuse, exactly as before the verdict.
        CHECK(effectiveItemId(QStringLiteral("Prey")) == effectiveItemId(QStringLiteral("Prey (2017)")));
        CHECK(effectiveItemId(QStringLiteral("Prey")) == itemId(QStringLiteral("Prey")));

        // 4. FUSE — the other direction. Two spellings the title heuristic leaves apart, joined by hand.
        const QString ffLong  = QStringLiteral("Final Fantasy VII Remake");
        const QString ffShort = QStringLiteral("FF7 Remake");
        CHECK(itemId(ffLong) != itemId(ffShort));                      // the premise
        setOverride(ffLong, ffShort, true);
        CHECK(effectiveItemId(ffLong) == effectiveItemId(ffShort));
        CHECK(!effectiveItemId(ffLong).isEmpty());
        // The canonical id is the SAME from either side and does not depend on which one was asked
        // first — an id that depended on the lookup order would move under the records it holds.
        CHECK(effectiveItemId(ffShort) == effectiveItemId(ffLong));
        // It is one of the two real keys, not some third invented string that no tile would carry.
        CHECK(effectiveItemId(ffLong) == itemId(ffShort) || effectiveItemId(ffLong) == itemId(ffLong));
        // Fusing two keys must not drag anything else in.
        CHECK(effectiveItemId(QStringLiteral("Hades")) != effectiveItemId(ffLong));
        CHECK(effectiveItemId(QStringLiteral("Hades")) == itemId(QStringLiteral("Hades")));
        CHECK(effectiveItemId(QStringLiteral("Hades")) != effectiveItemId(QStringLiteral("Hades II")));
        {
            QVector<QPair<QString, QString>> lib;
            lib << qMakePair(QStringLiteral("steam:1462040"), ffLong)
                << qMakePair(QStringLiteral("epic:ff7r"),     ffShort)
                << qMakePair(QStringLiteral("steam:1145360"), QStringLiteral("Hades"));
            const QHash<QString, QString> t = remapTable(lib);
            // Both copies' records go to the ONE tile the folder now builds for them.
            CHECK(t.value(QStringLiteral("steam:1462040")) == t.value(QStringLiteral("epic:ff7r")));
            // Asserted on the side whose id ACTUALLY MOVES. The fused key is the smaller of the two, which
            // here is ffShort's own — so "the short spelling's destination is its effective id" holds even
            // on a remap that ignores overrides entirely, and pinned that way it was inert (measured). The
            // long spelling is the one whose records have somewhere new to go.
            CHECK(t.value(QStringLiteral("steam:1462040")) == effectiveItemId(ffLong));
            // Same reason for the same side: rule 1 DROPS an entry whose destination came back empty, so
            // "there is a destination at all" is only a real check on the copy whose id the verdict moves.
            CHECK(!t.value(QStringLiteral("steam:1462040")).isEmpty());
            // ...and the untouched game did NOT join them. Without this, a build that maps everything to
            // one destination passes every check above.
            CHECK(t.value(QStringLiteral("steam:1145360")) != t.value(QStringLiteral("epic:ff7r")));
            CHECK(t.value(QStringLiteral("steam:1145360")) == itemId(QStringLiteral("Hades")));
        }
        clearOverride(ffLong, ffShort);
        CHECK(effectiveItemId(ffLong) != effectiveItemId(ffShort));   // back to two games

        // 5. Leave the store exactly as §7d found it (only §5b's (hades, hades ii) verdict), so the
        //    record-store sections below are not reading state this one invented. A probe whose later
        //    sections depend on an earlier one's leftovers is the failure mode the header's note about
        //    the persisting override store already records once.
        CHECK(effectiveItemId(QStringLiteral("Prey")) == itemId(QStringLiteral("Prey")));
        for (const MergeVerdict& v : overrides())
            CHECK(v.a == QStringLiteral("hades") || v.b == QStringLiteral("hades"));
    }

    // ---- 7e. A TITLE THAT IS NOT A NORMALISATION FIXED POINT ------------------------------------
    //
    // Every fixture above this one — Prey, Hades, FF7 Remake, Portal 2 — is a fixed point of
    // normalizeTitle, so none of them can see a second normalisation happen. That is not a gap in the
    // assertions; it is a gap in the INPUTS, and it is what let two unrecoverable defects ship with a
    // 44/44 mutation kill rate.
    //
    // The shape: the edition-phrase strip (step 3) runs BEFORE punctuation becomes space (step 4),
    // because the phrases carry punctuation themselves ("director's cut"). So an edition phrase wrapped
    // in brackets survives the strip, and only the NEXT pass sees it:
    //
    //     "Batman Arkham City (GOTY)"  ->  "batman arkham city goty"  ->  "batman arkham city"
    //
    // Bracketed edition suffixes are routine in downloaded release names, which is a population this
    // folder explicitly ingests. Anything that normalises a key it was handed ALREADY NORMALISED builds a
    // key nothing ever wrote, and every read and remove against it silently misses.
    {
        const QString gotyRaw = QStringLiteral("Batman Arkham City (GOTY)");
        const QString gogRaw  = QStringLiteral("Batman: Arkham City");

        // THE PREMISE, pinned so this fixture cannot quietly stop testing what it is named for. If a
        // later change makes normalizeTitle idempotent, this CHECK fails loudly and whoever made that
        // change gets told that the fixture guarding the double-normalisation bug no longer reproduces
        // its precondition — which is the moment to decide about migrating existing ids, not later.
        const QString n1 = normalizeTitle(gotyRaw);
        const QString n2 = normalizeTitle(n1);
        CHECK(n1 == QStringLiteral("batman arkham city goty"));
        CHECK(n2 == QStringLiteral("batman arkham city"));
        CHECK(n1 != n2);                                   // the whole point: not a fixed point
        CHECK(normalizeTitle(gogRaw) == n2);               // ...and the second pass lands on the OTHER copy
        CHECK(itemId(gotyRaw) != itemId(gogRaw));          // so the heuristic leaves them apart

        // 1. FUSE, then UNDO THE WAY THE UI UNDOES IT — from the STORED keys overrides() hands back.
        setOverride(gogRaw, gotyRaw, true);
        CHECK(effectiveItemId(gogRaw) == effectiveItemId(gotyRaw));
        CHECK(!effectiveItemId(gotyRaw).isEmpty());
        // The verdict is stored under the SINGLY-normalised keys, which is the form the fuse path matches.
        {
            bool sawPair = false;
            for (const MergeVerdict& v : overrides())
                if ((v.a == n1 && v.b == n2) || (v.a == n2 && v.b == n1)) { sawPair = true; CHECK(v.same); }
            CHECK(sawPair);
        }
        // The remap agrees with the folder — both copies' records go to the one tile.
        {
            QVector<QPair<QString, QString>> lib;
            lib << qMakePair(QStringLiteral("gog:1234"), gogRaw)
                << qMakePair(QStringLiteral("dl:batman"), gotyRaw);
            const QHash<QString, QString> t = remapTable(lib);
            CHECK(t.value(QStringLiteral("gog:1234")) == t.value(QStringLiteral("dl:batman")));
            CHECK(t.value(QStringLiteral("dl:batman")) == effectiveItemId(gotyRaw));
        }

        // UNDO. clearOverrideKeys takes the stored keys verbatim; the raw-title entry point would
        // re-normalise "batman arkham city goty" into "batman arkham city", remove a pair nobody wrote
        // (QSettings::remove no-ops), and report success while the fuse survived every later scan with
        // the loser side's favourites, marks and play time stranded under an id nothing looks up again.
        for (const MergeVerdict& v : overrides())
            if (v.a == n1 || v.b == n1) clearOverrideKeys(v.a, v.b);
        // THE GROUPING ACTUALLY RETURNED — asserted on the ids, not on the absence of a toast.
        CHECK(effectiveItemId(gogRaw) != effectiveItemId(gotyRaw));
        CHECK(effectiveItemId(gotyRaw) == itemId(gotyRaw));
        CHECK(effectiveItemId(gogRaw) == itemId(gogRaw));
        // ...and the verdict is gone from the store, so the next scan re-decides rather than re-reading it.
        for (const MergeVerdict& v : overrides()) CHECK(v.a != n1 && v.b != n1);
        // A second undo on an already-undone pair is a no-op, not a resurrection.
        clearOverrideKeys(n1, n2);
        CHECK(effectiveItemId(gogRaw) != effectiveItemId(gotyRaw));

        // 2. SPLIT — the self-pair, on the same non-idempotent key. The verdict is written at
        //    normalizeTitle(title); overrideSaysSeparate has to ask about THAT key and not about its
        //    re-normalisation, or the destructive confirm and the success toast both fire and the folder
        //    comes back byte-identical.
        const QString before = effectiveItemId(gotyRaw);
        setOverride(gotyRaw, gotyRaw, false);
        CHECK(overrideSaysSeparate(gotyRaw) == true);
        // THE FOLDER ACTUALLY CHANGED.
        CHECK(effectiveItemId(gotyRaw) != before);
        CHECK(effectiveItemId(gotyRaw) == itemId(gotyRaw) + QStringLiteral("#") + separationTag(gotyRaw));
        // ...and it did not leak onto the key the SECOND normalisation would have landed on — that key
        // belongs to a different game, and splitting one entry must never re-key another.
        CHECK(overrideSaysSeparate(gogRaw) == false);
        CHECK(effectiveItemId(gogRaw) == itemId(gogRaw));

        // The remap follows the split, or the split's new tile holds no records.
        {
            QVector<QPair<QString, QString>> lib;
            lib << qMakePair(QStringLiteral("dl:batman"), gotyRaw)
                << qMakePair(QStringLiteral("gog:1234"), gogRaw);
            const QHash<QString, QString> t = remapTable(lib);
            CHECK(t.value(QStringLiteral("dl:batman")) == effectiveItemId(gotyRaw));
            CHECK(t.value(QStringLiteral("dl:batman")) != t.value(QStringLiteral("gog:1234")));
        }

        // Undo the split too, from the stored keys, and confirm the entry came back.
        for (const MergeVerdict& v : overrides())
            if (v.a == n1 || v.b == n1) clearOverrideKeys(v.a, v.b);
        CHECK(overrideSaysSeparate(gotyRaw) == false);
        CHECK(effectiveItemId(gotyRaw) == before);

        // Leave §7d's state: only (hades, hades ii).
        for (const MergeVerdict& v : overrides())
            CHECK(v.a == QStringLiteral("hades") || v.b == QStringLiteral("hades"));
    }

    // ---- 8. applyRemap: the records follow the id, in every store and every profile -------------
    // Each store below is keyed the way its OWNER keys it (three different hashes), and the ids used
    // are the shapes the launcher scans actually emit.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap.ini"));
        QFile::remove(gRecIni);

        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(QStringLiteral("steam:2074920"), QStringLiteral("Hades II"));
        const QHash<QString, QString> t = remapTable(lib);
        const QString oldId = QStringLiteral("steam:2074920");
        const QString newId = t.value(oldId);
        CHECK(!newId.isEmpty());
        CHECK(newId != oldId);

        const QString mBlob = marksBlob(true, QStringLiteral("finished"), { QStringLiteral("rpg") }, 111);
        const QString sBlob = statsBlob(60, 0, 900, QStringLiteral("Hades II"));
        recSet(QStringLiteral("marks/default/items/") + md5Full(oldId), mBlob);
        recSet(QStringLiteral("marks/p2/items/") + md5Full(oldId), mBlob);   // a SECOND profile
        recSet(QStringLiteral("stats/default/devA/items/") + md5Full(oldId), sBlob);
        const QString pOld = QStringLiteral("playstats/default/devA/") + sha1Full(oldId);
        recSet(pOld + QStringLiteral("/total"), 100);
        recSet(pOld + QStringLiteral("/sessions"), 2);
        recSet(pOld + QStringLiteral("/last"), 500);
        recSet(QStringLiteral("favorites/default/items"), favJson({ qMakePair(oldId, qint64(9)) }));
        recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/pos"), 30.0);
        recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/dur"), 100.0);
        recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/ts"), 7);

        reseatRemapStore();
        applyRemap(t);

        // ItemMarks — moved VERBATIM when the destination is empty. updatedAt is preserved rather than
        // restamped: the multi-device merge orders items on it, so a migration that bumped it would let
        // a stale peer copy lose to a rewrite that added nothing.
        CHECK(!recHas(QStringLiteral("marks/default/items/") + md5Full(oldId)));
        CHECK(recGet(QStringLiteral("marks/default/items/") + md5Full(newId)).toString() == mBlob);
        // ...in EVERY profile, not just the active one. A record belongs to whoever accrued it.
        CHECK(!recHas(QStringLiteral("marks/p2/items/") + md5Full(oldId)));
        CHECK(recGet(QStringLiteral("marks/p2/items/") + md5Full(newId)).toString() == mBlob);

        // ConsumptionStats (hashed like ItemMarks, but device-namespaced).
        CHECK(!recHas(QStringLiteral("stats/default/devA/items/") + md5Full(oldId)));
        CHECK(recObj(QStringLiteral("stats/default/devA/items/") + md5Full(newId))
                  .value(QStringLiteral("mediaSeconds")).toDouble() == 60.0);

        // PlayStats — SHA-1, not MD5. Getting this wrong finds no record and migrates nothing, silently.
        const QString pNew = QStringLiteral("playstats/default/devA/") + sha1Full(newId);
        CHECK(!recHas(pOld + QStringLiteral("/total")));
        CHECK(recGet(pNew + QStringLiteral("/total")).toLongLong() == 100);
        CHECK(recGet(pNew + QStringLiteral("/sessions")).toLongLong() == 2);
        CHECK(recGet(pNew + QStringLiteral("/last")).toLongLong() == 500);

        // FavoritesStore — the one store that is NOT hashed; the id sits in the list verbatim.
        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            CHECK(fav.size() == 1);
            CHECK(favIndexOf(fav, newId) >= 0);
            CHECK(favIndexOf(fav, oldId) < 0);
        }

        // resume — MD5 truncated to ten characters, a third distinct shape, and NOT per-profile.
        CHECK(!recHas(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/pos")));
        CHECK(recGet(QStringLiteral("resume/") + md5Short(newId) + QStringLiteral("/pos")).toDouble() == 30.0);
        CHECK(recGet(QStringLiteral("resume/") + md5Short(newId) + QStringLiteral("/dur")).toDouble() == 100.0);
    }

    // ---- 9. a COLLISION must MERGE, never overwrite ---------------------------------------------
    // Two launcher entries collapsing into one game means two RECORDS collapsing into one. Whichever
    // is processed second must not land on top of the first: that silently deletes real play time, a
    // star, or a completion mark, and the user has no way to notice until it is long gone. The table
    // is a QHash, so which of the two is processed first is not even deterministic — every rule below
    // is therefore order-independent by construction, and these checks pin that.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-collide.ini"));
        QFile::remove(gRecIni);

        const QString steamId = QStringLiteral("steam:1145360");
        const QString gogId   = QStringLiteral("gog:1207658930");
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(steamId, QStringLiteral("Hades"))
            << qMakePair(gogId,   QStringLiteral("Hades"));
        const QHash<QString, QString> t = remapTable(lib);
        const QString newId = t.value(steamId);
        CHECK(newId == t.value(gogId));       // the premise: both really do collide
        CHECK(!newId.isEmpty());

        // marks: the Steam copy carries the completion + a tag; the GOG copy carries the HIDE + another
        // tag and is the newer write. Neither contribution may be lost.
        recSet(QStringLiteral("marks/default/items/") + md5Full(steamId),
               marksBlob(false, QStringLiteral("finished"), { QStringLiteral("roguelike") }, 100));
        recSet(QStringLiteral("marks/default/items/") + md5Full(gogId),
               marksBlob(true, QStringLiteral("none"), { QStringLiteral("indie") }, 200));
        // stats: two real watch/play accumulations.
        recSet(QStringLiteral("stats/default/devA/items/") + md5Full(steamId),
               statsBlob(100, 0, 10, QStringLiteral("Hades")));
        recSet(QStringLiteral("stats/default/devA/items/") + md5Full(gogId),
               statsBlob(50, 0, 20, QStringLiteral("Hades (GOG)")));
        // playstats: 10 minutes on Steam, 20 on GOG. The merged game has 30.
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(steamId) + QStringLiteral("/total"), 600);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(steamId) + QStringLiteral("/sessions"), 3);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(steamId) + QStringLiteral("/last"), 10);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(gogId) + QStringLiteral("/total"), 1200);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(gogId) + QStringLiteral("/sessions"), 2);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(gogId) + QStringLiteral("/last"), 50);
        // favorites: BOTH copies starred. One star out, and it keeps the newer star date.
        recSet(QStringLiteral("favorites/default/items"),
               favJson({ qMakePair(steamId, qint64(5)), qMakePair(gogId, qint64(9)) }));
        // resume: the GOG copy is the newer position.
        recSet(QStringLiteral("resume/") + md5Short(steamId) + QStringLiteral("/pos"), 10.0);
        recSet(QStringLiteral("resume/") + md5Short(steamId) + QStringLiteral("/ts"), 100);
        recSet(QStringLiteral("resume/") + md5Short(gogId) + QStringLiteral("/pos"), 80.0);
        recSet(QStringLiteral("resume/") + md5Short(gogId) + QStringLiteral("/ts"), 200);

        reseatRemapStore();
        applyRemap(t);

        // marks: hidden ORs, the real completion survives "none", the tag lists UNION, updatedAt is the
        // newer of the two.
        {
            const QJsonObject m = recObj(QStringLiteral("marks/default/items/") + md5Full(newId));
            CHECK(m.value(QStringLiteral("hidden")).toBool() == true);
            CHECK(m.value(QStringLiteral("completion")).toString() == QStringLiteral("finished"));
            QStringList tags;
            for (const QJsonValue& v : m.value(QStringLiteral("tags")).toArray()) tags << v.toString();
            CHECK(tags.contains(QStringLiteral("roguelike")));
            CHECK(tags.contains(QStringLiteral("indie")));
            CHECK(qint64(m.value(QStringLiteral("updatedAt")).toDouble()) == 200);
            CHECK(!recHas(QStringLiteral("marks/default/items/") + md5Full(steamId)));
            CHECK(!recHas(QStringLiteral("marks/default/items/") + md5Full(gogId)));
        }

        // stats: seconds SUM (the category rollup is the sum of the items, so nothing else keeps it
        // coherent), lastActivity is the max, and the newer side owns the display title.
        {
            const QJsonObject e = recObj(QStringLiteral("stats/default/devA/items/") + md5Full(newId));
            CHECK(qint64(e.value(QStringLiteral("mediaSeconds")).toDouble()) == 150);
            CHECK(qint64(e.value(QStringLiteral("lastActivity")).toDouble()) == 20);
            CHECK(e.value(QStringLiteral("title")).toString() == QStringLiteral("Hades (GOG)"));
        }

        // playstats: total and sessions SUM, last is the max. This is the check that a user's 30 minutes
        // does not become 20.
        {
            const QString p = QStringLiteral("playstats/default/devA/") + sha1Full(newId);
            CHECK(recGet(p + QStringLiteral("/total")).toLongLong() == 1800);
            CHECK(recGet(p + QStringLiteral("/sessions")).toLongLong() == 5);
            CHECK(recGet(p + QStringLiteral("/last")).toLongLong() == 50);
            CHECK(!recHas(QStringLiteral("playstats/default/devA/") + sha1Full(steamId) + QStringLiteral("/total")));
            CHECK(!recHas(QStringLiteral("playstats/default/devA/") + sha1Full(gogId) + QStringLiteral("/total")));
        }

        // favorites: two entries collapse to ONE (a duplicated star is a duplicated tile), carrying the
        // newer star date.
        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            CHECK(fav.size() == 1);
            const int at = favIndexOf(fav, newId);
            CHECK(at >= 0);
            if (at >= 0) CHECK(qint64(fav.at(at).toObject().value(QStringLiteral("ts")).toDouble()) == 9);
        }

        // resume: newest-wins (a position is a single point, so there is nothing to add up), and both
        // old records are gone.
        CHECK(recGet(QStringLiteral("resume/") + md5Short(newId) + QStringLiteral("/pos")).toDouble() == 80.0);
        CHECK(!recHas(QStringLiteral("resume/") + md5Short(steamId) + QStringLiteral("/pos")));
        CHECK(!recHas(QStringLiteral("resume/") + md5Short(gogId) + QStringLiteral("/pos")));

        // ---- 10. IDEMPOTENT: a second pass changes nothing -------------------------------------
        // The remap runs on EVERY library refresh (records live under a one-way hash, so a game that is
        // not installed right now cannot be found — a one-shot pass would strand its records forever).
        // That makes "twice == once" a hard requirement, and the accumulators are where it would break:
        // a second pass that re-summed would double the user's play time on every single refresh.
        reseatRemapStore();
        applyRemap(t);
        applyRemap(t);
        {
            const QString p = QStringLiteral("playstats/default/devA/") + sha1Full(newId);
            CHECK(recGet(p + QStringLiteral("/total")).toLongLong() == 1800);
            CHECK(recGet(p + QStringLiteral("/sessions")).toLongLong() == 5);
            const QJsonObject e = recObj(QStringLiteral("stats/default/devA/items/") + md5Full(newId));
            CHECK(qint64(e.value(QStringLiteral("mediaSeconds")).toDouble()) == 150);
            const QJsonObject m = recObj(QStringLiteral("marks/default/items/") + md5Full(newId));
            CHECK(qint64(m.value(QStringLiteral("updatedAt")).toDouble()) == 200);
            CHECK(recArr(QStringLiteral("favorites/default/items")).size() == 1);
            CHECK(recGet(QStringLiteral("resume/") + md5Short(newId) + QStringLiteral("/pos")).toDouble() == 80.0);
        }

        // ...and the table's own output is a fixed point: re-deriving the table from the MERGED ids and
        // applying it again is a no-op, which is what the every-refresh call site actually does.
        {
            QVector<QPair<QString, QString>> merged;
            merged << qMakePair(newId, QStringLiteral("Hades"));
            const QHash<QString, QString> t3 = remapTable(merged);
            CHECK(t3.value(newId) == newId);
            reseatRemapStore();
            applyRemap(t3);
            const QString p = QStringLiteral("playstats/default/devA/") + sha1Full(newId);
            CHECK(recGet(p + QStringLiteral("/total")).toLongLong() == 1800);
            CHECK(recObj(QStringLiteral("marks/default/items/") + md5Full(newId))
                      .value(QStringLiteral("hidden")).toBool() == true);
        }
    }

    // ---- 11. applyRemap never writes an empty key, and never drops what it cannot map ------------
    // remapTable cannot emit an empty destination, but applyRemap is a public entry point taking a
    // caller-built hash. One empty value would hash "" and fuse every affected record onto a single
    // bogus key — the record is destroyed, and the old one is gone too. The pair must be IGNORED.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-empty.ini"));
        QFile::remove(gRecIni);

        const QString keep = QStringLiteral("steam:2074920");
        const QString blob = marksBlob(false, QStringLiteral("inProgress"), {}, 55);
        recSet(QStringLiteral("marks/default/items/") + md5Full(keep), blob);
        recSet(QStringLiteral("playstats/default/devA/") + sha1Full(keep) + QStringLiteral("/total"), 42);

        QHash<QString, QString> bad;
        bad.insert(keep, QString());                                   // no destination
        bad.insert(QString(), QStringLiteral("pcgame:hades"));         // no source
        reseatRemapStore();
        applyRemap(bad);

        // Untouched, and nothing landed under the hash of an empty id.
        CHECK(recGet(QStringLiteral("marks/default/items/") + md5Full(keep)).toString() == blob);
        CHECK(recGet(QStringLiteral("playstats/default/devA/") + sha1Full(keep) + QStringLiteral("/total"))
                  .toLongLong() == 42);
        CHECK(!recHas(QStringLiteral("marks/default/items/") + md5Full(QString())));
        CHECK(!recHas(QStringLiteral("playstats/default/devA/") + sha1Full(QString()) + QStringLiteral("/total")));

        // A self-map is a no-op, not a self-destructive move-then-delete.
        QHash<QString, QString> self;
        self.insert(keep, keep);
        reseatRemapStore();
        applyRemap(self);
        CHECK(recGet(QStringLiteral("marks/default/items/") + md5Full(keep)).toString() == blob);
    }

    // ---- 12. a PARTIALLY WRITTEN playstats record must not be re-summed on the retry ---------------
    // The one place idempotence could break, and only on the path nobody exercises. The playstats move
    // writes THREE keys from a SUM, and they cannot fail together: `total` (= a+b) lands, `sessions`
    // hits a full or read-only ini, and the pass gives up with the source still in place. Rule 2 holds —
    // nothing is lost — but the destination now holds a+b while the source still holds b, so a retry
    // that re-summed would write a+2b, and again on every refresh after that. The user's play time
    // inflates, silently, forever.
    //
    // The state below IS that state, built by hand: it is exactly what the ini contains after the
    // marker was written and `total` committed and `sessions` did not. Simulating it needs no seam and
    // no fault injection — the record store is a plain ini and a half-written record is a describable
    // arrangement of keys, so the probe writes the arrangement directly rather than trying to make a
    // real write fail.
    //
    // The marker key is rebuilt here from its documented composition rather than by calling into
    // PcGameRemap, for the same reason the three record hashes are: a probe that asked the production
    // code where its journal lives could not notice the journal moving.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-partial.ini"));
        QFile::remove(gRecIni);

        const QString oldId = QStringLiteral("steam:1145360");
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(oldId, QStringLiteral("Hades"));
        const QHash<QString, QString> t = remapTable(lib);
        const QString newId = t.value(oldId);
        CHECK(newId == itemId(QStringLiteral("Hades")));

        const QString cont = QStringLiteral("playstats/default/devA");
        const QString sp   = cont + QLatin1Char('/') + sha1Full(oldId);
        const QString dp   = cont + QLatin1Char('/') + sha1Full(newId);
        // PcGameRemap.cpp journalKey(): "pcgameremap/pending/" + md5(container|srcSha1|dstSha1).
        const QString jk = QStringLiteral("pcgameremap/pending/")
                         + md5Full(cont + QLatin1Char('|') + sha1Full(oldId) + QLatin1Char('|')
                                 + sha1Full(newId));

        // The source, still intact (the failed pass never removed it).
        recSet(sp + QStringLiteral("/total"), 600);
        recSet(sp + QStringLiteral("/sessions"), 3);
        recSet(sp + QStringLiteral("/last"), 10);
        // The destination the failed pass was merging INTO had 1200/2/50, so the sums are 1800/5/50...
        // ...and `total` is the one leaf that made it to disk before the write error.
        recSet(dp + QStringLiteral("/total"), 1800);
        recSet(dp + QStringLiteral("/sessions"), 2);
        recSet(dp + QStringLiteral("/last"), 50);
        // The marker the pass wrote BEFORE touching any of that, carrying the absolute values.
        recSet(jk, QStringLiteral("{\"total\":1800,\"sessions\":5,\"last\":50}"));

        reseatRemapStore();
        applyRemap(t);

        // THE CHECK: 1800, not 2400. A re-sum would read the already-committed total and add the source
        // to it a second time; committing the marker's absolute values cannot.
        CHECK(recGet(dp + QStringLiteral("/total")).toLongLong() == 1800);
        // ...and the leaf that never landed is finished off, so the record is whole again.
        CHECK(recGet(dp + QStringLiteral("/sessions")).toLongLong() == 5);
        CHECK(recGet(dp + QStringLiteral("/last")).toLongLong() == 50);
        // The source is only now removed, and the marker with it — a completed move leaves no journal.
        CHECK(!recHas(sp + QStringLiteral("/total")));
        CHECK(!recHas(jk));

        // And the pass after THAT is an ordinary no-op: with the source gone there is nothing to add,
        // which is the property the every-refresh call site depends on.
        reseatRemapStore();
        applyRemap(t);
        CHECK(recGet(dp + QStringLiteral("/total")).toLongLong() == 1800);
        CHECK(recGet(dp + QStringLiteral("/sessions")).toLongLong() == 5);
    }

    // ---- 13. a resume record moves WHOLE, never leaf-by-leaf onto the loser's leftovers -------------
    // The winner of the newest-wins comparison replaces the destination outright. Copying only the
    // leaves the winner HAS leaves the loser's stale ones standing beside them, and the resume bar is
    // computed from pos/dur TOGETHER: a new pos of 30 next to an inherited dur of 100 draws the bar at
    // 30% of a duration that position was never 30% of. The title goes the same way — the row would name
    // the wrong thing.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-resume.ini"));
        QFile::remove(gRecIni);

        const QString oldId = QStringLiteral("steam:1145360");
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(oldId, QStringLiteral("Hades"));
        const QHash<QString, QString> t = remapTable(lib);
        const QString newId = t.value(oldId);
        const QString sr = QStringLiteral("resume/") + md5Short(oldId);
        const QString dr = QStringLiteral("resume/") + md5Short(newId);

        // The destination holds a COMPLETE older record; the source is newer but carries pos+ts only.
        recSet(dr + QStringLiteral("/pos"), 10.0);
        recSet(dr + QStringLiteral("/dur"), 100.0);
        recSet(dr + QStringLiteral("/ts"), 100);
        recSet(dr + QStringLiteral("/title"), QStringLiteral("stale"));
        recSet(sr + QStringLiteral("/pos"), 30.0);
        recSet(sr + QStringLiteral("/ts"), 200);

        reseatRemapStore();
        applyRemap(t);

        CHECK(recGet(dr + QStringLiteral("/pos")).toDouble() == 30.0);
        CHECK(recGet(dr + QStringLiteral("/ts")).toLongLong() == 200);
        // The two leaves the winner does not carry are GONE, not inherited from the loser.
        CHECK(!recHas(dr + QStringLiteral("/dur")));
        CHECK(!recHas(dr + QStringLiteral("/title")));
        CHECK(!recHas(sr + QStringLiteral("/pos")));

        // The mirror: when the winner DOES carry every leaf, they all arrive. (Without this, "clear the
        // destination" and "clear the destination and copy nothing" look the same.)
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-resume2.ini"));
        QFile::remove(gRecIni);
        recSet(dr + QStringLiteral("/pos"), 10.0);
        recSet(dr + QStringLiteral("/dur"), 100.0);
        recSet(dr + QStringLiteral("/ts"), 100);
        recSet(sr + QStringLiteral("/pos"), 30.0);
        recSet(sr + QStringLiteral("/dur"), 60.0);
        recSet(sr + QStringLiteral("/ts"), 200);
        recSet(sr + QStringLiteral("/title"), QStringLiteral("fresh"));
        reseatRemapStore();
        applyRemap(t);
        CHECK(recGet(dr + QStringLiteral("/pos")).toDouble() == 30.0);
        CHECK(recGet(dr + QStringLiteral("/dur")).toDouble() == 60.0);
        CHECK(recGet(dr + QStringLiteral("/title")).toString() == QStringLiteral("fresh"));
        CHECK(recGet(dr + QStringLiteral("/ts")).toLongLong() == 200);
    }

    // ---- 14. a MIGRATED favourite is STAMPED, not merely re-keyed ---------------------------------
    // A favourite is the only migrated record with a second key beside its id: `system`, which is what
    // decides whether the PC console's ★ Favorites folder lists it (browse::favoritesCatalog skips
    // f.system != "pc"; HomeView's hasFav gate, which decides whether that folder is even offered, tests
    // the same field). The legacy per-launcher star was written by the generic addon-favourite path, which
    // stamped no `system`, no `kind` and no `path` — there was no file to point at. Rewriting ONLY its id
    // therefore migrates a record that Home and Plays show correctly and that folder silently omits: from
    // where the user looks for their favourite, the star vanished. Losing a star is losing user data even
    // though every byte is technically still on disk.
    {
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp.ini"));
        QFile::remove(gRecIni);

        const QString oldId = QStringLiteral("steam:1145360");
        QVector<QPair<QString, QString>> lib;
        lib << qMakePair(oldId, QStringLiteral("Hades"));
        const QHash<QString, QString> t = remapTable(lib);
        const QString newId = t.value(oldId);
        CHECK(newId == itemId(QStringLiteral("Hades")));

        // favJson writes EXACTLY the legacy shape — itemId, title, ts and nothing else.
        recSet(QStringLiteral("favorites/default/items"), favJson({ qMakePair(oldId, qint64(9)) }));
        reseatRemapStore();
        applyRemap(t);

        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            const int at = favIndexOf(fav, newId);
            CHECK(at >= 0);
            if (at >= 0)
            {
                const QJsonObject o = fav.at(at).toObject();
                CHECK(o.value(QStringLiteral("system")).toString() == QStringLiteral("pc"));
                // …and the routing kind the WRITE side (browse::localGameFavorite) stamps on the same
                // record, so a migrated star and a freshly pressed one are one record, not two shapes.
                CHECK(o.value(QStringLiteral("kind")).toString() == QStringLiteral("pcgame"));
                // THE GATE ITSELF, spelled out as favoritesCatalog applies it (probe_browse pins that this
                // is the rule; this probe cannot link the builder — it is QtCore-only by charter). A record
                // failing this is absent from the folder no matter how correct its id is.
                const bool mergedPc = o.value(QStringLiteral("itemId")).toString()
                                          .startsWith(QStringLiteral("pcgame:"));
                const bool hasPath  = !o.value(QStringLiteral("path")).toString().isEmpty();
                CHECK((hasPath || mergedPc)
                      && o.value(QStringLiteral("system")).toString() == QStringLiteral("pc"));
                // The star's own data is untouched: the stamp adds fields, it does not re-date the record
                // (ts orders the multi-device merge — bumping it would let this rewrite beat a real peer).
                CHECK(qint64(o.value(QStringLiteral("ts")).toDouble()) == 9);
                CHECK(o.value(QStringLiteral("title")).toString() == oldId);  // favJson's title
            }
        }

        // Idempotent, like every other store: a second pass adds no entry and changes no field.
        reseatRemapStore();
        applyRemap(t);
        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            CHECK(fav.size() == 1);
            const int at = favIndexOf(fav, newId);
            CHECK(at >= 0);
            if (at >= 0)
                CHECK(fav.at(at).toObject().value(QStringLiteral("system")).toString()
                      == QStringLiteral("pc"));
        }

        // ...AND the record an EARLIER BUILD already migrated: its id is a pcgame: id, so it is in NO
        // table (every table key is a per-launcher id) and a stamp conditioned on "did this pass rewrite
        // it" would never reach it — stranding precisely the users who ran the build with the bug.
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp2.ini"));
        QFile::remove(gRecIni);
        recSet(QStringLiteral("favorites/default/items"), favJson({ qMakePair(newId, qint64(9)) }));
        reseatRemapStore();
        applyRemap(t);
        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            const int at = favIndexOf(fav, newId);
            CHECK(at >= 0);
            if (at >= 0)
            {
                CHECK(fav.at(at).toObject().value(QStringLiteral("system")).toString()
                      == QStringLiteral("pc"));
                CHECK(fav.at(at).toObject().value(QStringLiteral("kind")).toString()
                      == QStringLiteral("pcgame"));
            }
        }

        // The mirror: a NON-PC favourite is left exactly as it was. The stamp keys on the merged id, so a
        // streamed/addon favourite riding in the same list must not acquire a console it does not belong
        // to — that would file a movie under the PC games console.
        gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp3.ini"));
        QFile::remove(gRecIni);
        recSet(QStringLiteral("favorites/default/items"),
               favJson({ qMakePair(QStringLiteral("tt0111161"), qint64(4)) }));
        reseatRemapStore();
        applyRemap(t);
        {
            const QJsonArray fav = recArr(QStringLiteral("favorites/default/items"));
            const int at = favIndexOf(fav, QStringLiteral("tt0111161"));
            CHECK(at >= 0);
            if (at >= 0)
            {
                CHECK(fav.at(at).toObject().value(QStringLiteral("system")).toString().isEmpty());
                CHECK(fav.at(at).toObject().value(QStringLiteral("kind")).toString().isEmpty());
            }
        }
    }

    // ---- 15. the first-entry migration flushes PER PASS, not per record ----------------------------
    // The remap runs on the GUI thread the first time the PC Games folder is opened, and a
    // QSettings::sync() is a whole-file rewrite. One sync per moved record therefore means one disk
    // write per record, and a library with hundreds of migrating records stalls visibly — once, on
    // first entry, which is precisely the moment the user is forming an opinion of the folder.
    //
    // The property pinned is not "fast" (unmeasurable repeatably on CI) but CONSTANT: the same fixture
    // at two sizes must flush the same number of times. A bound alone would pass a version that halved
    // the per-record count; only the two sizes agreeing says the flushes stopped scaling at all.
    {
        auto runBatchFixture = [](int n) {
            gRecIni = QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-batch%1.ini").arg(n));
            QFile::remove(gRecIni);

            QVector<QPair<QString, QString>> lib;
            QVector<QPair<QString, qint64>>  favs;
            for (int i = 0; i < n; ++i)
            {
                const QString oldId = QStringLiteral("steam:%1").arg(900000 + i);
                lib  << qMakePair(oldId, QStringLiteral("Batch Game %1").arg(i));
                favs << qMakePair(oldId, qint64(i));
                // A record in EVERY store, so every pass has work to do at both sizes.
                recSet(QStringLiteral("marks/default/items/") + md5Full(oldId),
                       marksBlob(true, QStringLiteral("finished"), { QStringLiteral("rpg") }, 111));
                recSet(QStringLiteral("stats/default/devA/items/") + md5Full(oldId),
                       statsBlob(60, 0, 900, QStringLiteral("Batch")));
                const QString p = QStringLiteral("playstats/default/devA/") + sha1Full(oldId);
                recSet(p + QStringLiteral("/total"), 100);
                recSet(p + QStringLiteral("/sessions"), 2);
                recSet(p + QStringLiteral("/last"), 500);
                recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/pos"), 30.0);
                recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/dur"), 100.0);
                recSet(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/ts"), 7);
            }
            recSet(QStringLiteral("favorites/default/items"), favJson(favs));

            const QHash<QString, QString> t = remapTable(lib);
            reseatRemapStore();
            resetRemapSyncCount();
            applyRemap(t);
            return qMakePair(remapSyncCount(), t);
        };

        const int small = 8, large = 64;
        const auto a = runBatchFixture(small);
        const auto b = runBatchFixture(large);
        std::fprintf(stderr, "PCGAMES-INFO: %d records -> %d ini sync(s); %d records -> %d ini sync(s)\n",
                     small, a.first, large, b.first);

        CHECK(a.first == b.first);          // constant in the size of the library — THE property
        CHECK(a.first <= 16);               // 5 passes x (write-flush + remove-flush) + applyRemap's own

        // ...and the batching did not cost correctness: the LARGE run's records really did move.
        {
            const QString oldId = QStringLiteral("steam:%1").arg(900000 + large - 1);
            const QString newId = b.second.value(oldId);
            CHECK(!newId.isEmpty());
            CHECK(!recHas(QStringLiteral("marks/default/items/") + md5Full(oldId)));
            CHECK(recObj(QStringLiteral("marks/default/items/") + md5Full(newId))
                      .value(QStringLiteral("hidden")).toBool());
            CHECK(recObj(QStringLiteral("stats/default/devA/items/") + md5Full(newId))
                      .value(QStringLiteral("mediaSeconds")).toDouble() == 60.0);
            const QString pNew = QStringLiteral("playstats/default/devA/") + sha1Full(newId);
            CHECK(!recHas(QStringLiteral("playstats/default/devA/") + sha1Full(oldId)
                          + QStringLiteral("/total")));
            CHECK(recGet(pNew + QStringLiteral("/total")).toLongLong() == 100);
            CHECK(favIndexOf(recArr(QStringLiteral("favorites/default/items")), newId) >= 0);
            CHECK(favIndexOf(recArr(QStringLiteral("favorites/default/items")), oldId) < 0);
            CHECK(!recHas(QStringLiteral("resume/") + md5Short(oldId) + QStringLiteral("/pos")));
            CHECK(recGet(QStringLiteral("resume/") + md5Short(newId) + QStringLiteral("/pos")).toDouble()
                  == 30.0);
            // No journal marker survives a completed pass, at any batch size.
            QSettings chk(gRecIni, QSettings::IniFormat);
            chk.beginGroup(QStringLiteral("pcgameremap/pending"));
            CHECK(chk.childKeys().isEmpty());
            chk.endGroup();
        }
    }

    // Leave nothing behind (issue #42): every scratch ini this run created goes, so the next run — and
    // any other probe sharing build/Release — starts from a clean file.
    QFile::remove(ini);
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-collide.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-empty.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-partial.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-resume.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-resume2.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp2.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-favstamp3.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-batch8.ini")));
    QFile::remove(QDir::temp().filePath(QStringLiteral("eb-probe-pcremap-batch64.ini")));

    if (failures == 0) { std::puts("PCGAMES-OK"); return 0; }
    std::fprintf(stderr, "PCGAMES: %d check(s) failed\n", failures);
    return 1;
}
