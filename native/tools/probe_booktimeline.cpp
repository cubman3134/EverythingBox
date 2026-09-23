// Headless probe for THE WHOLE BOOK ON THE TIMELINE (issue #218): the model behind a position bar that
// spans a fifty-seven-part audiobook instead of the forty-five minutes of part one.
//
// WHY THIS PROBE EXISTS. The feature is arithmetic over numbers nobody can see: a total derived from part
// sizes and one measured duration, corrections absorbed as more durations arrive, and an elapsed reading
// that has to be monotone across a boundary it can only be observed crossing fourteen hours into a book.
// Every one of those is a pure function of core/BookTimeline.h, so all of it can be pinned here — with no
// window, no mpv, no network and no fifteen-hour wait — and mutation-tested.
//
// THE THREE PROPERTIES THAT MATTER, and each is a defect if it goes:
//
//   1. THE TOTAL DOES NOT JITTER. A number that visibly changes at each of fifty-six boundaries is worse
//      than one that is stable and slightly wrong; the model holds it by taking each measurement's error
//      out of the parts not yet heard.
//   2. THE ELAPSED READING NEVER GOES BACKWARDS. The last second of part 3 and the first of part 4 are
//      adjacent numbers, or the bar jumps back in the listener's face at every chapter.
//   3. NO SIZES, NO TOTAL. A release that does not say how big its parts are gets today's per-part
//      display, not a total that had to be invented.
//
// AND SINCE #197, A THIRD INPUT: per-part DURATIONS (an Audiobookshelf play session's tracks). Section 9
// pins the rule that picks between them — durations when every part has one, else sizes when every part
// has one, never a mix — and the exact mapping they give, both ways, at every boundary: the arithmetic a
// seek on the bar uses to land in the right track at the right offset.
//
// Prints BOOKTIMELINE-OK on success; any failure prints BOOKTIMELINE-FAIL <cond> (line) and exits non-zero.
#include "BookTimeline.h"

