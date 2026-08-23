// Crossfade (issue #141): the pure, deterministic answer to "should the boundary between these two queue
// entries be crossfaded, and for how long?" Header-only + QtCore-only, so MpvWidget/MainWindow and
// probe_crossfade share ONE decision and no rule can drift between the code and its test — the same
// arrangement AudioOutput.h, RefreshSync.h, HdrOutput.h and ReplayGain.h use.
//
// WHY THE DECISION IS THE WHOLE FEATURE. Overlapping two decoders and ramping two gains is mechanical; #141
// says the judgement is in WHERE IT APPLIES, and it is emphatic about the failure mode: "crossfading a live
// album is vandalism". A crossfade that fires where it should not does audible damage to a record the user
// chose to play in order, and it does it silently and every time. A crossfade that fails to fire costs the
// user a nicety. Those two costs are not symmetric, so every rule below resolves ties toward NOT fading.
//
// THE FOUR SUPPRESSIONS, in the order they are applied:
//
//   1. OFF IS OFF. The setting is seconds, 0 == off, and off is the shipped default. Unlike ReplayGain this
//      is opt-in, for the reason above: ReplayGain only ever touches files somebody deliberately tagged,
//      while a crossfade rewrites every boundary it is allowed near.
//
//   2. MUSIC ONLY. An audiobook or a podcast is never crossfaded — dissolving the last sentence of a chapter
//      into the first of the next is not a stylistic choice, it is losing words. The split is NOT a new
//      notion of "is this music": it is exactly the one ReplayGain (#141) and the per-item speed (#140)
//      already make, handed in as `isMusic` by the single call site that computes it. Video is false through
//      that same expression, which is also #141's "never for video" — one bool, one rule, no second opinion.
//
//   3. NEVER ACROSS AN ALBUM BOUNDARY. Two consecutive tracks that carry the SAME album tag are two halves of
//      one record — a live set, a concept side, a DJ mix — and #141 names crossfading them as the thing not
//      to do. Compared case-insensitively on the trimmed tag and on the ALBUM TAG ALONE: deliberately not
//      album+artist, because a badly tagged compilation has a different artist on every track, and pairing
//      the two would un-suppress exactly the record most likely to be a continuous one. Two different records
//      that happen to share the title "Greatest Hits" therefore do not crossfade either, which is the
//      harmless direction of that trade.
//
//      The FOLDER is consulted only when NEITHER side carries an album tag. It can never override, weaken or
//      contradict a tag — it is the fallback for "we have no idea what record this is", where two files
//      sitting in one directory are one record far more often than they are not. This is the one rule here
//      that #141 does not spell out; it is separated into sameAlbum()'s second clause so it can be read,
//      probed and removed on its own.
//
//   4. NEVER LONGER THAN THE MUSIC. A 12 s fade across a 9 s skit is not a transition, it is two tracks
//      played at once. The window is capped at half of whichever side is shorter (a known length only —
//      0 means "we could not read one", and an unknown length constrains nothing), and a window that ends up
//      under a second is dropped entirely rather than shipped as an audible blip.
#pragma once
#include <QString>

#include <cmath>
namespace Crossfade
{
    // The setting's band, in seconds. #141 names 1-12 s ("Spotify's range"); 0 is off and is the default.
    inline int offSeconds()     { return 0; }
    inline int minSeconds()     { return 1; }
    inline int maxSeconds()     { return 12; }
    inline int defaultSeconds() { return offSeconds(); }

    // Stored value -> a value in the band. Anything below the minimum that is not exactly 0 rounds UP to the
    // minimum rather than down to off, so a hand-edited ini or an older build's value can never turn the
    // feature off by accident while still reading as "on" in the settings surface.
    inline int clampSeconds(int s)
    {
        if (s <= offSeconds()) return offSeconds();
        if (s < minSeconds())  return minSeconds();
        return s > maxSeconds() ? maxSeconds() : s;
    }

