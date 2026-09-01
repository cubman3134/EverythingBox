// Hardware-decode policy (issue #67): the pure map from the user's "Hardware decoding" Setting choice
// (Off / Auto / On) to the mpv `hwdec` option string set once at player creation, honouring the two
// platform carve-outs. Header-only + QtCore-only so both MpvWidget and probe_hwdecode share ONE mapping
// with no duplicated string table drifting between them.
//
// WHY Auto is the default, and why it names decoders instead of taking one of mpv's "auto" modes.
//
// MpvWidget used to hard-code `hwdec=no` because D3D11VA corrupted 10-bit HEVC (p010) on the dev machine even
// in copy mode. Auto replaced that blanket refusal with a mode this comment described as "copy-back preferred,
// software fallback" — and then mapped it to mpv's "auto-safe", which is not that. "auto-safe" is a whitelist
// of decoders mpv considers SAFE; it expresses no preference for copy-back ones. Measured with
// native/tools/hwdecframe on an RTX 5080 (issue #229): "auto-safe" resolves to `nvdec` DIRECT — pixfmt=cuda,
// CUDA<->GL interop straight into our QOpenGLWidget — which is the interop this file believed it was avoiding.
// Settings.h and HdrOutput.h both reason from the copy-back premise too, so all three were describing
// behaviour the code did not have.
//
// The obvious repair, mpv's own "auto-copy-safe", is deliberately NOT what Auto maps to, and that is a
// measurement rather than a preference: on the same machine "auto-copy-safe" resolves to `d3d11va-copy`, not
// `nvdec-copy`. It reaches for the D3D11VA family the paragraph above is a scar from, on a machine whose
// vendor decoder is sitting right there, and it costs more doing it (1080p60: 0.41 core-seconds per second of
// playback against nvdec-copy's 0.29). To be fair to it: the p010 corruption in the paragraph above did NOT
// reproduce on this machine in 2026-09 (driver 592.01) — d3d11va-copy decoded a 10-bit HEVC clip
// bit-identically to software. So the order below is a preference for the vendor decoder and the cheaper of
// the two, not a claim that D3D11VA is broken today; on a machine with no NVIDIA in it, d3d11va-copy is
// exactly what Auto lands on.
//
// So Auto names the copy-back decoders explicitly, in preference order, with "auto-copy-safe" last as the
// catch-all. mpv takes a comma-separated hwdec LIST and skips entries that do not exist on the platform
// (measured: "bogus-copy,nvdec-copy" resolves to nvdec-copy), which is what lets ONE list be right everywhere:
// Windows+NVIDIA takes nvdec-copy, Windows+Intel/AMD falls through to d3d11va-copy, Linux to vaapi-copy,
// Android to mediacodec-copy, macOS to videotoolbox-copy, and any platform none of those cover lands on mpv's
// own safe copy-back pick. Every entry is copy-back, so no path here hands the render context an interop
// texture; a profile no hardware decoder supports still falls back to software, as before.
//
// The cost of copy-back is one frame copy per frame, measured with `hwdecframe --play` on 1080p H.264:
// 1080p24 nvdec 0.117 vs nvdec-copy 0.140 core-seconds per second, zero delayed frames either way; 1080p60
// 0.221 vs 0.289. Software decode is 0.140 and 0.490 for comparison.
//
// What this is NOT: a demonstrated cure for #229's speckle. hwdecframe found nvdec DIRECT bit-identical to
// software decode on every clip it was pointed at (8-bit H.264 1080p, 8-bit HEVC, 10-bit HEVC p010), in a bare
// harness and inside the app. This change is made because the documented contract said copy-back and the code
// did not deliver it, and because it removes the CUDA<->GL interop that the crash in nvoglv64.dll implicates —
// not because the corruption was reproduced and then cured. Off still preserves guaranteed-correct software
// decode; On is still full hardware, interop paths included, for anyone who wants the interop back.
#pragma once
#include <QString>
#include <QtGlobal>

namespace HwDecode
{
    // The build's target platform for the mapping. Passed explicitly (not read from macros) so the mapping
    // stays a pure function the probe can exercise for all three, regardless of the OS it is compiled on.
    enum class Platform { Desktop, Android, IOS };

    // The stored id of the default choice ("video/hwdec" absent => Auto). Kept here so Settings and the two
    // settings builders agree on the spelling.
    inline QString defaultChoice() { return QStringLiteral("auto"); }

    // What Auto asks mpv for: copy-back decoders, in preference order, then mpv's own safe copy-back pick as
    // the catch-all. A comma-separated hwdec LIST, not one method — see the header comment for the
    // measurements behind both the list form and the order. Named rather than inlined so the probe can pin the
    // exact string, and so a reader of Settings.h can be sent somewhere that says what it is.
    inline QString autoCopyBackList()
    {
        return QStringLiteral("nvdec-copy,d3d11va-copy,vaapi-copy,videotoolbox-copy,mediacodec-copy,auto-copy-safe");
    }

    // Pure: (stored choice, platform) -> the mpv `hwdec` option string.
    //   choice: "off" | "auto" | "on"; any unknown/empty value is treated as the "auto" default.
    //   iOS renders every frame through a software QImage blit (no GL/D3D surface for a decoder to hand back),
    //     so hardware decode CANNOT be used there — forced to "no" regardless of the choice.
    //   Android reaches mediacodec through the same Auto path: the list below names mediacodec-copy, the
    //     copy-back mode libmpv's render API needs, and every entry before it is absent on Android and so is
    //     skipped. So Android is NOT forced to software the way iOS is.
    inline QString mpvOption(const QString& choice, Platform platform)
    {
        if (platform == Platform::IOS)
            return QStringLiteral("no");                 // software-render path: hardware decode unavailable
        if (choice == QStringLiteral("off"))
            return QStringLiteral("no");                 // preserve the old guaranteed-correct software decode
        if (choice == QStringLiteral("on"))
            return QStringLiteral("auto");               // full hardware, direct paths included
        return autoCopyBackList();                       // "auto" (default) + any unknown value
    }

    // The platform THIS build targets, from the compile-time OS macros. Used by MpvWidget at player creation;
    // the probe calls mpvOption() with explicit platforms instead so it can cover all three.
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