#include <QCoreApplication>
#include <QVector>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BOOKTIMELINE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool near(double a, double b, double eps = 0.01) { return std::fabs(a - b) <= eps; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the size text a source describes a part with -------------------------------------------
    // The field is a general subtitle, so a UNIT is what makes a number a size. Without that rule a row
    // reading "S1 · E2" would answer 1 byte and a whole book's timeline would be built out of it.
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("1 B")), 1.0));
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("2 KB")), 2048.0));
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("42.19 MB")), 42.19 * 1024 * 1024, 1.0));
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("1.5 GB")), 1.5 * 1024 * 1024 * 1024, 1.0));
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("42.19MB")), 42.19 * 1024 * 1024, 1.0)); // no space
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("42.19 MiB")), 42.19 * 1024 * 1024, 1.0));
    // The server formats this string under whatever culture it is running in, so the decimal separator is
    // whichever of the two comes LAST. Both of these are real strings from a real formatter.
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("42,19 MB")), 42.19 * 1024 * 1024, 1.0));
    CHECK(near(BookTimeline::bytesFromSizeText(QStringLiteral("1,024.5 MB")), 1024.5 * 1024 * 1024, 1.0));
    // ...and everything that is not a size answers nothing at all, which is what makes the fallback fire.
    CHECK(BookTimeline::bytesFromSizeText(QStringLiteral("S1 · E2")) == 0.0);
    CHECK(BookTimeline::bytesFromSizeText(QStringLiteral("Library Genesis")) == 0.0);
    CHECK(BookTimeline::bytesFromSizeText(QStringLiteral("128")) == 0.0);     // a bare number is not a size
    CHECK(BookTimeline::bytesFromSizeText(QString()) == 0.0);
    CHECK(BookTimeline::bytesFromSizeText(QStringLiteral("0 MB")) == 0.0);    // an empty part is no part
    CHECK(BookTimeline::bytesFromSizeText(QStringLiteral("12 furlongs")) == 0.0);

    // ---- 2. the seed: bytes become seconds, once ONE part has been opened --------------------------
    {
        // Three parts, 10 MB / 20 MB / 30 MB, and part 0 turns out to be 600 s. So a byte is worth
        // 600/10MB and the book is 3600 s.
        const double MB = 1024.0 * 1024.0;
        const QVector<double> bytes = { 10 * MB, 20 * MB, 30 * MB };
        const QVector<double> seed = BookTimeline::secondsFromBytes(bytes, 0, 600.0);
        CHECK(seed.size() == 3);
        CHECK(near(seed.value(0), 600.0));
        CHECK(near(seed.value(1), 1200.0));
        CHECK(near(seed.value(2), 1800.0));
        // The measured part keeps its MEASURED value exactly, never its own derived one — it is the fact
        // the other two are priced against, and a rounding of it would put an error into the anchor.
        CHECK(seed.value(0) == 600.0);

        // Calibrating off a part in the MIDDLE (a resume that starts at part 2) gives the same book.
        const QVector<double> fromMiddle = BookTimeline::secondsFromBytes(bytes, 2, 1800.0);
        CHECK(near(fromMiddle.value(0), 600.0));
        CHECK(near(fromMiddle.value(1), 1200.0));
        CHECK(near(fromMiddle.value(2), 1800.0));

        // THE UNIT DOES NOT MATTER. The same release measured in raw bytes and in kilobytes is the same
        // book, because only the ratios and the one real duration survive. This is why a source that
        // reports MB where it means MiB costs nothing.
        const QVector<double> kb = { 10 * 1024.0, 20 * 1024.0, 30 * 1024.0 };
        CHECK(near(BookTimeline::secondsFromBytes(kb, 0, 600.0).value(2), 1800.0));

        // ---- NO SIZES, NO TOTAL (the issue's own rule) --------------------------------------------
        CHECK(BookTimeline::secondsFromBytes({ 10 * MB, 0.0, 30 * MB }, 0, 600.0).isEmpty());
        CHECK(BookTimeline::secondsFromBytes({}, 0, 600.0).isEmpty());
        CHECK(BookTimeline::secondsFromBytes(bytes, 0, 0.0).isEmpty());     // nothing measured yet
        CHECK(BookTimeline::secondsFromBytes(bytes, 5, 600.0).isEmpty());   // a part that is not in the book
        // A SINGLE-PART release forms a seed but is not a book-scale timeline: one file already shows its
        // whole self, which is the case this whole issue is about being denied to the others.
        BookTimeline::Timeline one;
        one.seed(BookTimeline::secondsFromBytes({ 10 * MB }, 0, 600.0));
        CHECK(!one.ready());
    }

    // ---- 3. the timeline: offsets, total, elapsed --------------------------------------------------
    {
        BookTimeline::Timeline t;
        t.seed({ 600.0, 1200.0, 1800.0 });
        CHECK(t.ready());
        CHECK(t.parts() == 3);
        CHECK(near(t.total(), 3600.0));
        CHECK(near(t.offsetOf(0), 0.0));
        CHECK(near(t.offsetOf(1), 600.0));
        CHECK(near(t.offsetOf(2), 1800.0));
        // The number the listener reads: where they are IN THE BOOK, not in the part.
        CHECK(near(t.elapsed(1, 30.0), 630.0));
        CHECK(near(t.fraction(1, 30.0), 630.0 / 3600.0, 0.0001));
        // A position past the part's own end cannot push the reading past the boundary it is about to
        // cross — mpv reports a hair over the duration it declared often enough to matter.
        CHECK(near(t.elapsed(0, 900.0), 600.0));
        CHECK(near(t.fraction(2, 99999.0), 1.0, 0.0001));
        // An index that is not a part reads as nothing rather than as the start of the book.
        CHECK(near(t.elapsed(-1, 10.0), 0.0));
        CHECK(near(t.elapsed(9, 10.0), 0.0));

        BookTimeline::Timeline empty;
        CHECK(!empty.ready());
        CHECK(near(empty.total(), 0.0));
        CHECK(near(empty.fraction(0, 10.0), 0.0));
        // A seed carrying a part with no length is no seed — the same all-or-nothing rule as the sizes.
        BookTimeline::Timeline bad;
        bad.seed({ 600.0, 0.0, 1800.0 });
        CHECK(!bad.ready());
    }

    // ---- 4. THE TOTAL DOES NOT JITTER --------------------------------------------------------------
    {
        // Every part runs 5% longer than the sizes said. Under a model that simply recomputed, the total
        // would move at every one of these boundaries; here each correction comes out of the parts not
        // yet heard, so it does not move at all.
        BookTimeline::Timeline t;
        t.seed({ 600.0, 600.0, 600.0, 600.0, 600.0 });
        const double before = t.total();
        t.measure(0, 630.0);
        CHECK(near(t.total(), before));
        t.measure(1, 630.0);
        CHECK(near(t.total(), before));
        t.measure(2, 630.0);
        CHECK(near(t.total(), before));
        // ...and the fact is always what the part itself reads as, never the estimate it replaced.
        CHECK(near(t.lengthOf(0), 630.0));
        CHECK(t.isMeasured(0));
        CHECK(!t.isMeasured(4));
        // The unheard tail is where the correction went: two parts holding what was 1200 s of a 3000 s
        // book, less the 90 s the first three turned out to need.
        CHECK(near(t.lengthOf(3) + t.lengthOf(4), 1200.0 - 90.0));
        // ...shared out in proportion, so two equal parts stay equal.
        CHECK(near(t.lengthOf(3), t.lengthOf(4)));

        // A measurement of a part touches that part and the ones AFTER it, and nothing before — which is
        // the mechanism, not a side effect: it is what makes the elapsed reading monotone at a boundary.
        BookTimeline::Timeline u;
        u.seed({ 600.0, 600.0, 600.0, 600.0 });
        const double off2 = u.offsetOf(2);
        u.measure(2, 700.0);
        CHECK(near(u.offsetOf(2), off2));
        CHECK(near(u.offsetOf(1), 600.0));
    }

    // ---- 5. THE ELAPSED READING NEVER GOES BACKWARDS -----------------------------------------------
    {
        // The reported defect's shape, one boundary at a time: play a five-part book through, each part
        // running a little longer than its size implied, and assert at every boundary that the last
        // instant of part k and the first of part k+1 are adjacent rather than a jump backwards.
        BookTimeline::Timeline t;
        t.seed({ 600.0, 600.0, 600.0, 600.0, 600.0 });
        const double real[5] = { 641.0, 588.0, 655.0, 602.0, 573.0 };
        double last = 0.0;
        for (int k = 0; k < 5; ++k)
        {
            t.measure(k, real[k]);                 // mpv opens the part and says how long it is
            const double atStart = t.elapsed(k, 0.0);
            CHECK(atStart >= last - 0.001);        // …never behind where the previous part ended
            CHECK(near(atStart, last));            // …and exactly there, which is what "adjacent" means
            for (double p = 0.0; p <= real[k]; p += 61.0)
            {
                const double e = t.elapsed(k, p);
                CHECK(e >= last - 0.001);
                last = e;
            }
            last = t.elapsed(k, real[k]);
        }
        // Played to the very end, the reading IS the book's length: by the last part every length is a
        // measurement, so the estimate has become the truth.
        CHECK(near(last, t.total()));
        CHECK(near(t.total(), 641.0 + 588.0 + 655.0 + 602.0 + 573.0));
    }

    // ---- 6. when the correction cannot be absorbed, the total moves — and says so ------------------
    {
        // The LAST part is the case that always happens: there is nothing after it to take the
        // difference, so the total moves by exactly that difference. This is the one thing that changes
        // it, and it is the moment an estimate becomes a measurement.
        BookTimeline::Timeline t;
        t.seed({ 600.0, 600.0 });
        t.measure(1, 660.0);
        CHECK(near(t.total(), 1260.0));

        // A correction bigger than everything left to hear. Squeezing the tail to nothing would claim the
        // rest of the book takes no time at all — the bar would peg at the end while a part still played
        // — so the tail keeps the only estimate there is for it and the total moves instead.
        BookTimeline::Timeline u;
        u.seed({ 600.0, 100.0 });
        u.measure(0, 1800.0);
        CHECK(near(u.lengthOf(1), 100.0));
        CHECK(near(u.total(), 1900.0));

        // Nothing at all is claimed for a measurement that is not one: mpv reports 0 for a file it has
        // not finished opening, and taking that would zero a part of the book.
        BookTimeline::Timeline v;
        v.seed({ 600.0, 600.0 });
        v.measure(0, 0.0);
        CHECK(near(v.total(), 1200.0));
        v.measure(-1, 500.0);
        v.measure(7, 500.0);
        CHECK(near(v.total(), 1200.0));
    }

    // ---- 7. the clamp: a book-scale gesture, landing inside the part in hand ------------------------
    {
        // The bar spans the book but the app is holding ONE part's file, so a drag is turned into a
        // position within that part. Before it: the part's start. After it: the part's end. Never a
        // position in a file nothing has minted a link for.
        BookTimeline::Timeline t;
        t.seed({ 600.0, 600.0, 600.0 });
        CHECK(near(t.positionWithin(1, 900.0), 300.0));   // mid-part
        CHECK(near(t.positionWithin(1, 600.0), 0.0));     // exactly its start
        CHECK(near(t.positionWithin(1, 1200.0), 600.0));  // exactly its end
        CHECK(near(t.positionWithin(1, 10.0), 0.0));      // a drag pointing back into part 0
        CHECK(near(t.positionWithin(1, 3000.0), 600.0));  // ...and one pointing past the end of the book
        CHECK(near(t.positionWithin(0, 300.0), 300.0));
        CHECK(near(t.positionWithin(9, 300.0), 0.0));     // not a part at all
        // The clamp and the readout agree: a clamped gesture read back as an elapsed time lands on the
        // part's own edge, which is what the drag showed while it was being aimed.
        CHECK(near(t.elapsed(1, t.positionWithin(1, 10.0)), 600.0));
        CHECK(near(t.elapsed(1, t.positionWithin(1, 3000.0)), 1200.0));
    }

    // ---- 8. a LOCAL book (#139) is the same model with nothing derived ------------------------------
    {
        // Its per-file durations come out of the tags, so the seed IS the book: the total is right at the
        // first frame, and mpv's own duration for each part changes nothing it did not already know.
        BookTimeline::Timeline t;
        t.seed({ 1830.0, 1902.0, 1774.0 });
        CHECK(near(t.total(), 5506.0));
        t.measure(0, 1830.0);
        CHECK(near(t.total(), 5506.0));
        t.measure(1, 1902.0);
        CHECK(near(t.total(), 5506.0));
        // ...and a tag that was a second out is absorbed like any other correction rather than moving it.
        BookTimeline::Timeline u;
        u.seed({ 1830.0, 1902.0, 1774.0 });
        u.measure(0, 1831.0);
        CHECK(near(u.total(), 5506.0));
        CHECK(near(u.lengthOf(0), 1831.0));
    }

    // ---- 9. PER-PART DURATIONS (#197): the exact seed, and the rule that picks it ------------------
    {
        using BookTimeline::Basis;
        const double MB = 1024.0 * 1024.0;
        const QVector<double> durs  = { 100.0, 200.0, 150.0 };    // the stub server's three tracks
        const QVector<double> bytes = { 10 * MB, 20 * MB, 30 * MB };

        // THE RULE. Durations win when complete — even beside a complete set of sizes, because they are
        // exact and the sizes are an estimate still waiting for a measurement.
        CHECK(BookTimeline::basisFor(3, durs, {}) == Basis::Durations);
        CHECK(BookTimeline::basisFor(3, durs, bytes) == Basis::Durations);
        // THE SIZES FALLBACK, UNCHANGED: no durations at all is exactly the #218 torrent release.
        CHECK(BookTimeline::basisFor(3, {}, bytes) == Basis::Sizes);
        // A PARTIAL duration list falls back to the sizes, never mixed with them...
        CHECK(BookTimeline::basisFor(3, { 100.0, 0.0, 150.0 }, bytes) == Basis::Sizes);
        CHECK(BookTimeline::basisFor(3, { 100.0, 200.0 }, bytes) == Basis::Sizes);             // one short
        CHECK(BookTimeline::basisFor(3, { 100.0, 200.0, 150.0, 9.0 }, bytes) == Basis::Sizes); // one over
        // ...and zero, negative and NaN are "unknown", the same as absent.
        CHECK(BookTimeline::basisFor(3, { 100.0, -5.0, 150.0 }, bytes) == Basis::Sizes);
        CHECK(BookTimeline::basisFor(3, { 100.0, std::nan(""), 150.0 }, bytes) == Basis::Sizes);
        CHECK(BookTimeline::basisFor(3, { 0.0, 0.0, 0.0 }, bytes) == Basis::Sizes);
        // Neither complete: no book-scale timeline at all.
        CHECK(BookTimeline::basisFor(3, { 100.0, 0.0, 150.0 }, {}) == Basis::None);
        CHECK(BookTimeline::basisFor(3, { 100.0, 0.0, 150.0 }, { 10 * MB, 0.0, 30 * MB }) == Basis::None);
        CHECK(BookTimeline::basisFor(3, {}, {}) == Basis::None);
        CHECK(BookTimeline::basisFor(0, {}, {}) == Basis::None);

        // THE SEED each basis gives. Durations are the seed as they stand, before anything has played.
        CHECK(BookTimeline::seedFor(3, durs, bytes, -1, 0.0) == durs);
        // Sizes need the one measured part, and answer nothing until there is one...
        CHECK(BookTimeline::seedFor(3, {}, bytes, -1, 0.0).isEmpty());
        // ...and then EXACTLY what #218 always computed.
        CHECK(BookTimeline::seedFor(3, { 100.0, 0.0, 150.0 }, bytes, 0, 600.0)
              == BookTimeline::secondsFromBytes(bytes, 0, 600.0));
        CHECK(BookTimeline::seedFor(3, { 100.0, 0.0, 150.0 }, {}, 0, 600.0).isEmpty());

        // build(): the one entry that takes either input.
        BookTimeline::Timeline t;
        t.build(3, durs, bytes);
        CHECK(t.ready());
        CHECK(t.exact());
        CHECK(t.parts() == 3);
        CHECK(t.total() == 450.0);
        // bookPos = sum(durations[0..i-1]) + posInPart, exactly — no estimate anywhere in it.
        CHECK(t.offsetOf(0) == 0.0);
        CHECK(t.offsetOf(1) == 100.0);
        CHECK(t.offsetOf(2) == 300.0);
        CHECK(t.elapsed(1, 150.0) == 250.0);
        CHECK(t.elapsed(2, 40.0) == 340.0);

        // BOTH WAYS, AT EVERY BOUNDARY AND AT THE ENDS.
        for (int k = 0; k < 3; ++k)
        {
            const double off = t.offsetOf(k);
            const double len = t.lengthOf(k);
            CHECK(t.elapsed(k, 0.0) == off);                           // a part's top is its offset
            CHECK(t.elapsed(k, len) == off + len);                     // ...and its end, the next one's top
            const BookTimeline::Landing top = t.land(off);             // ON the boundary: the LATER part
            CHECK(top.part == k && top.within == 0.0);
            const BookTimeline::Landing mid = t.land(off + len / 2.0);
            CHECK(mid.part == k && near(mid.within, len / 2.0, 1e-9));
            if (k > 0)
            {
                // A hair before the boundary is still the EARLIER part, at its very end.
                const BookTimeline::Landing before = t.land(off - 0.001);
                CHECK(before.part == k - 1 && near(before.within, t.lengthOf(k - 1) - 0.001, 1e-9));
            }
        }
        const BookTimeline::Landing start = t.land(0.0);
        CHECK(start.part == 0 && start.within == 0.0);
        const BookTimeline::Landing beforeStart = t.land(-30.0);          // before the book: its top
        CHECK(beforeStart.part == 0 && beforeStart.within == 0.0);
        const BookTimeline::Landing end = t.land(450.0);                  // the very end: last part, its end
        CHECK(end.part == 2 && end.within == 150.0);
        const BookTimeline::Landing past = t.land(99999.0);               // past it: the same, never nothing
        CHECK(past.part == 2 && past.within == 150.0);
        // THE ROUND TRIP. Every point in the book lands somewhere that reads back as that same point.
        for (double T = 0.0; T <= 450.0; T += 12.5)
        {
            const BookTimeline::Landing l = t.land(T);
            CHECK(l.part >= 0 && near(t.elapsed(l.part, l.within), T, 1e-9));
        }

        // THE LIVE DRIVE'S BOOK: three 30-second tracks, a seek to 45 s lands in track 2 at 15 s.
        BookTimeline::Timeline tones;
        tones.build(3, { 30.0, 30.0, 30.0 }, {});
        CHECK(tones.ready() && tones.total() == 90.0);
        const BookTimeline::Landing at45 = tones.land(45.0);
        CHECK(at45.part == 1 && at45.within == 15.0);
        const BookTimeline::Landing at75 = tones.land(75.0);
        CHECK(at75.part == 2 && at75.within == 15.0);

        // AN EXACT TIMELINE IS NOT MOVED BY mpv. The server's lengths are the book time its progress is in;
        // a streamed file read a few milliseconds long must not pull the bar off that.
        tones.measure(0, 30.04);
        CHECK(tones.lengthOf(0) == 30.0);
        CHECK(tones.total() == 90.0);
        CHECK(tones.isMeasured(1));

        // ...but a timeline seeded the #218 way afterwards (a torrent book played next) is NOT exact, and
        // absorbs a measurement exactly as it always did.
        tones.seed({ 600.0, 600.0 });
        CHECK(!tones.exact());
        tones.measure(0, 630.0);
        CHECK(near(tones.lengthOf(0), 630.0) && near(tones.total(), 1200.0));

        // THE FALLBACK, THROUGH build(). A partial duration list beside complete sizes is the SIZES
        // timeline — nothing until one part is measured, then the very lengths #218 gave.
        BookTimeline::Timeline partial;
        partial.build(3, { 100.0, 0.0, 150.0 }, bytes);
        CHECK(!partial.ready());
        partial.build(3, { 100.0, 0.0, 150.0 }, bytes, 0, 600.0);
        BookTimeline::Timeline sizes;
        sizes.seed(BookTimeline::secondsFromBytes(bytes, 0, 600.0));
        CHECK(partial.ready() && !partial.exact());
        CHECK(partial.parts() == 3);
        for (int k = 0; k < 3; ++k) CHECK(partial.lengthOf(k) == sizes.lengthOf(k));
        CHECK(near(partial.total(), 3600.0));
        // ...a zero-duration list with no sizes is no timeline (the per-part display, as before #197)...
        BookTimeline::Timeline zero;
        zero.build(3, { 0.0, 0.0, 0.0 }, {});
        CHECK(!zero.ready() && zero.parts() == 0);
        // ...and build() over a timeline that was ready clears it rather than leaving the old book standing.
        BookTimeline::Timeline reused;
        reused.build(3, durs, {});
        reused.build(3, {}, {});
        CHECK(!reused.ready() && !reused.exact());

        // land() on an ESTIMATED timeline is the same arithmetic, and on an empty one it names no part.
        const BookTimeline::Landing est = sizes.land(700.0);
        CHECK(est.part == 1 && near(est.within, 100.0));
        BookTimeline::Timeline none;
        CHECK(none.land(10.0).part == -1);
    }

    if (failures == 0) { std::puts("BOOKTIMELINE-OK"); return 0; }
    std::fprintf(stderr, "BOOKTIMELINE: %d check(s) failed\n", failures);
    return 1;
}
