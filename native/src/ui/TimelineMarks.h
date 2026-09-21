// TimelineMarks — where a time, and a RANGE of time, land on a transport bar (issue #85, timeline markup).
//
// The seek bar knows two things about the file that nobody was drawing: where its chapters begin, and which
// stretches of it are the intro and the end credits. Both are already in the app — mpv's chapter list (or an
// Audiobookshelf book's, rebased: MainWindow::currentChapters) and the ARMED segment set that gatherSegments
// resolves out of the .edl / server / chapter / learned tiers. This header is the one place that turns either
// of them into pixels.
//
// WHY A HEADER, AND WHY IT IS PURE. There are two bars — the classic transport's SeekSlider and the themed
// now-playing page's QML one — and the failure to avoid is the two of them disagreeing about where a chapter
// is, because each did its own arithmetic. So the MODEL is built once (build()) and handed to both, and the
// GEOMETRY that turns a time into an x, or a range into a rect, is these functions and nothing else. No Qt
// GUI, no widget, no mpv: probe_playerbar exercises every rule below with no window and no file.
//
// THE RULES, each of which exists because its absence is a visible wrong mark:
//
//  * A mark outside [0, duration] is DROPPED, not clamped to the edge. Clamping a chapter at -1 s to x=0
//    invents a chapter boundary at 0:00 that the file does not have. A RANGE, by contrast, is TRIMMED: an
//    .edl written against a slightly different cut still tells the truth about the part that overlaps.
//  * duration <= 0 draws nothing at all. A length arrives asynchronously, so "no length yet" is a state the
//    bar is genuinely in, and every mark computed against zero would pile up on one column.
//  * Fewer than two chapters is not a chaptered file (the whole file as "chapter 1" is what mpv reports for
//    an unchaptered one), and a chapter at 0 is the start of the file rather than a boundary within it.
//  * Marks that land on the same column MERGE. Two chapters a second apart on a 600 px bar over a three-hour
//    film are the same pixel; painting both stacks two translucent strokes into one darker one, which reads
//    as a mark that means something different from its neighbours. Overlapping RANGES likewise merge (or, if
//    they are of different kinds, the later one is trimmed) so that no pixel is ever shaded twice.
#pragma once
#include "../core/MediaSegments.h"

