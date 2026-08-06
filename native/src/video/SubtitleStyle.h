// Subtitle styling (issue #71): the pure, deterministic map from the user's subtitle-appearance preferences
// to the set of mpv option (name, value) pairs the built-in player applies — at player creation and again,
// live, whenever one of the settings changes. Header-only + QtCore-only, so MpvWidget and probe_substyle share
// ONE mapping and no string table can drift between the code and its test (the same arrangement HwDecode.h
// uses for the hwdec choice).
//
// WHY these particular options, and the one subtlety that matters:
//   * font/size/colour/outline/box/position/bold all target mpv's UN-STYLED subtitle renderer, which is what
//     draws SRT and other plain-text formats — the overwhelming case for an OpenSubtitles fetch.
//   * ASS/SSA subtitles carry their own typography by design. mpv gates whether our styling reaches them
//     through `sub-ass-override`: at its default ("scale", which we emit when the toggle is off) mpv applies
//     only the size scale to ASS and leaves the sub's own fonts/colours/positions ALONE, so styling a plain
//     SRT never mangles an anime fansub. Only `sub-ass-override=force` — emitted solely when the user turns
//     on "Override styled subtitles" — pushes the full plain style onto ASS too. Getting this backwards would
//     render every styled sub in the user's SRT look, a regression, which is why the default here emits no
//     `force` and probe_substyle pins exactly that.
//   * size maps to `sub-scale`, the SAME option the in-player size stepper already drives — NOT a second
//     `sub-font-size` knob. There is one notion of subtitle size.
//
// Every field is emitted UNCONDITIONALLY (font falls back to mpv's own default family, the box to a fully
// transparent colour, the ASS gate to "scale") so that a live re-apply after a change actively clears the
// previous value — turning the box off, or clearing a font, resets rather than leaving a stale option set on
// the mpv instance. The one place absence is meaningful is `sub-ass-override=force`, which the "off" path
// never emits.
#pragma once
#include <QString>
#include <QVector>
#include <QPair>
#include <QtGlobal>

namespace SubtitleStyle
{
    // One coherent set of subtitle-appearance preferences. Defaults mirror mpv's own where there is one
    // (scale 1.0, border 3px, sans-serif, bottom, no box) so a factory-fresh install renders exactly as mpv
    // would with no options set — the styling only diverges once the user changes something.
    struct Style
    {
        QString fontFamily;                 // "" => mpv's default family (emitted as "sans-serif")
        int     sizePercent      = 100;     // -> sub-scale (100% = 1.0). The existing size notion, not a new one.
        QString textColor        = QStringLiteral("#FFFFFF");   // -> sub-color   (#RRGGBB)
        int     borderSize       = 3;       // -> sub-border-size (outline thickness; mpv default 3)
        QString borderColor      = QStringLiteral("#000000");   // -> sub-border-color (#RRGGBB)
        bool    boxEnabled       = false;   // background box behind the text
        int     boxOpacityPercent = 75;     // box alpha when enabled (0..100); ignored when boxEnabled is false
        int     position         = 100;     // -> sub-pos (0 = top … 100 = bottom; mpv accepts up to 150)
        bool    bold             = false;   // -> sub-bold
        bool    overrideStyled   = false;   // -> sub-ass-override: "force" only when true (default leaves ASS alone)
    };

    // The stored default for the ASS-override gate, kept here so Settings and the toMpvOptions() gate agree on
    // spelling: the toggle is OFF by default, so styled subs are left alone.
    inline QString assOverrideValue(bool overrideStyled)
    {
        return overrideStyled ? QStringLiteral("force") : QStringLiteral("scale");
    }

    // Encode the background box as an mpv `<color>`: #AARRGGBB, black, with the alpha carrying the box.
    // OFF => #00000000 (fully transparent — no box, which is also mpv's own sub-back-color default). ON =>
    // the opacity percentage scaled onto the 0..255 alpha channel (mpv: 00 transparent … FF opaque), over a
    // black fill. So "box off" is guaranteed to draw nothing regardless of the stored opacity.
    inline QString backColor(bool boxEnabled, int opacityPercent)
    {
        const int pct   = boxEnabled ? qBound(0, opacityPercent, 100) : 0;
        const int alpha = qRound(pct / 100.0 * 255.0);
        // Build the two-digit alpha separately: "#%1000000".arg(...) would parse "%10" as a placeholder and
        // swallow a zero, yielding a 7-digit colour. Concatenate instead so RGB is exactly "000000".
        const QString aa = QStringLiteral("%1").arg(alpha, 2, 16, QLatin1Char('0')).toUpper();
        return QStringLiteral("#") + aa + QStringLiteral("000000");
    }

    // Pure: a Style -> the ordered list of mpv (option name, value) pairs MpvWidget sets via
    // mpv_set_option_string. Deterministic; no I/O; no dependence on an mpv instance. Values are formatted in
    // the C locale (QString::number does not localise), which is what mpv's parser expects.
    inline QVector<QPair<QString, QString>> toMpvOptions(const Style& s)
    {
        const double scale = qBound(10, s.sizePercent, 1000) / 100.0;
        QVector<QPair<QString, QString>> out;
        out.reserve(9);
        out << qMakePair(QStringLiteral("sub-font"),
                         s.fontFamily.trimmed().isEmpty() ? QStringLiteral("sans-serif") : s.fontFamily.trimmed());
        out << qMakePair(QStringLiteral("sub-scale"),        QString::number(scale));
        out << qMakePair(QStringLiteral("sub-color"),        s.textColor);
        out << qMakePair(QStringLiteral("sub-border-size"),  QString::number(qBound(0, s.borderSize, 20)));
        out << qMakePair(QStringLiteral("sub-border-color"), s.borderColor);
        out << qMakePair(QStringLiteral("sub-back-color"),   backColor(s.boxEnabled, s.boxOpacityPercent));
        out << qMakePair(QStringLiteral("sub-pos"),          QString::number(qBound(0, s.position, 150)));
        out << qMakePair(QStringLiteral("sub-bold"),         s.bold ? QStringLiteral("yes") : QStringLiteral("no"));
        // The ASS gate LAST. "force" only when the user asked for it; otherwise "scale" — mpv's own default,
        // which leaves a styled sub's fonts and colours untouched. Never emits "force" on the default path.
        out << qMakePair(QStringLiteral("sub-ass-override"), assOverrideValue(s.overrideStyled));
        return out;
    }
}
