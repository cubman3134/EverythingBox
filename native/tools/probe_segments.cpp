// Headless coverage for the intro/credits segment core: the three providers, the per-type precedence rule,
// series-key derivation, and the tracker's enter/consume/re-arm behaviour. Pure — no player, no video file.
// Prints SEGMENTS-OK on success; any failure prints SEGMENTS-FAIL <what> and exits non-zero.
#include "MediaSegments.h"
#include "SegmentStore.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "SEGMENTS-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace MediaSegments;

static int countOf(const QVector<Segment>& v, SegmentType t)
{
    int n = 0;
    for (const Segment& s : v) if (s.type == t) ++n;
    return n;
}

static bool has(const QVector<Segment>& v, SegmentType t, double start, double end)
{
    for (const Segment& s : v)
        if (s.type == t && qAbs(s.start - start) < 0.01 && qAbs(s.end - end) < 0.01) return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---------------------------------------------------------------- 1. parseEdl: the three time forms
    {
        // seconds, HH:MM:SS.sss, and #frames at 25fps (250/25 = 10s .. 750/25 = 30s).
        const QString edl = QStringLiteral(
            "10.0\t40.0\t3\n"
            "00:01:00.000\t00:01:30.000\t3\n"
            "#250\t#750\t3\n");
        const QVector<Segment> v = parseEdl(edl, 3600.0, 25.0);
        CHECK(v.size() == 3, "all three time forms parse");
        CHECK(has(v, SegmentType::Intro, 10.0, 40.0), "plain seconds");
        CHECK(has(v, SegmentType::Commercial, 60.0, 90.0), "HH:MM:SS");
        CHECK(has(v, SegmentType::Commercial, 10.0, 30.0), "frames at 25fps");

        // Without an fps the frame line is unusable — but it must not poison the rest of the file.
        const QVector<Segment> nofps = parseEdl(edl, 3600.0, 0.0);
        CHECK(nofps.size() == 2, "frame lines drop when fps is unknown, others survive");
    }

    // ---------------------------------------------------------------- 2. parseEdl: actions and junk
    {
        const QString edl = QStringLiteral(
            "10 40 0\n"      // cut -> a skip
            "50 80 1\n"      // mute -> not a skip
            "90 120 2\n"     // scene marker -> not a skip
            "130 160 3\n"    // commercial -> a skip
            "\n"
            "garbage\n"
            "200 abc 3\n"    // unparseable end time
            "300 250 3\n"    // inverted — rejected by the minimum-length check, which subsumes end <= start
            "400 402 3\n");  // shorter than kMinSegmentS
        const QVector<Segment> v = parseEdl(edl, 3600.0, 25.0);
        // NAME the survivors, not just how many: a count alone survives swapping which actions are skips.
        CHECK(has(v, SegmentType::Intro, 10.0, 40.0), "action 0 (cut) is a skip, and is the first Intro");
        CHECK(has(v, SegmentType::Commercial, 130.0, 160.0), "action 3 (commercial break) is a skip");
        CHECK(v.size() == 2, "and nothing else: junk, mute, marker, inverted and tiny all drop");
    }

    // ---------------------------------------------------------------- 3. parseEdl: position typing
    {
        // Credits: ends within kCreditsTailS of duration. Every NEGATIVE below also names the type the range
        // DID receive — "not Intro" alone is satisfied by the range being dropped entirely.
        CHECK(countOf(parseEdl(QStringLiteral("3500 3560 3\n"), 3600.0, 0.0), SegmentType::Credits) == 1,
              "a range ending 40s before the end is Credits");
        // ON the boundary, so the comparison operator itself is pinned (>=, not >).
        CHECK(countOf(parseEdl(QStringLiteral("3480 3540 3\n"), 3600.0, 0.0), SegmentType::Credits) == 1,
              "a range ending exactly kCreditsTailS before the end is still Credits");
        const QVector<Segment> nearTail = parseEdl(QStringLiteral("3400 3530 3\n"), 3600.0, 0.0);
        CHECK(countOf(nearTail, SegmentType::Credits) == 0 && has(nearTail, SegmentType::Commercial, 3400.0, 3530.0),
              "a range ending 70s before the end is Commercial, not Credits");

        // Intro window boundary: starts before kIntroWindowS.
        CHECK(countOf(parseEdl(QStringLiteral("899 950 3\n"), 3600.0, 0.0), SegmentType::Intro) == 1,
              "starting at 899s is inside the intro window");
        const QVector<Segment> atWindow = parseEdl(QStringLiteral("900 950 3\n"), 3600.0, 0.0);
        CHECK(countOf(atWindow, SegmentType::Intro) == 0 && has(atWindow, SegmentType::Commercial, 900.0, 950.0),
              "starting at exactly kIntroWindowS is OUTSIDE the window (start < window, not <=)");
        const QVector<Segment> pastWindow = parseEdl(QStringLiteral("901 950 3\n"), 3600.0, 0.0);
        CHECK(countOf(pastWindow, SegmentType::Intro) == 0 && has(pastWindow, SegmentType::Commercial, 901.0, 950.0),
              "starting at 901s is outside the intro window, and stays Commercial");

        // Intro length boundary: <= kIntroMaxLenS.
        CHECK(countOf(parseEdl(QStringLiteral("10 309 3\n"), 3600.0, 0.0), SegmentType::Intro) == 1,
              "a 299s range is short enough to be an intro");
        CHECK(countOf(parseEdl(QStringLiteral("10 310 3\n"), 3600.0, 0.0), SegmentType::Intro) == 1,
              "a range of exactly kIntroMaxLenS is still an intro (length <=, not <)");
        const QVector<Segment> tooLong = parseEdl(QStringLiteral("10 311 3\n"), 3600.0, 0.0);
        CHECK(countOf(tooLong, SegmentType::Intro) == 0 && has(tooLong, SegmentType::Commercial, 10.0, 311.0),
              "a 301s range is too long to be an intro, and stays Commercial");

        // Only the FIRST qualifying range is the intro.
        const QVector<Segment> two = parseEdl(QStringLiteral("10 40 3\n60 90 3\n"), 3600.0, 0.0);
        CHECK(countOf(two, SegmentType::Intro) == 1 && has(two, SegmentType::Intro, 10.0, 40.0),
              "only the first qualifying range is the Intro");
        CHECK(has(two, SegmentType::Commercial, 60.0, 90.0), "…and the second survives as a Commercial");

        // THE OVERLAP CASE. In a short file one range satisfies both rules. Credits must win: typing it as
        // an Intro would make the chip offer to skip the entire rest of the episode.
        const QVector<Segment> overlap = parseEdl(QStringLiteral("30 100 3\n"), 120.0, 0.0);
        CHECK(has(overlap, SegmentType::Credits, 30.0, 100.0) && countOf(overlap, SegmentType::Intro) == 0,
              "a range satisfying both rules types as Credits, never Intro");
    }

    // ---------------------------------------------------------------- 4. fromChapters
    {
        const QVector<Chapter> ch = {
            { 0.0,   QStringLiteral("Recap") },
            { 45.0,  QStringLiteral("Opening Credits") },
            { 135.0, QStringLiteral("Part One") },
            { 900.0, QStringLiteral("End Credits") },
        };
        const QVector<Segment> v = fromChapters(ch, 1000.0);
        CHECK(has(v, SegmentType::Recap, 0.0, 45.0), "a Recap chapter runs to the next chapter");
        // "Opening Credits" CONTAINS "credits". Intro phrases are tested first for exactly this reason —
        // otherwise every anime and drama opening in the world types as end credits.
        CHECK(has(v, SegmentType::Intro, 45.0, 135.0), "\"Opening Credits\" is an Intro, not Credits");
        CHECK(has(v, SegmentType::Credits, 900.0, 1000.0), "the last chapter runs to duration");
        CHECK(countOf(v, SegmentType::Intro) == 1 && v.size() == 3, "\"Part One\" matches nothing");

        // THE OTHER ORDERING TRAP, and the whole title-typing contract in one table.
        //
        // "End Titles" and "Closing Titles" — the conventional BBC/ITV chapter names — contain the bare
        // intro noun "titles", so end cues must be tested BEFORE the intro stage: typing one as an Intro
        // hands the caller a skip covering the rest of the episode.
        //
        // The end stage is COMPOSITIONAL — any qualifier of {end, ending, closing, final} beside any noun of
        // {credits, credit, titles, title, theme} — and this table is why. It used to be a flat list of
        // spelled-out phrases ("end credits", "end titles", "closing credits", "closing titles"), which is
        // unfixable by construction: the list enumerates a cross product, so every pair nobody wrote down is
        // a live bug. "Closing Theme" was exactly that — it matched no end phrase, fell through to the intro
        // stage, matched the bare "theme", and came back an INTRO; in a 2760s episode with that chapter at
        // 2700s the feature then offered "Skip Intro" 60 seconds from the end. "End Theme", "Final Titles"
        // and the singular "End Title" all failed the same way. Every row below must hold, and the rows are
        // the product, not a list of remembered special cases.
        struct TitleCase { const char* title; int type; };   // -1 = matches nothing
        static const TitleCase titleCases[] = {
            { "Opening Credits", int(SegmentType::Intro)   },  // "opening" is NOT an end qualifier…
            { "Opening Titles",  int(SegmentType::Intro)   },  // …so these must miss the end stage entirely
            { "Main Theme",      int(SegmentType::Intro)   },
            { "Intro",           int(SegmentType::Intro)   },
            { "[OP]",            int(SegmentType::Intro)   },  // punctuation and case normalize away
            { "End Titles",      int(SegmentType::Credits) },
            { "End Title",       int(SegmentType::Credits) },  // singular: the pair list never had it
            { "Closing Titles",  int(SegmentType::Credits) },
            { "Closing Theme",   int(SegmentType::Credits) },  // the regression this rule exists for
            { "End Theme",       int(SegmentType::Credits) },
            { "Final Titles",    int(SegmentType::Credits) },
            { "End Credits",     int(SegmentType::Credits) },
            { "Ending",          int(SegmentType::Credits) },  // a qualifier with no noun: standalone marker
            { "Credits",         int(SegmentType::Credits) },  // generic, no qualifier at all
            { "Recap",           int(SegmentType::Recap)   },
            { "Previously On",   int(SegmentType::Recap)   },
            { "Introduction",    -1 },                         // word-boundary, not substring, vs "intro"
            { "Part One",        -1 },
        };
        for (const TitleCase& tc : titleCases)
        {
            const QVector<Segment> v2 = fromChapters({ { 0.0,  QString::fromLatin1(tc.title) },
                                                       { 90.0, QStringLiteral("Body") } }, 2760.0);
            const QByteArray what = QByteArray("\"") + tc.title + "\" types as expected";
            if (tc.type < 0)
                CHECK(v2.isEmpty(), what.constData());
            else
                CHECK(v2.size() == 1 && has(v2, SegmentType(tc.type), 0.0, 90.0), what.constData());
        }

        // …and the positional consequence, which is what makes a mistyped end cue harmful rather than
        // merely wrong: an end cue 60s from the end must never come back as a skippable Intro.
        for (const char* endCue : { "End Titles", "End Title", "Closing Titles", "Closing Theme",
                                    "End Theme", "Final Titles", "End Credits", "Closing Credits", "Ending" })
        {
            const QVector<Segment> v2 = fromChapters({ { 0.0,    QStringLiteral("Part One") },
                                                       { 2700.0, QString::fromLatin1(endCue) } }, 2760.0);
            CHECK(has(v2, SegmentType::Credits, 2700.0, 2760.0) && countOf(v2, SegmentType::Intro) == 0,
                  (QByteArray("\"") + endCue + "\" is Credits, never an Intro at 2700s").constData());
        }

        // A last chapter cannot be sized without a duration.
        CHECK(fromChapters({ { 0.0, QStringLiteral("Intro") } }, 0.0).isEmpty(),
              "an unknown duration drops the last chapter");
    }

    // ---------------------------------------------------------------- 5. resolve: per-TYPE precedence
    {
        const QVector<Segment> edl      = { { 100, 200, SegmentType::Commercial } };
        const QVector<Segment> chapters = { { 10,  40,  SegmentType::Intro } };
        const QVector<Segment> learned  = { { 15,  45,  SegmentType::Intro },
                                            { 900, 1000, SegmentType::Credits } };
        const QVector<Segment> v = resolve(edl, chapters, learned);
        CHECK(has(v, SegmentType::Intro, 10.0, 40.0), "chapters beat learned for Intro");
        CHECK(countOf(v, SegmentType::Intro) == 1, "the losing tier's Intro is not also included");
        CHECK(has(v, SegmentType::Credits, 900.0, 1000.0), "learned supplies Credits when nothing else does");
        CHECK(has(v, SegmentType::Commercial, 100.0, 200.0), "the .edl Commercial survives");
        // The whole point of per-type precedence:
        CHECK(countOf(resolve(edl, chapters, {}), SegmentType::Intro) == 1,
              "an .edl with ONLY a Commercial does not suppress a chapter Intro");
    }

    // ---------------------------------------------------------------- 6. keyFor
    {
        const Key k = keyFor(QStringLiteral("tt0903747:2:7"), QString());
        CHECK(k.seriesKey == QStringLiteral("tt0903747") && k.season == 2, "the stream id supplies series+season");

        const Key f = keyFor(QString(), QStringLiteral("D:/TV/Breaking Bad/Breaking Bad S03E05.mkv"));
        CHECK(f.seriesKey == QStringLiteral("name:breaking bad") && f.season == 3,
              "a filename with no stream id still yields a series key");

        CHECK(keyFor(QString(), QStringLiteral("D:/Movies/Blade Runner (1982).mkv")).seriesKey.isEmpty(),
              "a movie has no series key");
        CHECK(keyFor(QString(), QString()).seriesKey.isEmpty(), "nothing in, nothing out");

        // SHAPE, not arity. "tmdb:tv:1396" is a real 3-part tile id; keying it would make seriesKey "tmdb"
        // and collapse every TMDB-catalogued show into one learned bucket, offering one show's intro in
        // another. An id that is not the "tt…:S:E" contract must leave the learn tier unavailable.
        const Key tmdb = keyFor(QStringLiteral("tmdb:tv:1396"), QString());
        CHECK(tmdb.seriesKey.isEmpty(), "a 3-part non-IMDB id yields NO series key");
    }

    // ---------------------------------------------------------------- 7. Tracker
    {
        Tracker t;
        t.reset({ { 10.0, 40.0, SegmentType::Intro } });
        CHECK(!t.onPosition(5.0).has_value(), "before the segment: no offer");
        CHECK(t.onPosition(10.0).has_value(), "entering the segment offers it");
        CHECK(!t.onPosition(20.0).has_value(), "still inside: not offered again");
        CHECK(!t.onPosition(50.0).has_value(), "past it: not offered again");
        CHECK(!t.onPosition(45.0).has_value(), "seeking back but still past the start does NOT re-arm");
        CHECK(!t.onPosition(9.0).has_value(), "seeking to before the start re-arms but does not itself offer");
        CHECK(t.onPosition(11.0).has_value(), "…and re-entering offers it again");

        Tracker empty;
        CHECK(empty.empty() && !empty.onPosition(10.0).has_value(), "an empty tracker never offers");
    }

    // ---------------------------------------------------------------- 8. SegmentStore
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir for the store");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("segments.json"));

        SegmentStore st(path);
        st.load();
        CHECK(st.lookup(QStringLiteral("tt1"), 1).isEmpty(), "an empty store has nothing");

        st.put(QStringLiteral("tt1"), 2, Segment{ 10.0, 40.0, SegmentType::Intro });

        // Round-trip through disk: a fresh store on the same file sees it.
        SegmentStore re(path);
        re.load();
        const QVector<Segment> s2 = re.lookup(QStringLiteral("tt1"), 2);
        CHECK(s2.size() == 1 && has(s2, SegmentType::Intro, 10.0, 40.0), "the season mark round-trips");

        // The series-level fallback: season 3 was never marked, so it inherits the most recent mark.
        const QVector<Segment> s3 = re.lookup(QStringLiteral("tt1"), 3);
        CHECK(s3.size() == 1 && has(s3, SegmentType::Intro, 10.0, 40.0), "an unmarked season falls back");

        // A different show gets nothing.
        CHECK(re.lookup(QStringLiteral("tt2"), 2).isEmpty(), "the fallback does not leak across series");

        // Same type overwrites; a different type coexists.
        re.put(QStringLiteral("tt1"), 2, Segment{ 12.0, 42.0, SegmentType::Intro });
        re.put(QStringLiteral("tt1"), 2, Segment{ 900.0, 1000.0, SegmentType::Credits });
        const QVector<Segment> both = re.lookup(QStringLiteral("tt1"), 2);
        CHECK(both.size() == 2, "marking credits does not clobber the learned intro");
        CHECK(has(both, SegmentType::Intro, 12.0, 42.0), "the same type overwrites in place");
        CHECK(has(both, SegmentType::Credits, 900.0, 1000.0), "the new type is added");

        // ---- The fallback is PER TYPE, exactly like resolve(). THE bug this pins: season 1 is inheriting an
        // intro skip that works; the user then marks season 1's credits. Whole-list fallback sees a non-empty
        // season entry, returns the Credits alone, and the intro skip that worked five minutes ago silently
        // stops — put() never clobbered it in storage, lookup() just made it unreachable, which to the user
        // is the same thing. Each type must be answered independently: the season's own row when it has one,
        // the series-level row for every type it is silent about.
        {
            SegmentStore mix(QDir(tmp.path()).filePath(QStringLiteral("mix.json")));
            mix.load();
            mix.put(QStringLiteral("tt7"), 1, Segment{ 10.0, 40.0, SegmentType::Intro });
            mix.put(QStringLiteral("tt7"), 2, Segment{ 12.0, 42.0, SegmentType::Intro });    // bare Intro moves on
            mix.put(QStringLiteral("tt7"), 2, Segment{ 900.0, 1000.0, SegmentType::Credits });
            const QVector<Segment> perType = mix.lookup(QStringLiteral("tt7"), 1);
            CHECK(perType.size() == 2, "a non-empty season entry does not suppress the types it lacks");
            CHECK(has(perType, SegmentType::Intro, 10.0, 40.0), "the season's OWN Intro beats the fallback's");
            CHECK(has(perType, SegmentType::Credits, 900.0, 1000.0), "…and the missing type is still inherited");

            // The reverse direction, which is the failure verbatim: mark season 3's credits, keep the intro.
            mix.put(QStringLiteral("tt7"), 3, Segment{ 800.0, 900.0, SegmentType::Credits });
            const QVector<Segment> s3 = mix.lookup(QStringLiteral("tt7"), 3);
            CHECK(has(s3, SegmentType::Intro, 12.0, 42.0), "marking credits does not retire an INHERITED intro");
            CHECK(has(s3, SegmentType::Credits, 800.0, 900.0), "…while the freshly marked credits win");
        }

        // ---- forget, one season only: the mark is genuinely GONE. put() wrote the bare series key too, so
        // dropping just "tt1|s2" would leave a byte-identical copy that lookup's fallback resurrects — a
        // wrong range the user can never clear, because no public call sequence reaches that copy.
        re.forget(QStringLiteral("tt1"), 2);
        CHECK(re.lookup(QStringLiteral("tt1"), 2).isEmpty(), "forgetting the only marked season leaves NOTHING");
        CHECK(re.lookup(QStringLiteral("tt1"), 9).isEmpty(), "…and there is no series-level ghost to inherit");
        {   // the season key itself is gone from the file, not merely shadowed by an empty lookup
            QFile f(path);
            CHECK(f.open(QIODevice::ReadOnly), "re-read the store file");
            const QByteArray raw = f.readAll();
            CHECK(!raw.contains("tt1|s2"), "the season key is erased on disk, not just emptied in memory");
            CHECK(!raw.contains("900"), "and the forgotten range is nowhere in the file");
        }
        SegmentStore reloaded(path);
        reloaded.load();
        CHECK(reloaded.lookup(QStringLiteral("tt1"), 2).isEmpty(), "the forget survives a reload");

        // ---- forget, two marked seasons: season 1 goes and season 1 then INHERITS season 2's mark. That is
        // the documented fallback doing its job — season 1 is now unmarked — and is distinguishable from the
        // bug above only because the inherited range is season 2's, not the one just deleted.
        const QString twoPath = QDir(tmp.path()).filePath(QStringLiteral("two.json"));
        {
            SegmentStore two(twoPath);
            two.load();
            CHECK(two.put(QStringLiteral("tt3"), 1, Segment{ 10.0, 40.0, SegmentType::Intro }), "put reports success");
            two.put(QStringLiteral("tt3"), 2, Segment{ 20.0, 50.0, SegmentType::Intro });
        }
        SegmentStore two(twoPath);
        two.load();                                        // via disk, so the "src" field is exercised
        CHECK(two.forget(QStringLiteral("tt3"), 1), "forget reports success");
        const QVector<Segment> inherited = two.lookup(QStringLiteral("tt3"), 1);
        CHECK(inherited.size() == 1 && has(inherited, SegmentType::Intro, 20.0, 50.0),
              "a forgotten season inherits the OTHER season's mark");
        CHECK(!has(inherited, SegmentType::Intro, 10.0, 40.0), "…never the range that was just forgotten");
        const QVector<Segment> s2Still = two.lookup(QStringLiteral("tt3"), 2);
        CHECK(s2Still.size() == 1 && has(s2Still, SegmentType::Intro, 20.0, 50.0), "season 2 is untouched");

        // ---- Rows from a file written before provenance existed carry no "src". They load as season 0, so
        // no forget matches them: an unknown origin is never guessed at and deleted.
        const QString oldPath = QDir(tmp.path()).filePath(QStringLiteral("old.json"));
        {
            QFile f(oldPath);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "write the pre-provenance file");
            f.write("{\"tt5\":[{\"s\":10,\"e\":40,\"t\":\"intro\"}],"
                    "\"tt5|s1\":[{\"s\":10,\"e\":40,\"t\":\"intro\"}]}");
        }
        SegmentStore legacy(oldPath);
        legacy.load();
        CHECK(has(legacy.lookup(QStringLiteral("tt5"), 1), SegmentType::Intro, 10.0, 40.0),
              "a file with no \"src\" still loads");
        legacy.forget(QStringLiteral("tt5"), 1);
        CHECK(has(legacy.lookup(QStringLiteral("tt5"), 1), SegmentType::Intro, 10.0, 40.0),
              "an unknown-provenance fallback row survives forget rather than being guessed away");

        // ---- A save that cannot happen is reported, not swallowed: this file is the only copy of ranges the
        // user hand-marked, so a caller must be able to tell the mark did not stick.
        SegmentStore unwritable(QDir(tmp.path()).filePath(QStringLiteral("no/such/dir/segments.json")));
        CHECK(!unwritable.put(QStringLiteral("tt6"), 1, Segment{ 10.0, 40.0, SegmentType::Intro }),
              "put returns false when the file cannot be written");

        // A season of 0 (unknown) keys the bare series entry, not "|s0".
        CHECK(SegmentStore::keyFor(QStringLiteral("tt1"), 0) == QStringLiteral("tt1"),
              "season 0 keys the bare series");
        CHECK(SegmentStore::keyFor(QStringLiteral("tt1"), 2) == QStringLiteral("tt1|s2"), "season key shape");

        // A missing file is a clean empty store, not a crash.
        SegmentStore missing(QDir(tmp.path()).filePath(QStringLiteral("nope.json")));
        missing.load();
        CHECK(missing.lookup(QStringLiteral("tt1"), 1).isEmpty(), "a missing file loads as empty");
    }

    // ------------------------------------------------- 9. the JSON type tokens, in BOTH directions
    // typeToString/typeFromString are exercised nowhere else: section 8's round-trip would still pass if
    // every type collapsed onto one token, so pin the mapping itself rather than trusting it incidentally.
    {
        QSet<QString> tokens;
        for (const SegmentType t : { SegmentType::Intro, SegmentType::Credits,
                                     SegmentType::Recap,  SegmentType::Commercial })
        {
            const QString tok = typeToString(t);
            tokens.insert(tok);
            const std::optional<SegmentType> back = typeFromString(tok);
            const QByteArray what = QByteArray("\"") + tok.toUtf8() + "\" survives to/from";
            CHECK(!tok.isEmpty() && back.has_value() && *back == t, what.constData());
        }
        // DISTINCT tokens: two types sharing one would silently rewrite a learned Credits as an Intro on the
        // next load — the round-trip above cannot see it, a collision only shows up as a lost token.
        CHECK(tokens.size() == 4, "each type has its own token");

        // …and the LITERALS, because every assertion above is expressed through typeToString itself and so
        // pins only the bijection, not the wire format. Renaming a token in typeToString and typeFromString
        // together — precisely what a tidy-up refactor does — keeps all of them green while orphaning every
        // file already on disk: load() skips the now-unknown token and every learned mark vanishes silently,
        // with no error anywhere. MediaSegments.h calls these "Stable tokens"; these four lines are what
        // makes that true.
        CHECK(typeToString(SegmentType::Intro)      == QStringLiteral("intro"),      "the Intro token is \"intro\"");
        CHECK(typeToString(SegmentType::Credits)    == QStringLiteral("credits"),    "the Credits token is \"credits\"");
        CHECK(typeToString(SegmentType::Recap)      == QStringLiteral("recap"),      "the Recap token is \"recap\"");
        CHECK(typeToString(SegmentType::Commercial) == QStringLiteral("commercial"), "the Commercial token is \"commercial\"");

        // Unknown text is an absence, never a default. If it fell back to Intro, a token from a newer build
        // would come back as a skippable opening at whatever timestamp that other segment happened to hold.
        CHECK(!typeFromString(QStringLiteral("sponsor")).has_value(), "an unknown token is nullopt, not Intro");
        CHECK(!typeFromString(QString()).has_value(), "an empty token is nullopt, not Intro");

        // …and end to end: a file written by a hypothetical newer build loads its known rows and DROPS the
        // rest, rather than crashing or admitting a bogus Intro.
        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir for the forward-compat file");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("newer.json"));
        {
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "write the forward-compat file");
            // The token is HARDCODED, not typeToString(Intro): this fixture stands in for a file already on
            // a user's disk, so it must break if the token is renamed — that is the whole point of it.
            f.write("{\"tt9|s1\":["
                    "{\"s\":10,\"e\":40,\"t\":\"intro\"},"
                    "{\"s\":500,\"e\":600,\"t\":\"sponsor\"}]}");
        }
        SegmentStore newer(path);
        newer.load();
        const QVector<Segment> v = newer.lookup(QStringLiteral("tt9"), 1);
        CHECK(v.size() == 1 && has(v, SegmentType::Intro, 10.0, 40.0), "the known row loads");
        CHECK(!has(v, SegmentType::Intro, 500.0, 600.0), "the unknown row is skipped, not read as an Intro");
    }

    if (failures) { std::fprintf(stderr, "SEGMENTS-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SEGMENTS-OK\n");
    return 0;
}
