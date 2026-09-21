// Headless check of PlayerBarNav (src/ui/PlayerBarNav.h) — the two-state key contract for the player's
// transport BARS (the seek slider and the volume slider), the clamped arrow-step arithmetic, and the
// one-state table for the rest of the ROW (sections 9 and 10 below).
//
// The bars have two states. SELECTED: focused but inert, arrows walk the transport ring past them. ADJUSTING:
// arrows move the bar's value. This pins the whole table, because the states differ on only some keys and it
// is exactly the shared keys that make a bar feel like a trap when they are wrong:
//
//   * Left/Right are ring movement while Selected and value steps while Adjusting;
//   * Enter goes IN from Selected and comes OUT from Adjusting;
//   * Back comes out of Adjusting but is NOT ours while merely Selected — there it stays the player's unified
//     Back (stop + return home), which is what it does on every other transport control;
//   * Up/Down ALWAYS belong to the bar, in BOTH states. This is the non-obvious one: a QSlider treats Up/Down
//     as value steps and accepts them, so a key the bar declined would silently move a bar the user only meant
//     to step off. Same reason PageUp/PageDown/Home/End are swallowed rather than declined.
//
// Sections 12-17 cover the OTHER arithmetic on the same bar: TimelineMarks (issue #85), which turns a
// chapter time into a column and an intro/credits range into a rect, for both the classic and the themed
// transport out of one model.
//
// Prints PLAYERBAR-OK on success; any failure prints PLAYERBAR-FAIL <cond> and exits non-zero.
#include "PlayerBarNav.h"
#include "TimelineMarks.h"