    // One side of a boundary, as the app already knows it: the album tag exactly as tagged (empty = untagged),
    // the containing folder (the untagged fallback — see rule 3), and the length in seconds (<= 0 = unknown).
    struct Track
    {
        QString album;
        QString folder;
        double  durationSec = 0.0;
    };

    // Rule 3. True when the two entries are, as far as anything we can read says, the same record.
    inline bool sameAlbum(const Track& a, const Track& b)
    {
        const QString aa = a.album.trimmed();
        const QString ba = b.album.trimmed();
        // Tagged: the tag decides, both ways. One side tagged and the other not is NOT the same album — a
        // tagged track and an untagged one are not evidence of one record, and falling through to the folder
        // here would let an untagged stray in an album folder suppress a boundary the tags disagree about.
        if (!aa.isEmpty() || !ba.isEmpty())
            return !aa.isEmpty() && !ba.isEmpty() && aa.compare(ba, Qt::CaseInsensitive) == 0;
        // Untagged on both sides: the directory is the only evidence of a record there is. Empty folders (a
        // stream, a path we could not resolve) are not evidence of anything and never match.
        const QString af = a.folder.trimmed();
        const QString bf = b.folder.trimmed();
        return !af.isEmpty() && !bf.isEmpty() && af.compare(bf, Qt::CaseInsensitive) == 0;
    }

    // THE DECISION. Returns the overlap in seconds, or 0 for "take this boundary as it comes".
    //
    // `setting` is Settings::crossfadeSeconds(); `isMusic` is the ONE music-vs-audiobook/podcast/video answer
    // the host already computed for ReplayGain (MainWindow::currentItemIsMusic()).
    inline double secondsFor(int setting, bool isMusic, const Track& outgoing, const Track& incoming)
    {
        const int want = clampSeconds(setting);
        // Rule 1, stated where it is read even though it cannot be the ONLY thing enforcing it: clampSeconds
        // has already turned every off-ish value into exactly 0, and a 0 would fall out of the bottom of this
        // function anyway (it is below minSeconds()). That makes this early exit REDUNDANT ON PURPOSE — it is
        // here so "off means off" is legible at the top of the decision rather than an emergent property of
        // two rules further down, and probe_crossfade's off cells pass with or without it. The rule itself is
        // pinned where it actually lives: the `clamp-off-becomes-one-second` mutant breaks clampSeconds' off
        // case and the probe goes red.
        if (want <= 0)                          return 0.0;  // 1: off
        if (!isMusic)                           return 0.0;  // 2: audiobook / podcast / video
        if (sameAlbum(outgoing, incoming))      return 0.0;  // 3: one record — never dissolve its seams
        // 4: cap at half of whichever known length is shorter, then refuse a fade too short to be one.
        double s = double(want);
        if (outgoing.durationSec > 0.0) s = qMin(s, outgoing.durationSec / 2.0);
        if (incoming.durationSec > 0.0) s = qMin(s, incoming.durationSec / 2.0);
        return s < double(minSeconds()) ? 0.0 : s;
    }

    // The equal-power pair for a window `t` of the way through (0 = the outgoing track alone, 1 = the incoming
    // track alone). #141 asks for equal power, and the reason is that the obvious linear ramp is wrong: two
    // uncorrelated signals sum in POWER, so a linear pair sums to 0.707 of full amplitude at the midpoint and
    // the transition audibly dips. sin/cos quadrature keeps out^2 + in^2 == 1 the whole way across, which is
    // flat. Clamped rather than asserted so a timer tick that overshoots the end of the window (a stalled GUI
    // thread, a long paint) lands on exactly (0, 1) instead of running the curve backwards past its quarter.
    inline double outgoingGain(double t)
    {
        const double x = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        return std::cos(x * 3.14159265358979323846 / 2.0);
    }
    inline double incomingGain(double t)
    {
        const double x = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        return std::sin(x * 3.14159265358979323846 / 2.0);
    }
}
