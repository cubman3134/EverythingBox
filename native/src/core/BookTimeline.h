// THE WHOLE BOOK ON THE TIMELINE, FOR A BOOK THAT IS MANY FILES (issue #218).
//
// THE ASYMMETRY THIS EXISTS TO REMOVE. A single-file .m4b shows the whole book on the position bar,
// because mpv knows that one file's length the moment it loads. The same book as a 57-part release
// showed "45:53 / 45:54" while playing part one of fifteen hours — mpv knows the length of the file it
// is playing and nothing about the other fifty-six. So the same work read completely differently
// depending on how somebody happened to package it, and the number a listener actually wants ("how much
// is left") was the one number neither surface had.
//
// #139 already settled that a multi-file audiobook is ONE BOOK with one resume point. The timeline is
// that same claim drawn on screen, and until this file existed it contradicted it.
//
// ---- WHAT A TIMELINE IS MADE OF, AND WHY IT IS AN ESTIMATE -----------------------------------------
//
// Two inputs, and they are not the same kind of thing:
//
//   * a per-part SEED — a length for every part, from whatever the caller has before playing anything.
//     For a remote release that is the part's SIZE IN BYTES scaled by the bytes-per-second of the one
//     part mpv has actually opened (secondsFromBytes below). For a local book (#139) it is the exact
//     per-file duration the tags already gave the library, and then the seed is not an estimate at all.
//
//   * a MEASUREMENT — mpv's exact duration for a part, which arrives when that part loads.
//
// A seed from bytes is wrong in the way a derived number is always wrong: VBR, tags, and a per-part
// preamble all move bytes-per-second around a little. It is wrong by a fraction of a percent for the
// ordinary case, because the parts of one release come out of ONE encode at ONE bitrate.
//
// THE UNIT THE BYTES ARE IN DOES NOT MATTER, which is worth stating because it looks as if it should.
// The seed is bytes_i / bps and bps is bytes_k / measuredSeconds_k, so any constant factor in the byte
// figures — 1000-vs-1024, a size read as MB that was MiB — cancels exactly. Only the RATIOS between the
// parts survive into the answer, plus the one real duration that sets the scale.
//
// ---- THE JITTER POLICY, WHICH IS THE ACTUAL DESIGN DECISION ----------------------------------------
//
// A total that visibly changes at every part boundary is worse than one that is stable and slightly
// wrong: it turns "how long is this book" into something the screen argues with itself about, 57 times.
// But a measurement is a FACT, and a part whose published length disagreed with its measured duration
// would send the elapsed reading BACKWARDS as the boundary was crossed — the one thing a position
// readout may never do.
//
// Both are satisfied at once, by absorbing rather than by recomputing:
//
//   * a measurement always becomes that part's published length (facts win, so the boundary is exact);
//   * the difference it made is taken OUT OF THE PARTS NOT YET HEARD, in proportion to what they were
//     already believed to be, so the total does not move;
//   * a measurement of part k touches part k and the parts AFTER it, and nothing before — which is what
//     makes the elapsed reading monotone across a boundary, by construction rather than by luck;
//   * when there is nothing left to absorb into (the last part, or a tail too short to take the
//     correction), the total simply moves. That is the only thing that can change it.
//
// So for ordinary sequential listening the total is fixed at the first part that loads and moves exactly
// once, at the final boundary, by the residue of an estimate that has been absorbing its own error all
// along — usually seconds. That is stated as a property and pinned by probe_booktimeline.
//
// ---- WHAT THIS FILE DOES NOT DECIDE ----------------------------------------------------------------
//
// Whether a book-scale bar may be DRAGGED across a part boundary. It cannot, and the reason is not here:
// crossing a boundary means minting the target part's link, which is a debrid resolve of the whole
// release, and #216 is the issue of one of those taking sixty-five seconds and coming back with nothing.
// The caller clamps the gesture into the current part's span, for which this file supplies the span
// (offsetOf / lengthOf) and the arithmetic (positionWithin). The argument lives with the clamp.
//
// Nothing here reads Settings, touches the filesystem, knows what a network or a player is, or holds any
// Qt type but QVector. Every method is a pure function of the state above it, so probe_booktimeline can
// drive the whole model — including a boundary — with no window and no mpv.
#pragma once
#include <QChar>
#include <QString>
#include <QVector>

