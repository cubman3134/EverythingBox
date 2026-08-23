// The two ARITHMETIC halves of #142's lyric presentation, pulled out of the player so both can be pinned
// without a window, a file on disk or a running clock:
//
//   * SEEK-TO-A-LINE. Selecting a lyric line seeks playback to that line's timestamp — the thing the issue
//     calls "the feature that makes synced lyrics useful, not just pretty". The decision of WHICH second a
//     line maps to is here; the two surfaces (the themed page's lyric zone, the classic panel's list) only
//     hand a line index in and pass the seconds that come out to the player.
//   * THE PER-ITEM OFFSET. Community .lrc files are 95% right; the rest are a constant fraction of a second
//     out, in one direction, for the whole file. A ±0.5 s nudge, remembered per item (LyricOffsetStore),
//     fixes those without editing anyone's file.
//
// THE SIGN CONVENTION, stated once because everything else follows from it: the offset is added to every
// line's time, so the EFFECTIVE time of line i is `lines[i].timeSec + offset`.
//
//   * a POSITIVE offset makes the lyrics appear LATER (the words were running ahead of the singer);
//   * a NEGATIVE offset makes them appear EARLIER.
//
// That is the user-facing direction the two surfaces label ("Lyrics later" / "Lyrics earlier"). It is
// deliberately NOT the LRC file's own `[offset:]` tag convention, which is inverted (a positive [offset:] is
// subtracted, making lines appear earlier) — that tag is a property of the FILE and is already applied by
// LrcLyrics::parseLrc before anything here sees a time. This one is a property of the LISTENER'S copy of the
// track, applied on top, and it reads the way a person nudging a slider expects.
//
// Both consequences of that one convention are here rather than at the call sites, because they must agree:
// the current line is the last line whose effective time is at or before the position (so `pos - offset` goes
// into the lookup), and a seek to line i goes to its effective time (so `+ offset` comes out). Split those
// across two surfaces and a nudge would move the highlight without moving the seek.
//
// UNSYNCED LYRICS OFFER NO SEEK. A USLT sheet has no timestamps at all — LrcLyrics parks every line at 0.0 —
// so "seek to this line" has no answer and `canSeek` says so. Both surfaces ask before they offer the
// gesture: the themed page counts its lyric nav zone to 0, the classic panel does not make its rows
// activatable. Without that gate every line of an unsynced sheet would seek to 0:00, which is a silent,
// infuriating way to lose your place.
#pragma once
#include "LrcLyrics.h"

#include <algorithm>
#include <cmath>

namespace LyricSeek
{
    // The nudge step the issue asks for, and the range it may accumulate to. The clamp is not a guess about
    // how wrong a file can be — it is a floor under the damage a stuck key or a corrupt stored value can do:
    // ±30 s is far past any real LRC drift, and past it the lyrics are simply the wrong file.
    inline constexpr double kStepSec      = 0.5;
    inline constexpr double kMaxOffsetSec = 30.0;

    // Snap an offset onto the ±0.5 s grid and into range. Everything that stores or applies an offset goes
    // through here, so a hand-edited ini, a stale value from an older build or a NaN cannot put a fractional
    // or unbounded offset into the sync maths.
    inline double clampOffset(double offsetSec)
    {
        if (std::isnan(offsetSec)) return 0.0;
        double snapped = std::round(offsetSec / kStepSec) * kStepSec;
        snapped = std::max(-kMaxOffsetSec, std::min(kMaxOffsetSec, snapped));
        // -0.0 reads back as "an offset is set" through any sign test and prints as "-0.0 s". Fold it.
        return snapped == 0.0 ? 0.0 : snapped;
    }

    // Step an offset by `steps` nudges (+1 = half a second later, −1 = half a second earlier), clamped. At
    // the clamp the value stops rather than wrapping — a held key parks at ±30 s.
    inline double nudge(double offsetSec, int steps)
    {
        return clampOffset(clampOffset(offsetSec) + steps * kStepSec);
    }

    // Can a line of THIS lyric set be seeked to? Only a synced set with at least one line: an unsynced sheet
    // carries no times, so every "line" would seek to the same meaningless second.
    inline bool canSeek(const LrcLyrics::Lyrics& ly)
    {
        return ly.synced && !ly.lines.isEmpty();
    }

    // The current line at `posSec` with `offsetSec` applied — the last line whose EFFECTIVE time is at or
    // before the position, or −1 before the first line. Shifting the POSITION back by the offset is the same
    // comparison as shifting every line time forward by it, done once instead of per line. An unsynced set
    // has no current line at all (its times are all 0.0, so the raw lookup would report the LAST line from
    // the first second onwards and every line would look "current" to a highlight).
    inline int lineAt(const LrcLyrics::Lyrics& ly, double posSec, double offsetSec)
    {
        if (!canSeek(ly)) return -1;
        return LrcLyrics::lineIndexAtTime(ly, posSec - clampOffset(offsetSec));
    }

    // The playback second selecting `line` should seek to: that line's effective time, floored at 0 (a
    // negative offset on an early line would otherwise ask for a position before the start of the file).
    // Returns −1 for a set that cannot be seeked at all, or a line index outside it — the callers treat that
    // as "no seek", never as "seek to the beginning".
    inline double seekTarget(const LrcLyrics::Lyrics& ly, int line, double offsetSec)
    {
        if (!canSeek(ly) || line < 0 || line >= ly.lines.size()) return -1.0;
        return std::max(0.0, ly.lines[line].timeSec + clampOffset(offsetSec));
    }

    // "+0.5 s" / "−1.5 s" / "0 s" — the one spelling both surfaces show, so the themed chip and the classic
    // menu row cannot disagree about what is stored. Uses a real minus sign, not a hyphen.
    inline QString describe(double offsetSec)
    {
        const double v = clampOffset(offsetSec);
        if (v == 0.0) return QStringLiteral("0 s");
        const QString mag = QString::number(std::abs(v), 'f', 1);
        return (v > 0.0 ? QStringLiteral("+") : QString::fromUtf8("\xE2\x88\x92")) + mag + QStringLiteral(" s");
    }
}
