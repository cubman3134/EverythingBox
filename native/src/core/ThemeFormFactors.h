// The theme manifest's `formFactors` declaration, and the one rule that turns it into a verdict for THIS
// device (issue #32). A theme.json may carry:
//
//     "formFactors": ["desktop", "tv", "mobile", "handheld"]
//
// ...which is the AUTHOR'S CLAIM about where their layout works. Nothing measures it, nothing verifies it,
// and a wrong claim is indistinguishable from a right one — so this unit never gates anything. It answers a
// question the picker asks in order to LABEL a row; it does not decide what may be rendered. See fit().
//
// The decision is PURE — it takes the raw manifest value and the current mode name as arguments rather than
// reading theme.json or FormFactor — so probe_theme pins it with no I/O and no Qt GUI. QtCore only (the
// header takes a QJsonValue and a QString and nothing else), so the probe stays lean. ThemeEngine owns the
// I/O half (themeFormFactorFit), because it already owns every other theme.json read.
#pragma once
#include <QString>

class QJsonValue;

namespace ThemeFormFactors
{
    // The manifest key. Named here so the reader, the writer of THEME_FORMAT.md and the probe cannot drift.
    inline constexpr const char* kKey = "formFactors";

    // THREE states, not two. "Undeclared" is deliberately distinct from both "supports everything" and
    // "supports nothing", because every community theme in the registry predates this key:
    //   * reading absent as "supports everything" invents a claim the author never made, and hides exactly
    //     the risk this feature exists to show;
    //   * reading absent as "supports nothing" flags every existing theme as broken, which is a false alarm
    //     (most of them do work) and trains the user to ignore the flag.
    // "We do not know" is the only honest reading, and it is a different thing to say in the UI than "the
    // author says no".
    enum class Fit
    {
        Undeclared,   // the manifest carries no well-formed `formFactors` array
        Supported,    // the declaration names this device's mode
        Unsupported   // the declaration is present and does NOT name this device's mode
    };

    // `declared` is the manifest's raw `formFactors` value (QJsonValue::Undefined when the key is absent);
    // `currentMode` is FormFactor::modeName() — "desktop" | "tv" | "mobile" — for the device deciding RIGHT
    // NOW. Never a stored mode: `display/mode` is device-local and `auto` re-resolves per device, so the same
    // synced profile legitimately has a theme that suits the phone and not the TV. The judge must be the
    // device in front of the user.
    //
    // The rules, all of them:
    //   * a value that is not a JSON ARRAY (absent, null, a bare string, an object, a number) -> Undeclared.
    //     STRICT on purpose. Guessing at "formFactors": "desktop" would mean inferring a claim, and the cost
    //     of inferring wrong is the silent lie the feature exists to prevent; the cost of strictness is a
    //     visible "support not declared" note, which is the author's error message.
    //   * entries are compared case- and whitespace-insensitively, so "Desktop" and " TV " match.
    //     Non-string and empty entries are ignored.
    //   * an EMPTY array is a real declaration ("fits nothing"), so it yields Unsupported everywhere. That is
    //     the one case where declaring the key is worse than omitting it, and it is what the author asked for.
    //   * the match is a plain string comparison against `currentMode`, with NO table of "modes we know".
    //     That is what makes "handheld" advisory today and correct tomorrow: FormFactor resolves only
    //     Desktop/Tv/Mobile, so a theme declaring only ["handheld"] does not name any device that exists yet
    //     and reads Unsupported — the honest answer, since the author never claimed this device. If a real
    //     FormFactor::Mode::Handheld ever lands, every theme already declaring "handheld" starts matching it
    //     with no change here. See THEME_FORMAT.md for the author-facing version of this.
    Fit fit(const QJsonValue& declared, const QString& currentMode);

    // The short user-facing note for a verdict, or an EMPTY string for Supported (a fitting theme is not
    // decorated at all — a badge on every row is a badge on none). One wording for all three surfaces that
    // show it; see the .cpp for why it lives here and not at each surface.
    QString shortNote(Fit f);
}