#include <QRect>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace TimelineMarks
{
    // What a shaded band means. Deliberately NOT MediaSegments::SegmentType: only the two ranges the bar
    // draws are named here, so a Recap or a Commercial cannot reach a painter that has no colour for it.
    enum class BandKind { Intro, Credits };

    struct Band
    {
        double   start = 0.0;
        double   end   = 0.0;
        BandKind kind  = BandKind::Intro;
    };

    // Everything a bar needs to paint the markup for one file. `duration` travels WITH the marks because a
    // painter that mixed a fresh mark list with a stale length would place every mark wrongly and silently.
    struct Marks
    {
        QVector<double> ticks;            // chapter boundaries, seconds, sorted, none at 0
        QVector<Band>   bands;            // intro/credits, sorted, never overlapping
        double          duration = 0.0;

        bool isEmpty() const { return ticks.isEmpty() && bands.isEmpty(); }
    };

    // ---- pure geometry ------------------------------------------------------------------------------

    inline bool usable(double duration, int width) { return duration > 0.0 && width > 0; }

    // Where t sits along the bar, as a fraction in [0,1] — or -1 for "do not draw this", which is a time
    // outside the file or a file with no known length. The themed bar positions in fractions of its own
    // width, so this is the form it is fed; xForTime() below is the same answer in pixels.
    inline double fractionForTime(double t, double duration)
    {
        if (duration <= 0.0)          return -1.0;
        if (t < 0.0 || t > duration)  return -1.0;
        return t / duration;
    }

    // The pixel column for t over a bar `width` wide, or -1. The clamp at the far end is the one place a
    // time IS clamped rather than dropped: t == duration is inside the file, and floor(1.0 * width) would
    // put it one column past the last one the bar owns.
    inline int xForTime(double t, double duration, int width)
    {
        const double f = fractionForTime(t, duration);
        if (f < 0.0 || width <= 0) return -1;
        int x = int(std::floor(f * double(width)));
        if (x < 0)          x = 0;
        if (x > width - 1)  x = width - 1;
        return x;
    }

    // The rect [start,end] occupies over a bar of `width` x `height`, its origin at the bar's top-left.
    // start/end are TRIMMED into [0,duration] (see the header comment); a range with nothing left inside the
    // file returns a null rect, which every caller treats as "draw nothing".
    inline QRect rectForRange(double start, double end, double duration, int width, int height)
    {
        if (!usable(duration, width) || height <= 0) return QRect();
        const double s = std::max(0.0, std::min(start, duration));
        const double e = std::max(0.0, std::min(end,   duration));
        if (e <= s) return QRect();
        int x0 = int(std::floor(s / duration * double(width)));
        int x1 = int(std::ceil (e / duration * double(width)));
        if (x0 < 0) x0 = 0;
        if (x1 > width) x1 = width;
        if (x1 <= x0) x1 = std::min(width, x0 + 1);   // a range shorter than a pixel is still a range
        return QRect(x0, 0, x1 - x0, height);
    }

    // ---- the model ----------------------------------------------------------------------------------

    // Chapter boundaries worth drawing. Empty for an unchaptered file, for a single-chapter one, and for a
    // file whose length is not known yet.
    inline QVector<double> chapterTicks(const QVector<MediaSegments::Chapter>& chapters, double duration)
    {
        QVector<double> out;
        if (duration <= 0.0 || chapters.size() < 2) return out;
        out.reserve(chapters.size());
        for (const MediaSegments::Chapter& c : chapters)
        {
            // > 0 rather than >= 0: fractionForTime returns 0 for the chapter AT the start of the file and
            // -1 for one outside it, so this single test drops both.
            if (fractionForTime(c.time, duration) > 0.0) out.push_back(c.time);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // The armed segments, as bands. Recap and Commercial are stored by MediaSegments and acted on by nobody,
    // so they are not drawn either. The output NEVER overlaps: same-kind neighbours merge, different-kind
    // ones are trimmed, and the result is what keeps a doubly-shaded stretch off the bar.
    inline QVector<Band> segmentBands(const QVector<MediaSegments::Segment>& segs, double duration)
    {
        QVector<Band> raw;
        if (duration <= 0.0) return raw;
        raw.reserve(segs.size());
        for (const MediaSegments::Segment& s : segs)
        {
            BandKind kind = BandKind::Intro;
            if (s.type == MediaSegments::SegmentType::Intro)        kind = BandKind::Intro;
            else if (s.type == MediaSegments::SegmentType::Credits) kind = BandKind::Credits;
            else continue;
            const double a = std::max(0.0, std::min(s.start, duration));
            const double b = std::max(0.0, std::min(s.end,   duration));
            if (b <= a) continue;
            raw.push_back({ a, b, kind });
        }
        std::sort(raw.begin(), raw.end(), [](const Band& l, const Band& r) {
            return l.start != r.start ? l.start < r.start : l.end < r.end;
        });

        QVector<Band> merged;
        merged.reserve(raw.size());
        for (Band b : raw)
        {
            if (!merged.isEmpty())
            {
                Band& last = merged.back();
                if (b.start <= last.end)
                {
                    // Same kind: one band. Touching counts — two adjacent intro ranges are one intro, and a
                    // hairline seam between them would read as two.
                    if (b.kind == last.kind) { last.end = std::max(last.end, b.end); continue; }
                    // Different kinds: the earlier band keeps the overlap and the later one starts after it.
                    // (An intro and a credits range overlapping means one of the tiers is wrong; the bar's
                    // job is to stay readable rather than to arbitrate.)
                    b.start = last.end;
                    if (b.end <= b.start) continue;
                }
            }
            merged.push_back(b);
        }
        return merged;
    }

    // The one call both bars are fed from.
    inline Marks build(const QVector<MediaSegments::Chapter>& chapters,
                       const QVector<MediaSegments::Segment>& segments,
                       double duration)
    {
        Marks m;
        if (duration <= 0.0) return m;                 // no length: no marks, and no stale duration either
        m.duration = duration;
        m.ticks    = chapterTicks(chapters, duration);
        m.bands    = segmentBands(segments, duration);
        return m;
    }

    // ---- laying the model out on a bar --------------------------------------------------------------

    // The columns to paint, deduped. `ticks` must be sorted (chapterTicks sorts). minGapPx is the smallest
    // distance two ticks may be apart and still be two ticks; below it the second is dropped rather than
    // drawn over the first.
    inline QVector<int> tickPixels(const QVector<double>& ticks, double duration, int width, int minGapPx = 2)
    {
        QVector<int> out;
        if (!usable(duration, width)) return out;
        const int gap = std::max(1, minGapPx);
        out.reserve(ticks.size());
        for (double t : ticks)
        {
            const int x = xForTime(t, duration, width);
            if (x < 0) continue;
            if (!out.isEmpty() && x - out.back() < gap) continue;
            out.push_back(x);
        }
        return out;
    }

    // The same list for a bar that positions in fractions rather than pixels (the themed QML one). `offset`
    // and `total` exist for the audiobook case, where the themed bar is the whole BOOK's timeline and the
    // chapters belong to the part playing: the part starts `offset` seconds into a `total`-second bar. For
    // every other play they are 0 and the file's own duration, and this is fractionForTime unchanged.
    inline QVector<double> tickFractions(const QVector<double>& ticks, double offset, double total)
    {
        QVector<double> out;
        if (total <= 0.0) return out;
        out.reserve(ticks.size());
        for (double t : ticks)
        {
            const double f = fractionForTime(t + offset, total);
            if (f > 0.0) out.push_back(f);
        }
        return out;
    }
}
