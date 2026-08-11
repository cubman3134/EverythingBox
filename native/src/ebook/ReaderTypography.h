// Reader typography (issue #135): the pure, deterministic map from the user's ebook-reading preferences —
// font family, size, line spacing, page margin, justification and a reading THEME — to the concrete values
// EbookView applies to its QTextDocument and page chrome. Header-only + QtCore-only (colours are #RRGGBB
// strings, the theme is a plain enum), so EbookView and probe_readertypography share ONE mapping and no clamp
// or palette can drift between the code and its test. This is the ebook twin of #71's SubtitleStyle.h: the
// renderer already flows chapters through a QTextDocument, which takes fonts, palettes and geometry happily —
// what was missing is purely this settings surface and the mapping that feeds it.
//
// WHY the values are shaped this way:
//   * Every field is CLAMPED at the boundary (clampSize/clampSpacing/clampMargin) so a hand-edited ini or a
//     stale synced value can never drive a nonsensical layout. The clamps live here, not in the caller, so the
//     probe pins them exactly.
//   * fontFamily EMPTY means "keep the reader's own default family" — the caller leaves QTextDocument's font
//     family untouched — mirroring how SubtitleStyle's empty family falls back to mpv's default. It is NOT a
//     placeholder string here; the caller decides what the default family is.
//   * The four reading THEMES are text/background #RRGGBB pairs applied to the document text AND the page
//     chrome TOGETHER, so a page never shows dark text on the theme's dark paper for one repaint. TrueBlack is
//     a pure #000000 background for OLED panels (the pixels are actually off), distinct from Dark's dark grey.
//   * resolve() folds a Settings into a Resolved that carries every concrete value the caller reads in one
//     place, so there is a single, testable statement of "what this preference set renders as".
#pragma once
#include <QString>
#include <QtGlobal>

namespace ReaderTypography
{
    // The four reading themes, each a text-on-background palette. Light is the paper default; Sepia is the warm
    // low-contrast reading look; Dark is light ink on dark grey; TrueBlack is light ink on pure black for OLED.
    enum class Theme { Light, Sepia, Dark, TrueBlack };

    // A text/background colour pair, as #RRGGBB strings (QtCore-only; the caller turns them into QColor).
    struct Palette
    {
        QString text;
        QString background;
    };

    // One coherent set of reading-typography preferences. Defaults are the reader's shipping look: no family
    // override (the document's own default family), 14pt (EbookView's existing default), single line spacing,
    // a 6%-of-width page margin, ragged-right (unjustified) text, and the Light paper theme.
    struct Settings
    {
        QString fontFamily;            // "" => keep the reader's current default family (no override)
        int     sizePt         = 14;   // reading font size in points (clamped 8..40)
        int     lineSpacingPct = 100;  // line height as a % of the font's natural leading (clamped 100..250)
        int     marginPct      = 6;    // left/right page margin as a % of the page width (clamped 0..25)
        bool    justify        = false;// justify paragraphs (both edges) vs. ragged-right
        Theme   theme          = Theme::Light;
    };

    // Clamps, kept here so Settings' setters and the caller agree on the exact bounds a stored value is held to.
    inline int clampSize(int pt)      { return qBound(8,   pt,  40);  }
    inline int clampSpacing(int pct)  { return qBound(100, pct, 250); }
    inline int clampMargin(int pct)   { return qBound(0,   pct, 25);  }

    // The palette for a theme. Hand-authored, fixed pairs — the probe pins each one independently.
    //   Light     : near-black ink on white paper.
    //   Sepia     : warm brown ink on aged-cream paper.
    //   Dark      : light-grey ink on dark-grey paper (not pure black — softer for a lit room).
    //   TrueBlack : light-grey ink on pure #000000 (OLED: the background pixels are genuinely off).
    inline Palette themePalette(Theme t)
    {
        switch (t)
        {
        case Theme::Sepia:     return { QStringLiteral("#5B4636"), QStringLiteral("#F4ECD8") };
        case Theme::Dark:      return { QStringLiteral("#C8C8C8"), QStringLiteral("#202124") };
        case Theme::TrueBlack: return { QStringLiteral("#C8C8C8"), QStringLiteral("#000000") };
        case Theme::Light:
        default:               return { QStringLiteral("#1A1A1A"), QStringLiteral("#FFFFFF") };
        }
    }

    // Every concrete value EbookView applies, resolved from a Settings in one place: the clamped numbers, the
    // justify flag verbatim, and the theme's two colours split out for direct use as the document's text colour
    // and the page's background.
    struct Resolved
    {
        QString fontFamily;     // "" => leave the document's default family alone
        int     sizePt;         // clamped
        int     lineSpacingPct; // clamped
        int     marginPct;      // clamped
        bool    justify;
        QString textColor;      // #RRGGBB — document text + page ink
        QString background;     // #RRGGBB — page paper + widget chrome
    };

    inline Resolved resolve(const Settings& s)
    {
        const Palette pal = themePalette(s.theme);
        Resolved r;
        r.fontFamily     = s.fontFamily.trimmed();
        r.sizePt         = clampSize(s.sizePt);
        r.lineSpacingPct = clampSpacing(s.lineSpacingPct);
        r.marginPct      = clampMargin(s.marginPct);
        r.justify        = s.justify;
        r.textColor      = pal.text;
        r.background     = pal.background;
        return r;
    }

    // The stored theme key spelling, kept here so Settings and any caller serialise the enum the same way (an
    // int 0..3). Out-of-range stored ints fall back to Light, so a corrupt/hand-edited value never selects a
    // theme that does not exist.
    inline int themeToInt(Theme t) { return int(t); }
    inline Theme themeFromInt(int v)
    {
        switch (v)
        {
        case 1:  return Theme::Sepia;
        case 2:  return Theme::Dark;
        case 3:  return Theme::TrueBlack;
        default: return Theme::Light;
        }
    }
}
