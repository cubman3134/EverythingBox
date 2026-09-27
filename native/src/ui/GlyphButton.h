// A narrow, fixed-width button whose whole caption is one glyph — a remove ✕, an edit ✎, a pane's pause.
//
// Issue #458. The app-wide sheet (AppStyleSheet::base) gives EVERY QPushButton 16px of horizontal padding a
// side. That is right for a worded button and fatal for a narrow one: at 28px the contents rect is negative,
// at 36px it is about zero, at 40px it is narrower than the glyph — and the style clips the caption to it, so
// the button draws an empty face. The classic registry browser's remove button looked like a font with no
// U+2715 in it; the font was fine (the same glyph draws on every wider button), there was simply no room.
//
// So a glyph button keeps the width its row was designed around and gives up only the side padding, in its
// OWN sheet: a rule there outranks the app's sheet and any ancestor's (a hosting panel may set its own
// QPushButton padding), and it touches nothing else — the background, border and focus highlight still come
// from wherever they came from before. Vertical padding is left alone so the button keeps its row height.
//
// probe_regbrowser pins this at the narrowest width in use, under the real app sheet, with a control that
// shows the same button without it drawing nothing.
#pragma once
#include <QPushButton>
#include <QString>

namespace GlyphButton
{
inline void apply(QPushButton* b, int width)
{
    b->setFixedWidth(width);
    b->setStyleSheet(QStringLiteral("QPushButton{padding-left:0px;padding-right:0px;}"));
}
} // namespace GlyphButton