#include <cmath>

namespace BookTimeline
{
// A part's size in bytes, out of the text a source described it with ("42.19 MB"). 0 for anything this
// cannot read, which the caller must treat as "no timeline" rather than as "an empty part".
//
// A UNIT IS REQUIRED, and that is the whole safety of this function. The field it parses is a general
// subtitle — a source that puts "S1 · E2" or "Library Genesis" there must produce nothing, not 1. So a
// bare number answers 0, and only a number followed by a size unit is a size.
//
// The decimal separator is whichever of '.' and ',' comes LAST, because the string was formatted by a
// server under whatever culture it was running in: "42,19 MB" and "1,024.5 MB" are both real and they
// disagree about which character is which. Taking the last one is right for both.
inline double bytesFromSizeText(const QString& text)
{
    const QString s = text.trimmed();
    if (s.isEmpty()) return 0.0;

    // The number: the first run of digits, with any separators inside it.
    int i = 0;
    while (i < s.size() && !s.at(i).isDigit()) ++i;
    if (i >= s.size()) return 0.0;
    const int numStart = i;
    while (i < s.size() && (s.at(i).isDigit() || s.at(i) == QLatin1Char('.') || s.at(i) == QLatin1Char(',')))
        ++i;
    QString num = s.mid(numStart, i - numStart);
    while (num.endsWith(QLatin1Char('.')) || num.endsWith(QLatin1Char(','))) num.chop(1);
    if (num.isEmpty()) return 0.0;

    const int lastDot = num.lastIndexOf(QLatin1Char('.'));
    const int lastCom = num.lastIndexOf(QLatin1Char(','));
    const int dec = lastDot > lastCom ? lastDot : lastCom;
    QString whole = dec >= 0 ? num.left(dec) : num;
    const QString frac = dec >= 0 ? num.mid(dec + 1) : QString();
    whole.remove(QLatin1Char('.')).remove(QLatin1Char(','));
    bool ok = false;
    double value = whole.isEmpty() ? 0.0 : whole.toDouble(&ok);
    if (!whole.isEmpty() && !ok) return 0.0;
    if (!frac.isEmpty())
    {
        bool fok = false;
        const double f = frac.toDouble(&fok);
        if (!fok) return 0.0;
        value += f / std::pow(10.0, double(frac.size()));
    }
    if (value <= 0.0) return 0.0;

    // The unit, immediately after the number bar whitespace. Binary multipliers, matching the server's
    // own formatter — and see the header: a constant factor cancels out of every answer this feeds.
    while (i < s.size() && s.at(i).isSpace()) ++i;
    QString unit;
    while (i < s.size() && s.at(i).isLetter()) unit.append(s.at(i++));
    unit = unit.toUpper();
    if (unit == QLatin1String("B"))                                              return value;
    if (unit == QLatin1String("K") || unit == QLatin1String("KB") || unit == QLatin1String("KIB")) return value * 1024.0;
    if (unit == QLatin1String("M") || unit == QLatin1String("MB") || unit == QLatin1String("MIB")) return value * 1024.0 * 1024.0;
    if (unit == QLatin1String("G") || unit == QLatin1String("GB") || unit == QLatin1String("GIB")) return value * 1024.0 * 1024.0 * 1024.0;
    if (unit == QLatin1String("T") || unit == QLatin1String("TB") || unit == QLatin1String("TIB")) return value * 1024.0 * 1024.0 * 1024.0 * 1024.0;
    return 0.0;
}

// The per-part seed a release's byte sizes give, once ONE part's real duration is known.
//
// Returns an empty vector — meaning NO BOOK-SCALE TIMELINE — for every case where the answer would have
// to be invented: a part whose size could not be read (the issue's own rule: fall back to the per-part
// display rather than show a total that cannot be supported), a measured part with no size to calibrate
// against, and a measurement of zero.
inline QVector<double> secondsFromBytes(const QVector<double>& bytes, int measuredIndex, double measuredSeconds)
{
    if (bytes.isEmpty() || measuredSeconds <= 0.0) return {};
    if (measuredIndex < 0 || measuredIndex >= bytes.size()) return {};
    for (double b : bytes)
        if (!(b > 0.0)) return {};
    const double bps = bytes.at(measuredIndex) / measuredSeconds;
    if (!(bps > 0.0)) return {};
    QVector<double> out;
    out.reserve(bytes.size());
    for (double b : bytes) out.push_back(b / bps);
    out[measuredIndex] = measuredSeconds;
    return out;
}

// The book's published timeline: one length per part, and which of them are measurements rather than
// estimates. Empty (parts() == 0) means there is no book-scale timeline and the caller shows the part.
class Timeline
{
public:
    // Install the seed. Any empty seed, or one carrying a non-positive length, leaves this empty — the
    // caller's cue to keep today's per-part display.
    void seed(const QVector<double>& lengths)
    {
        lengths_.clear();
        measured_.clear();
        if (lengths.isEmpty()) return;
        for (double d : lengths)
            if (!(d > 0.0)) return;
        lengths_ = lengths;
        measured_.fill(false, lengths.size());
    }

