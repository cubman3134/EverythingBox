// Refresh-rate matching, Tier 1 (issue #70): the pure map from the "Reduce judder" Setting to mpv's
// `video-sync` option the built-in player sets at player creation and again, live, when the toggle changes.
// Header-only + QtCore-only, so MpvWidget and probe_refreshsync share ONE mapping with no string table
// drifting between the code and its test (the same arrangement HwDecode.h and SubtitleStyle.h use).
//
// WHAT this does and why it is safe to set globally:
//   * `video-sync=display-resync` locks the video clock to the display's refresh and RESAMPLES audio to match,
//     so 24fps film on a 60Hz panel stops repeating every 5th frame (the 3:2-pulldown judder the issue names)
//     without touching the display mode. That is Tier 1; actual display-mode switching is Tier 2, deferred.
//   * The option only affects timing when there IS a video track. For audio-only playback mpv keeps the audio
//     clock regardless, so `display-resync` is inert there — "audio-only left untouched" falls straight out of
//     mpv's own semantics, no per-stream gating needed. This is why it is set once, globally, like hwdec.
//
// The two carve-outs the issue calls for:
//   * iOS renders every frame through a software QImage blit (no GL/D3D surface driven by the display clock),
//     so the display-resync has nothing to lock to — forced to mpv's default "audio" regardless of the toggle.
//   * The default is form-factor dependent: ON for desktop/TV (where smooth cinematic pans are the point), OFF
//     for a mobile handheld. That is a DEFAULT decision (defaultEnabled), separate from the option map: a user
//     who explicitly turns it on stays on wherever the render path supports it (only iOS is forced off), so the
//     form factor does NOT belong inside videoSyncFor. Splitting them keeps every parameter load-bearing.
//
// OFF emits mpv's own default ("audio"), not nothing, so a live re-apply after the user turns the toggle off
// actively clears `video-sync` back to default rather than leaving `display-resync` set on the context — the
// same "emit the default so re-apply resets" discipline SubtitleStyle.h uses.
#pragma once
#include <QString>
#include <QtGlobal>

namespace RefreshSync
{
    // The build's target platform for the mapping. Passed explicitly (not read from macros) so the map stays a
    // pure function the probe can exercise for all three, regardless of the OS it is compiled on.
    enum class Platform { Desktop, Android, IOS };

    // The resolved UI form factor. Mirrors theme2/FormFactor::Mode but kept independent here so this header
    // stays QtCore-only (no theme2 dependency); Settings translates FormFactor::Mode -> this at the call site.
    enum class FormFactor { Desktop, Tv, Mobile };

    // Pure: the shipped default for the toggle when the user has never set it. ON for desktop and TV (judder on
    // a big-screen pan is exactly what this fixes), OFF for a mobile handheld.
    inline bool defaultEnabled(FormFactor formFactor)
    {
        return formFactor == FormFactor::Desktop || formFactor == FormFactor::Tv;
    }

    // Pure: (toggle enabled, platform) -> the mpv `video-sync` option value.
    //   iOS software-render path: always "audio" — the QImage blit has no display clock to resync to.
    //   otherwise: enabled -> "display-resync" (lock video to display, resample audio); off -> "audio" (mpv's
    //     own default, emitted so a re-apply after turning the toggle off resets rather than leaving resync set).
    inline QString videoSyncFor(bool enabled, Platform platform)
    {
        if (platform == Platform::IOS)
            return QStringLiteral("audio");                  // software-render path: no display clock to lock to
        return enabled ? QStringLiteral("display-resync")
                       : QStringLiteral("audio");            // off => mpv default, so re-apply clears back
    }

    // The platform THIS build targets, from the compile-time OS macros. Used by MpvWidget at player creation;
    // the probe calls videoSyncFor() with explicit platforms instead so it can cover all three.
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
