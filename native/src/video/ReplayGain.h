// ReplayGain (issue #141): the pure, deterministic map from "what the user asked for + what this file actually
// is + what it is actually tagged with" to the set of mpv option (name, value) pairs the built-in player
// applies. Header-only + QtCore-only, so MpvWidget and probe_replaygain share ONE mapping and no string table
// can drift between the code and its test — the same arrangement AudioOutput.h, RefreshSync.h and HdrOutput.h
// use for the device/passthrough, video-sync and HDR options.
//
// WHY THERE IS NO SIGNAL WORK HERE. mpv implements ReplayGain itself: it reads the four REPLAYGAIN_* tags out
// of the container it already demuxed and scales the audio output gain. So this whole feature is option
// plumbing — `replaygain`, `replaygain-preamp`, `replaygain-clip`, `replaygain-fallback` — and the only thing
// with judgement in it is WHICH options to ask for. That decision is what lives here, and it is the part
// probe_replaygain pins.
//
// ALBUM IS THE DEFAULT, AND THE REASON IS NOT "it sounds louder". Album mode applies ONE gain to every track of
// a record, so the quiet interlude stays quieter than the single that follows it — the loudness relationships
// the mastering engineer put there survive. Track mode normalises each track to the same perceived loudness,
// which flattens exactly those relationships; that is wrong inside an album and RIGHT across a shuffled queue,
// where the tracks have no relationship to preserve and a 1970s master next to a 2010s one is the whole
// complaint. So: Album is the default because the album is the unit a listener who cares chose to play, and
// Track is offered because shuffle is the case where per-track is the honest answer.
//
// MUSIC ONLY. An audiobook or a podcast is not normalised: those are single-voice recordings whose level was
// chosen by whoever produced them, they are almost never ReplayGain-tagged, and a listener who has set a book
// to a comfortable volume does not want the next chapter to arrive at a different one. The carve-out is the
// same one issue #140's per-item speed already makes, and it is applied HERE (isMusic == false forces Off)
// rather than at the call site, so there is exactly one place that can be wrong about it.
//
// UNTAGGED PLAYS UNMODIFIED, AND THAT IS ENFORCED TWICE. There is no analysis pass and nothing is ever written
// back to the user's files (#141 says so explicitly): a file with no ReplayGain tags is simply played as it
// was mastered. Belt: a file with neither gain tag resolves to Off, so the option we ask mpv for says nothing
// about it. Braces: `replaygain-fallback` is emitted as 0 (disabled) every single time — that is mpv's OWN
// "apply this gain to untagged files" knob, and leaving it unset would let a stale value from anywhere gain a
// file we deliberately decided not to touch.
//
// PRESENCE IS PRESENCE, NEVER `gain != 0`. AudioTags::GainValue carries a `present` flag precisely because
// 0.00 dB is a real, common tag value — a track that genuinely needs no adjustment. Deciding tag presence by
// testing the number against zero would classify every already-normalised track as untagged, which is the one
// mistake the #74 reader was shaped to make impossible. Every test below reads `.present` and nothing else.
#pragma once
#include "../media/AudioTags.h"   // GainValue — the presence-carrying value type the #74 reader produces

#include <QString>
#include <QVector>
#include <QPair>

namespace ReplayGain
{
    // What the user picked. Off is a real choice (a user with a carefully levelled library wants nothing
    // touched); Album is the shipped default; Track is for shuffled listening.
    enum class Mode { Off, Track, Album };

    // The shipped default. Named here rather than spelled `Mode::Album` at each of Settings, the two settings
    // builders and the probe, so "the default is Album" is one fact in one place.
    inline Mode defaultMode() { return Mode::Album; }

    // Stored id <-> Mode. The id is what goes in the ini ("playback/replayGain") and what both settings
    // builders map their displayed option back through; an unrecognised or absent id resolves to the default,
    // so a hand-edited ini or a value from a future build degrades to the shipped behaviour instead of Off.
    inline QString idForMode(Mode m)
    {
        switch (m)
        {
            case Mode::Off:   return QStringLiteral("off");
            case Mode::Track: return QStringLiteral("track");
            case Mode::Album: return QStringLiteral("album");
        }
        return QStringLiteral("album");
    }
    inline Mode modeFromId(const QString& id)
    {
        const QString s = id.trimmed().toLower();
        if (s == QLatin1String("off"))   return Mode::Off;
        if (s == QLatin1String("track")) return Mode::Track;
        if (s == QLatin1String("album")) return Mode::Album;
        return defaultMode();
    }