    void clear() { lengths_.clear(); measured_.clear(); }
    bool ready() const { return lengths_.size() > 1; }
    int parts() const { return lengths_.size(); }
    bool isMeasured(int i) const { return measured_.value(i, false); }

    // mpv opened part k and said how long it is. See the header for the whole of the policy this is:
    // the fact is published, and the difference it made is taken out of the parts not yet heard.
    void measure(int k, double seconds)
    {
        if (k < 0 || k >= lengths_.size() || !(seconds > 0.0)) return;
        const double delta = seconds - lengths_.at(k);
        lengths_[k] = seconds;
        measured_[k] = true;
        if (std::fabs(delta) < 0.001) return;

        double pool = 0.0;
        for (int i = k + 1; i < lengths_.size(); ++i)
            if (!measured_.at(i)) pool += lengths_.at(i);
        // Nothing after this part is still an estimate — the last part, or a listener who has heard the
        // tail already. The correction has nowhere to go, so the TOTAL moves by it. This is the one path
        // that changes the total, and it is the honest one: the estimate has become a measurement.
        if (!(pool > 0.0)) return;
        const double scale = (pool - delta) / pool;
        // A correction larger than everything left to hear. Squeezing the tail to nothing would claim the
        // rest of the book takes no time; the total moves instead, and the tail keeps the only estimate
        // there is for it.
        if (!(scale > 0.0)) return;
        for (int i = k + 1; i < lengths_.size(); ++i)
            if (!measured_.at(i)) lengths_[i] *= scale;
    }

    double lengthOf(int i) const { return lengths_.value(i, 0.0); }

    // Where part i starts in the book. A sum rather than a cached running total on purpose: the lengths
    // move under absorption, and a cache is how one of them would go stale against the other.
    double offsetOf(int i) const
    {
        double at = 0.0;
        for (int k = 0; k < i && k < lengths_.size(); ++k) at += lengths_.at(k);
        return at;
    }

    double total() const
    {
        double t = 0.0;
        for (double d : lengths_) t += d;
        return t;
    }

    // The position IN THE BOOK, for a position in part i. Clamped into the part's own span, so a player
    // that reports a second past the end it declared cannot push the reading past the boundary it is
    // about to cross.
    double elapsed(int i, double positionInPart) const
    {
        if (i < 0 || i >= lengths_.size()) return 0.0;
        const double len = lengths_.at(i);
        const double p = positionInPart < 0.0 ? 0.0 : (positionInPart > len ? len : positionInPart);
        return offsetOf(i) + p;
    }

    double fraction(int i, double positionInPart) const
    {
        const double t = total();
        if (!(t > 0.0)) return 0.0;
        const double f = elapsed(i, positionInPart) / t;
        return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    }

    // THE CLAMP. A book-scale gesture names a point in the BOOK; this is where it lands inside part i,
    // which is the only part there is a link for. A point before the part starts is its start and a point
    // after it ends is its end — the knob stops at the edge of the part rather than the gesture being
    // refused, so what the drag shows is always what the release will do.
    double positionWithin(int i, double bookSeconds) const
    {
        if (i < 0 || i >= lengths_.size()) return 0.0;
        const double rel = bookSeconds - offsetOf(i);
        const double len = lengths_.at(i);
        return rel < 0.0 ? 0.0 : (rel > len ? len : rel);
    }

private:
    QVector<double> lengths_;
    QVector<bool>   measured_;
};
} // namespace BookTimeline