#include <QRect>
#include <QVector>
#include <Qt>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PLAYERBAR-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    using eb::BarAct;
    const bool sel = false;   // Selected: focused, inert
    const bool adj = true;    // Adjusting: arrows move the value

    // 1. Left/Right: ring movement while Selected, value steps while Adjusting.
    CHECK(eb::barKey(Qt::Key_Left,  sel) == BarAct::FocusPrev);
    CHECK(eb::barKey(Qt::Key_Right, sel) == BarAct::FocusNext);
    CHECK(eb::barKey(Qt::Key_Left,  adj) == BarAct::StepDown);
    CHECK(eb::barKey(Qt::Key_Right, adj) == BarAct::StepUp);

    // 2. Enter/Return/Select: in from Selected, out from Adjusting. All three spellings, because a remote
    //    and a pad both arrive as Key_Select while a keyboard sends Return or Enter.
    for (int k : { Qt::Key_Return, Qt::Key_Enter, Qt::Key_Select })
    {
        CHECK(eb::barKey(k, sel) == BarAct::Enter);
        CHECK(eb::barKey(k, adj) == BarAct::Leave);
    }

    // 3. Back (all three spellings): out of Adjusting; not ours while Selected, where it must stay the
    //    player's unified Back. A bar that swallowed Back while merely focused would strand the user on the
    //    transport row with no way off the player at all.
    for (int k : { Qt::Key_Escape, Qt::Key_Backspace, Qt::Key_Back })
    {
        CHECK(eb::barKey(k, adj) == BarAct::Leave);
        CHECK(eb::barKey(k, sel) == BarAct::NotOurs);
    }

    // 4. Up/Down belong to the bar in BOTH states — see the header comment. Up goes to the ‹ Back overlay,
    //    Down re-lands on the transport row, and either one leaves Adjusting on the way.
    CHECK(eb::barKey(Qt::Key_Up,   sel) == BarAct::LeaveToBack);
    CHECK(eb::barKey(Qt::Key_Up,   adj) == BarAct::LeaveToBack);
    CHECK(eb::barKey(Qt::Key_Down, sel) == BarAct::LeaveToRow);
    CHECK(eb::barKey(Qt::Key_Down, adj) == BarAct::LeaveToRow);

    // 5. QSlider's OTHER value keys are swallowed in both states, never declined: declining them hands the
    //    slider a key it would act on, which is the same silent-value-change bug as Up/Down above.
    for (int k : { Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End })
    {
        CHECK(eb::barKey(k, sel) == BarAct::Consume);
        CHECK(eb::barKey(k, adj) == BarAct::Consume);
    }

    // 6. Anything else falls through untouched, in both states (F12 screenshot, M queue menu, S skip, [ and ]
    //    speed — every player shortcut has to keep working while a bar holds focus).
    for (int k : { Qt::Key_Space, Qt::Key_F12, Qt::Key_M, Qt::Key_S, Qt::Key_I,
                   Qt::Key_BracketLeft, Qt::Key_BracketRight, Qt::Key_Menu, Qt::Key_A })
    {
        CHECK(eb::barKey(k, sel) == BarAct::NotOurs);
        CHECK(eb::barKey(k, adj) == BarAct::NotOurs);
    }

    // 7. Step arithmetic clamps at both ends of both ranges and never overshoots.
    CHECK(eb::barStep(100, +1, eb::kVolumeStep, 0, 200) == 105);
    CHECK(eb::barStep(100, -1, eb::kVolumeStep, 0, 200) == 95);
    CHECK(eb::barStep(198, +1, eb::kVolumeStep, 0, 200) == 200);   // clamps, does not wrap to 203
    CHECK(eb::barStep(2,   -1, eb::kVolumeStep, 0, 200) == 0);     // clamps, does not go negative
    CHECK(eb::barStep(200, +1, eb::kVolumeStep, 0, 200) == 200);   // already at the top: idempotent
    CHECK(eb::barStep(0,   -1, eb::kVolumeStep, 0, 200) == 0);     // already at the bottom: idempotent
    CHECK(eb::barStep(500, +1, eb::kSeekStep, 0, 1000) == 510);
    CHECK(eb::barStep(500, -1, eb::kSeekStep, 0, 1000) == 490);
    CHECK(eb::barStep(995, +1, eb::kSeekStep, 0, 1000) == 1000);
    CHECK(eb::barStep(5,   -1, eb::kSeekStep, 0, 1000) == 0);

    // 8. The step sizes themselves are the design's numbers, not whatever happened to be typed: 40 presses
    //    across the volume range, 100 across the seek range (1% of duration per press).
    CHECK(eb::kVolumeStep == 5);
    CHECK(eb::kSeekStep == 10);
    CHECK(200 / eb::kVolumeStep == 40);   // 0..200 in half-steps of the 0..100 scale
    CHECK(1000 / eb::kSeekStep == 100);

    // 9. The transport ROW's table — every ring member that is not a bar (the transport buttons and the skip
    //    chip), plus the ‹ Back overlay above the row. The four arrows are the RING's, so that playerRing_
    //    and not Qt's creation-order tab chain decides where Left and Right go. Unlike a bar there is no
    //    second state: a button has a position in the row and nothing else.
    CHECK(eb::rowKey(Qt::Key_Left)  == eb::RowAct::FocusPrev);
    CHECK(eb::rowKey(Qt::Key_Right) == eb::RowAct::FocusNext);
    CHECK(eb::rowKey(Qt::Key_Up)    == eb::RowAct::FocusBack);
    CHECK(eb::rowKey(Qt::Key_Down)  == eb::RowAct::FocusRow);

    // 10. Everything else is declined, and the three groups are declined for three different reasons:
    //     * Enter/Return/Select and Space are how a focused button is PRESSED. Claiming them would not make
    //       the row asymmetric, it would make it dead.
    //     * Back, in all three spellings, must stay the player's unified Back — exactly the rule barKey
    //       applies to a merely Selected bar, and for the same reason: a row that swallowed Back would leave
    //       the user with no way off the player.
    //     * The player's own shortcuts (F12, M, S, I, [ and ]) have to keep working while the row has focus.
    //       PageUp/PageDown/Home/End are declined rather than swallowed here, unlike on a bar: a QPushButton
    //       does not act on them, so there is no silent value change to defend against.
    for (int k : { Qt::Key_Return, Qt::Key_Enter, Qt::Key_Select, Qt::Key_Space,
                   Qt::Key_Escape, Qt::Key_Backspace, Qt::Key_Back,
                   Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End,
                   Qt::Key_F12, Qt::Key_M, Qt::Key_S, Qt::Key_I, Qt::Key_Menu,
                   Qt::Key_BracketLeft, Qt::Key_BracketRight })
        CHECK(eb::rowKey(k) == eb::RowAct::NotOurs);

    // 11. One step around a focus ring of VISIBLE members, wrapping. The arithmetic is shared, because the
    //     player has TWO rings and they obey one rule: the transport row, and the TOP BAND above it (the
    //     "Back" overlay and, whenever the open media offers another release, "Issue with Streaming" drawn
    //     immediately to its right). The chip used to belong to no ring at all -- reachable by mouse and by
    //     nothing else -- and the shape of that bug is the last two checks here: an arrow inside a band must
    //     come to rest on a band member, never fall through onto the transport row.
    CHECK(eb::ringStep(12,  0, +1) ==  1);
    CHECK(eb::ringStep(12, 11, +1) ==  0);   // wraps forward round the transport row
    CHECK(eb::ringStep(12,  0, -1) == 11);   // and backward
    CHECK(eb::ringStep(2,   0, +1) ==  1);   // the two-member band: Right off Back reaches the chip
    CHECK(eb::ringStep(2,   1, +1) ==  0);
    CHECK(eb::ringStep(2,   0, -1) ==  1);   // Left off Back reaches it too, the far way round
    CHECK(eb::ringStep(2,   1, -1) ==  0);
    CHECK(eb::ringStep(1,   0, +1) ==  0);   // a one-member band (no chip): the arrow stays put...
    CHECK(eb::ringStep(1,   0, -1) ==  0);   // ...and does not leave the band
    CHECK(eb::ringStep(0,  -1, +1) == -1);   // nothing visible: there is no member to land on

    // ---------------------------------------------------------------------------------------------------
    // 12-17. TimelineMarks (src/ui/TimelineMarks.h) — issue #85's markup on the same bar: chapter ticks and
    // shaded intro/credit ranges. It lives here rather than beside the trickplay probe because it is bar
    // GEOMETRY, which is what this probe already is: the arithmetic that decides where on the transport a
    // press, a step, or now a mark lands. Two surfaces draw these marks (the classic SeekSlider and the
    // themed now-playing bar) and section 17 is the assertion that they agree.
    using TimelineMarks::BandKind;

    // 12. time -> x, over a 100 px bar of a 200 s file. The ends are the whole point: 0 is the first column,
    //     the duration is the LAST one (not one past it), and anything outside the file is not drawn at all.
    CHECK(TimelineMarks::xForTime(0.0,   200.0, 100) == 0);
    CHECK(TimelineMarks::xForTime(100.0, 200.0, 100) == 50);
    CHECK(TimelineMarks::xForTime(199.0, 200.0, 100) == 99);
    CHECK(TimelineMarks::xForTime(200.0, 200.0, 100) == 99);   // the last column, not 100
    CHECK(TimelineMarks::xForTime(200.5, 200.0, 100) == -1);   // past the end: dropped, NOT clamped to 99
    CHECK(TimelineMarks::xForTime(-1.0,  200.0, 100) == -1);   // before the start: dropped, not pinned to 0
    CHECK(TimelineMarks::xForTime(50.0,    0.0, 100) == -1);   // no length yet: nothing is drawable
    CHECK(TimelineMarks::xForTime(50.0,  200.0,   0) == -1);   // no bar yet either
    CHECK(TimelineMarks::fractionForTime(50.0, 200.0) == 0.25);
    CHECK(TimelineMarks::fractionForTime(-0.1, 200.0) < 0.0);
    CHECK(TimelineMarks::fractionForTime(201.0, 200.0) < 0.0);

    // 13. range -> rect, including the two ways a range can hang off the end of the file. A range is TRIMMED
    //     where a tick is dropped: an .edl cut against a slightly different release still tells the truth
    //     about the part of it that overlaps this one.
    {
        const QRect mid = TimelineMarks::rectForRange(50.0, 100.0, 200.0, 100, 6);
        CHECK(mid == QRect(25, 0, 25, 6));
        const QRect early = TimelineMarks::rectForRange(-30.0, 20.0, 200.0, 100, 6);
        CHECK(early.x() == 0);                       // trimmed at the start...
        CHECK(early.right() == 9);
        const QRect late = TimelineMarks::rectForRange(180.0, 900.0, 200.0, 100, 6);
        CHECK(late.x() == 90);
        CHECK(late.x() + late.width() == 100);       // ...and at the end, never past the bar
        CHECK(TimelineMarks::rectForRange(10.0, 20.0, 0.0, 100, 6).isNull());     // no length
        CHECK(TimelineMarks::rectForRange(20.0, 10.0, 200.0, 100, 6).isNull());   // inverted
        CHECK(TimelineMarks::rectForRange(300.0, 400.0, 200.0, 100, 6).isNull()); // wholly outside
        // Shorter than a pixel is still a range: it gets one column rather than a zero-width rect that
        // paints nothing and leaves the user looking for a band the bar said existed.
        CHECK(TimelineMarks::rectForRange(10.0, 10.5, 200.0, 100, 6).width() == 1);
    }

    // 14. Chapter ticks. A single "chapter" spanning the file is what mpv reports for an UNCHAPTERED one, so
    //     two is the floor — the same count the transport's chapter buttons appear at. The chapter at 0 is
    //     the start of the file, not a boundary inside it.
    {
        const QVector<MediaSegments::Chapter> one  = { { 0.0, QStringLiteral("All") } };
        const QVector<MediaSegments::Chapter> four = { { 0.0,   QStringLiteral("Open") },
                                                      { 60.0,  QStringLiteral("Two")  },
                                                      { 120.0, QStringLiteral("Three")},
                                                      { 190.0, QStringLiteral("End")  } };
        CHECK(TimelineMarks::chapterTicks(one,  200.0).isEmpty());   // one chapter: nothing to mark
        CHECK(TimelineMarks::chapterTicks({},   200.0).isEmpty());   // none at all
        CHECK(TimelineMarks::chapterTicks(four,   0.0).isEmpty());   // length unknown
        const QVector<double> t = TimelineMarks::chapterTicks(four, 200.0);
        CHECK(t.size() == 3);                                        // the 0.0 one is not drawn
        CHECK(t.value(0) == 60.0);
        CHECK(t.value(2) == 190.0);
        // Out of range on either side, and out of order coming in.
        const QVector<MediaSegments::Chapter> odd = { { 190.0, {} }, { -5.0, {} }, { 60.0, {} },
                                                      { 500.0, {} }, { 0.0, {} } };
        const QVector<double> ot = TimelineMarks::chapterTicks(odd, 200.0);
        CHECK(ot.size() == 2);
        CHECK(ot.value(0) == 60.0);
        CHECK(ot.value(1) == 190.0);
    }

    // 15. Near-identical ticks MERGE into one column. Without this, two chapters a second apart on a long
    //     film paint two translucent strokes onto the same pixel and that pixel reads darker — a mark that
    //     looks like it means something the others do not.
    {
        const double dur = 7200.0;                  // a two-hour film on a 600 px bar: 12 s per column
        const QVector<double> close = { 600.0, 603.0, 606.0, 3600.0 };
        const QVector<int> px = TimelineMarks::tickPixels(close, dur, 600);
        CHECK(px.size() == 2);                       // the three within half a column are one tick
        CHECK(px.value(0) == 50);
        CHECK(px.value(1) == 300);
        // Far apart, nothing merges; and a bar with no width paints nothing.
        CHECK(TimelineMarks::tickPixels({ 600.0, 1800.0, 3600.0 }, dur, 600).size() == 3);
        CHECK(TimelineMarks::tickPixels(close, dur, 0).isEmpty());
        CHECK(TimelineMarks::tickPixels(close, 0.0, 600).isEmpty());
    }

    // 16. Bands: kind filtering, trimming, and the overlap merge. The output must NEVER overlap — a pixel
    //     shaded twice is a pixel darker than the rest of the same band, which reads as a third kind of
    //     range that does not exist.
    {
        using MediaSegments::Segment;
        using MediaSegments::SegmentType;
        const QVector<Segment> segs = {
            { 10.0, 40.0,  SegmentType::Intro },
            { 30.0, 60.0,  SegmentType::Intro },      // overlaps the first: one band, 10..60
            { 90.0, 95.0,  SegmentType::Recap },      // stored, never drawn
            { 96.0, 99.0,  SegmentType::Commercial }, // ditto
            { 180.0, 260.0, SegmentType::Credits },   // runs past the end: trimmed to the duration
        };
        const QVector<TimelineMarks::Band> b = TimelineMarks::segmentBands(segs, 200.0);
        CHECK(b.size() == 2);
        CHECK(b.value(0).start == 10.0);
        CHECK(b.value(0).end   == 60.0);
        CHECK(b.value(0).kind  == BandKind::Intro);
        CHECK(b.value(1).start == 180.0);
        CHECK(b.value(1).end   == 200.0);
        CHECK(b.value(1).kind  == BandKind::Credits);
        // Touching same-kind ranges are one band, not two with a seam.
        const QVector<Segment> touch = { { 10.0, 30.0, SegmentType::Intro }, { 30.0, 50.0, SegmentType::Intro } };
        CHECK(TimelineMarks::segmentBands(touch, 200.0).size() == 1);
        CHECK(TimelineMarks::segmentBands(touch, 200.0).value(0).end == 50.0);
        // Different kinds that overlap: the later one starts where the earlier one ends. No shared pixel.
        const QVector<Segment> clash = { { 10.0, 60.0, SegmentType::Intro }, { 40.0, 90.0, SegmentType::Credits } };
        const QVector<TimelineMarks::Band> cb = TimelineMarks::segmentBands(clash, 200.0);
        CHECK(cb.size() == 2);
        CHECK(cb.value(0).end == 60.0);
        CHECK(cb.value(1).start == 60.0);            // trimmed, not overlapping
        CHECK(cb.value(1).end == 90.0);
        // Nothing survives: no length, a zero-length range, an inverted one, a wholly-outside one.
        CHECK(TimelineMarks::segmentBands(segs, 0.0).isEmpty());
        CHECK(TimelineMarks::segmentBands({ { 20.0, 20.0, SegmentType::Intro } }, 200.0).isEmpty());
        CHECK(TimelineMarks::segmentBands({ { 50.0, 20.0, SegmentType::Intro } }, 200.0).isEmpty());
        CHECK(TimelineMarks::segmentBands({ { 300.0, 400.0, SegmentType::Intro } }, 200.0).isEmpty());
    }

    // 17. ONE model, BOTH bars. build() is what MainWindow hands the classic SeekSlider and, as fractions,
    //     the themed now-playing bar: the check is that the two layouts place the SAME mark in the same
    //     place, because each computing its own would be the bug this header exists to prevent.
    {
        using MediaSegments::Segment;
        using MediaSegments::SegmentType;
        const QVector<MediaSegments::Chapter> chaps = { { 0.0, {} }, { 50.0, {} }, { 100.0, {} }, { 150.0, {} } };
        const QVector<Segment> segs = { { 0.0, 40.0, SegmentType::Intro },
                                        { 190.0, 200.0, SegmentType::Credits } };
        const TimelineMarks::Marks m = TimelineMarks::build(chaps, segs, 200.0);
        CHECK(m.duration == 200.0);
        CHECK(m.ticks.size() == 3);
        CHECK(m.bands.size() == 2);
        CHECK(!m.isEmpty());

        const int width = 400;
        const QVector<int>    px = TimelineMarks::tickPixels(m.ticks, m.duration, width);
        const QVector<double> fr = TimelineMarks::tickFractions(m.ticks, 0.0, m.duration);
        CHECK(px.size() == fr.size());
        for (int i = 0; i < px.size() && i < fr.size(); ++i)
            CHECK(int(fr.value(i) * width) == px.value(i));

        // The audiobook case (#218): the themed bar is the whole BOOK's timeline, so this part's chapters sit
        // `offset` seconds along a `total`-second bar. Same times, same header, one extra argument.
        const QVector<double> bookFr = TimelineMarks::tickFractions(m.ticks, 200.0, 800.0);
        CHECK(bookFr.size() == 3);
        CHECK(bookFr.value(0) == 0.3125);            // (200 + 50) / 800
        CHECK(TimelineMarks::tickFractions(m.ticks, 0.0, 0.0).isEmpty());

        // Duration 0 and a one-chapter item produce nothing at all, through the same front door.
        const TimelineMarks::Marks none = TimelineMarks::build(chaps, segs, 0.0);
        CHECK(none.isEmpty());
        CHECK(none.duration == 0.0);
        const TimelineMarks::Marks lone = TimelineMarks::build({ { 0.0, {} } }, {}, 200.0);
        CHECK(lone.ticks.isEmpty());
        CHECK(lone.isEmpty());
    }

    if (failures) { std::fprintf(stderr, "PLAYERBAR-FAIL %d check(s)\n", failures); return 1; }
    std::printf("PLAYERBAR-OK\n");
    return 0;
}
