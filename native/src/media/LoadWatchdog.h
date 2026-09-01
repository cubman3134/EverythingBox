// A LOAD THAT SAYS NOTHING (issue #213) — the pure rules, and nothing else.
//
// WHY THIS FILE EXISTS. #228 fixed what happens when mpv REFUSES a file: END_FILE with reason ERROR reaches
// the host, which stops and zeroes the transport and says why. That event is emitted only when mpv gives up.
// A source that is dead or throttled does not make mpv give up. The connection opens, mpv emits START_FILE,
// and then nothing — no FILE_LOADED, no END_FILE, no positions — for as long as anyone cares to wait. The
// page shows a cover, 0:00 and a dead transport, and the app has nothing to say because as far as it knows
// the load is still in progress. That is the commoner shape when a source is degraded rather than gone, and
// it was the one #213 was filed from.
//
// So silence is turned into a failure by a deadline. The care is in WHICH deadline: a cold debrid link that
// is warming up must not be killed, and the issue's own guidance is followed — prefer "no bytes and no
// file-loaded event at all" over a wall-clock guess about speed. Hence two phases:
//
//   1. At the first deadline mpv is asked whether the file has made progress. The answer has THREE values,
//      and the middle one is the whole point of this being a table:
//        Some    — a demuxer exists and has parsed something: slow but alive, ONE re-grace.
//        Unknown — mpv cannot say. This is what a link that is still warming up looks like: until enough
//                  bytes arrive to identify the format there is no demuxer, and no demuxer-level property
//                  exists to ask. Measured live (2026-09-01): a server that sent headers and 4 KiB then hung
//                  reads exactly like one that sent nothing. Unknown is therefore NOT "no bytes" — treating
//                  it so is precisely the slow-link kill this design exists to avoid — so it re-graces too.
//        None    — a demuxer exists and reports an EMPTY cache at the deadline. That is positive evidence:
//                  the format was identified, and nothing has come since. Dead, stall now.
//   2. At the second deadline it is a stall whatever the answer: this is the "buffered a little and then
//      died" shape, and a single no-progress test would let it hang forever. Phase two never re-arms, which
//      is what makes "a load that never progresses always terminates" a property rather than a hope.
//
// And one exclusion: live/HLS links are not watched at all. They buffer differently, a channel merely slow
// to open would be turned into an error message, and IPTV has its own skip handling. The test is the same
// one the app already uses to classify HLS (an .m3u8 / .m3u extension), applied conservatively — a VOD
// .m3u8 is excluded too, which for a watchdog is the right direction to be wrong in.
//
// NOTHING HERE TOUCHES THE WORLD. No Qt, no mpv, no clock. MpvWidget owns the timer and the property query
// and asks this table what they mean. Same relationship to the host that PlaybackFailure.h has.
#pragma once

#include <string_view>

namespace LoadWatchdog
{
    // ---- The two deadlines --------------------------------------------------------------------------------
    // 12 s, then 20 s more. Half a minute in total before a dead link is declared dead: long enough that a
    // cold remote link is not killed, and a one-line change if that proves too patient. Named here rather
    // than in Settings on purpose — nobody should have to reason about demuxer timeouts to play a book.
    constexpr int kFirstDeadlineMs  = 12000;
    constexpr int kSecondDeadlineMs = 20000;

    enum class Phase { First, Second };

    inline int deadlineMs(Phase p) { return p == Phase::First ? kFirstDeadlineMs : kSecondDeadlineMs; }

    // How long the listener has been looking at nothing when a deadline fires in this phase — the number
    // the message says. Cumulative, because the second deadline is measured from the first, not from zero.
    inline int waitedSeconds(Phase p)
    {
        return (p == Phase::First ? kFirstDeadlineMs : kFirstDeadlineMs + kSecondDeadlineMs) / 1000;
    }

    // ---- Which loads are watched --------------------------------------------------------------------------
    // Everything except a live/HLS link. Local paths are watched too: they never stall, so it costs nothing,
    // and a rule with no special case for them is a rule with one fewer way to be wrong.
    inline bool watches(std::string_view url)
    {
        // The extension is judged on the PATH, not the whole url: a query string or fragment can follow it
        // (a signed .m3u8 nearly always carries a token), and a path segment named "m3u8" is not an extension.
        const auto cut = url.find_first_of("?#");
        std::string_view path = cut == std::string_view::npos ? url : url.substr(0, cut);
        auto endsWithNoCase = [&path](std::string_view suffix) {
            if (path.size() < suffix.size()) return false;
            const std::string_view tail = path.substr(path.size() - suffix.size());
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                char c = tail[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != suffix[i]) return false;
            }
            return true;
        };
        return !(endsWithNoCase(".m3u8") || endsWithNoCase(".m3u"));
    }

    // ---- What mpv can say about progress ------------------------------------------------------------------
    // Three values, not a bool, for the reason given at the top: the host cannot tell "no bytes" from "not
    // enough bytes to have a demuxer yet", and only the former is evidence of anything.
    enum class Progress
    {
        Unknown,   // no demuxer-level property answered: the format is not identified yet (or mpv is idle)
        None,      // a demuxer answered, and reports nothing cached or read
        Some       // a demuxer answered with a positive cache time or stream position
    };

    // ---- What a deadline means ----------------------------------------------------------------------------
    struct Tick
    {
        Phase    phase      = Phase::First;
        bool     fileLoaded = false;             // FILE_LOADED has been seen for this file (the timer should
                                                 // already be off; asked anyway — a queued timeout can land late)
        Progress progress   = Progress::Unknown; // what mpv said when asked at the deadline
    };

    enum class Verdict
    {
        Loaded,    // the file loaded after all — nothing to do
        Regrace,   // slow, or not yet knowable: arm Phase::Second, once
        Stall      // declare it: emit the failure
    };

    inline Verdict judge(const Tick& t)
    {
        if (t.fileLoaded) return Verdict::Loaded;
        if (t.phase == Phase::First && t.progress != Progress::None) return Verdict::Regrace;
        return Verdict::Stall;   // a demuxer that positively reports nothing, or the second deadline regardless
    }
}
