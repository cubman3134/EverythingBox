// Hardware-decode policy (issue #67): the pure map from the user's "Hardware decoding" Setting choice
// (Off / Auto / On) to the mpv `hwdec` option string set once at player creation, honouring the two
// platform carve-outs. Header-only + QtCore-only so both MpvWidget and probe_hwdecode share ONE mapping
// with no duplicated string table drifting between them.
//
// WHY Auto is the default and maps to "auto-safe", not full hardware: MpvWidget used to hard-code
// `hwdec=no` because D3D11VA corrupted 10-bit HEVC (p010) on the dev machine even in copy mode. "auto-safe"
// only enables decode APIs mpv marks safe — it prefers copy-back and falls back to software on an
// unsupported profile — so it sidesteps that corruption class rather than re-opening it the way a blanket
// "on"/"auto" would. Off preserves the old guaranteed-correct behaviour for a known-bad driver.
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

    // Pure: (stored choice, platform) -> the mpv `hwdec` option string.
    //   choice: "off" | "auto" | "on"; any unknown/empty value is treated as the "auto" default.
    //   iOS renders every frame through a software QImage blit (no GL/D3D surface for a decoder to hand back),
    //     so hardware decode CANNOT be used there — forced to "no" regardless of the choice.
    //   Android reaches mediacodec through the same Auto path: "auto-safe" resolves to mediacodec-copy, the
    //     copy-back mode libmpv's render API needs. So Android is NOT forced to software the way iOS is.
    inline QString mpvOption(const QString& choice, Platform platform)
    {
        if (platform == Platform::IOS)
            return QStringLiteral("no");                 // software-render path: hardware decode unavailable
        if (choice == QStringLiteral("off"))
            return QStringLiteral("no");                 // preserve the old guaranteed-correct software decode
        if (choice == QStringLiteral("on"))
            return QStringLiteral("auto");               // full hardware, direct paths included
        return QStringLiteral("auto-safe");              // "auto" (default) + any unknown value: safe + fallback
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
