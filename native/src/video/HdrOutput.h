// HDR output handling (issue #68): the pure map from the user's "HDR video" Setting to the small set of mpv
// option (name, value) pairs the built-in player applies — at player creation and again, live, when the setting
// changes. Header-only + QtCore-only, so MpvWidget and probe_hdroutput share ONE mapping with no string table
// drifting between the code and its test (the same arrangement HwDecode.h, RefreshSync.h, SubtitleStyle.h and
// AudioOutput.h use). Before this, HDR content got only a diagnostic log (logVideoInfo's transfer/primaries),
// so on an SDR panel it showed washed-out grey and on an HDR panel we never told the compositor we had HDR.
//
// The two-way switch the issue asks for — "Start minimal", no curve picker, no per-knob sprawl:
//   * TONE-MAP TO SDR (default): the common failure. `tone-mapping=bt.2446a` maps HDR down to the SDR gamut so
//     it stops looking washed-out on an SDR display, and `hdr-compute-peak=yes` measures the source's actual
//     peak brightness per-scene so the roll-off is right rather than clipping. Pure option plumbing; safe on
//     SDR content too (tone-mapping is inert when the source has no HDR transfer). This path also emits
//     `target-colorspace-hint=no` — mpv's own default — so the passthrough hint is actively CLEARED (see below).
//   * PASSTHROUGH WHEN SUPPORTED: `target-colorspace-hint=yes` makes mpv signal the source's HDR10 metadata to
//     the swapchain, so on a display that supports HDR (Windows/Android) the compositor shows real HDR instead
//     of a tone-mapped approximation. Where the platform does NOT support it, mpv tone-maps anyway as the
//     fallback — so this is strictly "HDR when we can, tone-map when we can't", never a washed-out result. It
//     keeps `hdr-compute-peak=yes` (needed for that fallback), and emits `tone-mapping=auto` — mpv's own
//     default — so a live flip back and forth resets the SDR curve rather than leaving `bt.2446a` pinned.
//
// The reset discipline (the reason every field is emitted UNCONDITIONALLY, not just the ones a mode "uses"):
// the user can flip this setting live, and MpvWidget re-applies the whole set on change. If a mode emitted only
// its own keys, flipping from Passthrough back to Tone-map would leave `target-colorspace-hint=yes` stuck on the
// context, and vice-versa. So BOTH modes emit all three keys, each writing the OTHER mode's key back to mpv's
// default — the same "emit the default so re-apply resets" discipline RefreshSync.h and SubtitleStyle.h use.
//
// The platform carve-out the issue calls for (pure and explicit, like RefreshSync's iOS force-off): iOS renders
// every frame through a software QImage blit with no HDR swapchain to hand HDR10 metadata to, so passthrough
// CANNOT work there — the stored mode is forced to tone-map regardless. That is a property of the render path,
// so it lives inside the pure map (the probe exercises it for an explicit IOS platform), not at the call site.
//
// Interacts with #67 (hardware decode): copy-back hwdec + tone-mapping is the tested-safe pairing; this map does
// not know or gate on hwdec — it only sets the output options — but the default (Auto) keeps that pairing intact
// without either setting reaching into the other. That sentence used to name "auto-safe" as the copy-back mode
// and was wrong about it for as long as it stood: auto-safe resolved to nvdec DIRECT on NVIDIA, so the pairing
// this comment vouched for was not the one running. Auto is a copy-back LIST now (#229, HwDecode.h).
#pragma once
#include <QString>
#include <QVector>
#include <QPair>
#include <QtGlobal>

namespace HdrOutput
{
    // The build's target platform for the mapping. Passed explicitly (not read from macros) so the map stays a
    // pure function the probe can exercise for all three, regardless of the OS it is compiled on.
    enum class Platform { Desktop, Android, IOS };

    // The two-way switch. ToneMapSdr is the shipped default (the common failure it fixes); PassthroughWhenSupported
    // signals HDR10 to a capable display and tone-maps as the fallback where it is not.
    enum class Mode { ToneMapSdr, PassthroughWhenSupported };

    // The stored id for the default mode, kept here so Settings and the two settings builders agree on spelling
    // ("video/hdr" absent => "tonemap").
    inline QString defaultModeId() { return QStringLiteral("tonemap"); }

    // Mode <-> stored id. idForMode names the string written to the ini; modeFromId maps a stored/absent/unknown
    // value back to a Mode, defaulting to ToneMapSdr so a hand-edited garbage value degrades to the safe default.
    inline QString idForMode(Mode mode)
    {
        return mode == Mode::PassthroughWhenSupported ? QStringLiteral("passthrough") : QStringLiteral("tonemap");
    }
    inline Mode modeFromId(const QString& id)
    {
        return id.trimmed() == QStringLiteral("passthrough") ? Mode::PassthroughWhenSupported : Mode::ToneMapSdr;
    }

    // Pure: (stored mode, platform) -> the ordered list of mpv (option name, value) pairs MpvWidget sets via
    // mpv_set_option_string. Deterministic; no I/O; no dependence on an mpv instance. BOTH modes emit all three
    // keys so a live re-apply after a mode flip resets the other mode's key to mpv's default (see the reset
    // discipline above). iOS is forced to tone-map — its software-render path has no HDR swapchain for passthrough.
    inline QVector<QPair<QString, QString>> optionsFor(Mode mode, Platform platform)
    {
        // iOS software-render path: no HDR swapchain to hand HDR10 metadata to, so passthrough cannot work there.
        // Force tone-map regardless of the stored mode (the same shape as HwDecode/RefreshSync's iOS carve-out).
        const Mode effective = (platform == Platform::IOS) ? Mode::ToneMapSdr : mode;

        QVector<QPair<QString, QString>> out;
        out.reserve(3);
        if (effective == Mode::PassthroughWhenSupported)
        {
            // Signal HDR10 to the swapchain; keep peak measurement for the tone-map fallback where the display
            // does not support HDR; reset the SDR curve to mpv's default so a flip back from Tone-map clears it.
            out << qMakePair(QStringLiteral("target-colorspace-hint"), QStringLiteral("yes"));
            out << qMakePair(QStringLiteral("hdr-compute-peak"),       QStringLiteral("yes"));
            out << qMakePair(QStringLiteral("tone-mapping"),           QStringLiteral("auto"));   // mpv default
        }
        else
        {
            // Tone-map HDR down to SDR so it stops looking washed-out; measure the source's per-scene peak; and
            // reset the passthrough hint to mpv's default "no" so a flip back from Passthrough clears it.
            out << qMakePair(QStringLiteral("tone-mapping"),           QStringLiteral("bt.2446a"));
            out << qMakePair(QStringLiteral("hdr-compute-peak"),       QStringLiteral("yes"));
            out << qMakePair(QStringLiteral("target-colorspace-hint"), QStringLiteral("no"));      // mpv default
        }
        return out;
    }

    // The platform THIS build targets, from the compile-time OS macros. Used by MpvWidget at player creation; the
    // probe calls optionsFor() with explicit platforms instead so it can cover all three.
    inline Platform currentPlatform()
    {
#if defined(Q_OS_IOS)
        return Platform::IOS;
#elif defined(Q_OS_ANDROID)
        return Platform::Android;
#else
        return Platform::Desktop;
#endif
    }
}