    // The preamp band offered to the user, in dB. mpv accepts -150..150, which is not a range anybody wants a
    // remote control pointed at: ±15 dB covers the two real reasons to touch it (ReplayGain's 89 dB reference
    // leaves most material quieter than an untagged file, so a few dB up matches the rest of the system; and a
    // few dB down buys headroom on a system that is already hot) and cannot silence or destroy the output.
    inline double minPreampDb() { return -15.0; }
    inline double maxPreampDb() { return  15.0; }
    inline double defaultPreampDb() { return 0.0; } // mpv's own default: apply the tagged gain and nothing more
    inline double clampPreamp(double db)
    {
        if (!(db == db)) return defaultPreampDb();  // NaN out of a corrupt ini is not a quiet 0, it is garbage
        return db < minPreampDb() ? minPreampDb() : (db > maxPreampDb() ? maxPreampDb() : db);
    }

    // The decision. Given what the user asked for, whether this item is music, and which gain tags the file
    // actually carries, what does mpv get asked to do?
    //
    //   * not music (audiobook / podcast / video)      -> Off. The carve-out, applied before anything else.
    //   * the user picked Off                          -> Off.
    //   * neither gain tag present                     -> Off. Nothing to apply; the file plays as mastered.
    //   * Album asked for, only a track gain tagged    -> Track. A single ripped without album tags is still
    //                                                    worth levelling, and the alternative is silently doing
    //                                                    nothing on exactly the files most likely to be odd.
    //   * Track asked for, only an album gain tagged   -> Album. The mirror image, for the same reason.
    //
    // The two fallbacks mirror what mpv's own tag decoder does internally (it copies one pair into the other
    // when only one is present), so this does not fight mpv — it makes the outcome a property of OUR code that
    // a probe can pin and a log line can state, instead of a behaviour we happen to inherit.
    inline Mode effectiveMode(Mode setting, bool isMusic,
                              const AudioTags::GainValue& trackGain,
                              const AudioTags::GainValue& albumGain)
    {
        if (!isMusic)             return Mode::Off;
        if (setting == Mode::Off) return Mode::Off;
        // Presence, never value: 0.00 dB is a tagged, already-normalised track (see the header comment).
        if (!trackGain.present && !albumGain.present) return Mode::Off;
        if (setting == Mode::Album && !albumGain.present) return Mode::Track;
        if (setting == Mode::Track && !trackGain.present) return Mode::Album;
        return setting;
    }

    // The mpv value for `replaygain` itself. "no" is mpv's default and its off state.
    inline QString mpvModeValue(Mode effective)
    {
        switch (effective)
        {
            case Mode::Off:   return QStringLiteral("no");
            case Mode::Track: return QStringLiteral("track");
            case Mode::Album: return QStringLiteral("album");
        }
        return QStringLiteral("no");
    }

    // Pure: the whole decision -> the ordered list of mpv (option name, value) pairs MpvWidget sets via
    // mpv_set_option_string. Deterministic; no I/O; no dependence on an mpv instance.
    //
    // ALL FOUR ARE EMITTED EVERY TIME, including when the answer is Off. The mpv handle outlives a file, so a
    // value set for the last track is still set for this one: an album-gained record followed by an audiobook
    // would keep gaining the audiobook if the off path merely "did not set anything". Every apply is therefore
    // a full reset to mpv's documented defaults plus whatever this item actually warrants — the same
    // unconditional-write discipline AudioOutput::toMpvOptions and SubtitleStyle::toMpvOptions use, and for the
    // same reason.
    inline QVector<QPair<QString, QString>> toMpvOptions(Mode setting, bool isMusic, double preampDb,
                                                         const AudioTags::GainValue& trackGain,
                                                         const AudioTags::GainValue& albumGain)
    {
        const Mode eff = effectiveMode(setting, isMusic, trackGain, albumGain);
        QVector<QPair<QString, QString>> out;
        out.reserve(4);
        out << qMakePair(QStringLiteral("replaygain"), mpvModeValue(eff));
        // The preamp rides ONLY on a real gain: with the answer Off it resets to 0, because a preamp on an
        // un-normalised file is not ReplayGain, it is a hidden volume change on material we just decided not to
        // touch. Clamped so a hand-edited ini cannot ask mpv for +150 dB.
        out << qMakePair(QStringLiteral("replaygain-preamp"),
                         QString::number(eff == Mode::Off ? defaultPreampDb() : clampPreamp(preampDb), 'g', 6));
        // Clipping prevention stays ON, always, and is deliberately not a setting. Boosting a track whose peak
        // is already near full scale is how ReplayGain turns into distortion, and "let me clip" is not a
        // preference anyone benefits from having offered. mpv's own default is also yes, so emitting it in the
        // Off case is a true reset rather than an opinion.
        out << qMakePair(QStringLiteral("replaygain-clip"), QStringLiteral("yes"));
        // The untagged guarantee: mpv's "gain to apply when a file has NO ReplayGain tags", pinned to 0
        // (disabled) unconditionally. #141 is explicit that untagged material plays unmodified — no analysis,
        // no invented gain — and this is the option that would otherwise invent one.
        out << qMakePair(QStringLiteral("replaygain-fallback"), QStringLiteral("0"));
        return out;
    }
}
