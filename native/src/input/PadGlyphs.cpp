#include "PadGlyphs.h"

namespace padglyphs
{

Brand brandFromName(const QString& name)
{
    if (name == QLatin1String("xbox"))        return Brand::Xbox;
    if (name == QLatin1String("playstation")) return Brand::PlayStation;
    if (name == QLatin1String("switch"))      return Brand::Switch;
    return Brand::Generic;
}

QString nameForBrand(Brand b)
{
    switch (b)
    {
    case Brand::Xbox:        return QStringLiteral("xbox");
    case Brand::PlayStation: return QStringLiteral("playstation");
    case Brand::Switch:      return QStringLiteral("switch");
    case Brand::Generic:     break;
    }
    return QStringLiteral("generic");
}

Verb verbForHint(const QString& hintKey)
{
    if (hintKey == QLatin1String("Enter")) return Verb::Confirm;
    if (hintKey == QLatin1String("Esc"))   return Verb::Back;
    if (hintKey == QLatin1String("I"))     return Verb::Details;
    if (hintKey == QLatin1String("/"))     return Verb::Search;
    if (hintKey == QLatin1String("F"))     return Verb::Filter;
    if (hintKey == QLatin1String("P"))     return Verb::Playlist;
    if (hintKey == QLatin1String("T"))     return Verb::Theme;
    if (hintKey == QLatin1String("S"))     return Verb::Skip;
    // Two spellings, ONE verb, because the gesture genuinely has two names. "Menu" is the keyboard key that
    // opens the browse context menu (Qt::Key_Menu — MainWindow::keyPressEvent and sendNavKey both route it to
    // openBrowseContextMenu), and it is what a THEME authors in its help bar, so a chip translated in pointer
    // mode still names a key the user can press. "Start" names the pad button directly and is what app PROSE
    // says (the OSK footer's commit arm), where there is no keyboard equivalent to name. Both resolve to
    // RetroPad START, so both follow a remap.
    if (hintKey == QLatin1String("Menu"))  return Verb::Menu;   // the keyboard key; what a help bar authors
    if (hintKey == QLatin1String("Start")) return Verb::Menu;   // the commit button, not a keyboard key
    return Verb::None;   // arrow chips and third-party text: the caller's own string survives
}

// THE SAME VOCABULARY IS STATED IN ONE OTHER PLACE: the `navs[]` table in MainWindow::pollMenuPad
// (src/ui/MainWindow.cpp), which is what physically DOES each job when the button is pressed. This function
// only decides what the help chip NAMES. Change one without the other and the bar advertises a button that
// does nothing. pollMenuPad carries static_asserts (plus a debug-only Q_ASSERT against these two functions)
// that fail the build if the two disagree, so edit this table and let the compiler point at the other one.
int retroIdForVerb(Verb v)
{
    // RETRO_DEVICE_ID_JOYPAD_*: B=0 (south) Y=1 (west) SELECT=2 START=3 A=8 (east) X=9 (north) L=10 R=11.
    switch (v)
    {
    case Verb::Confirm:  return 0;
    case Verb::Search:   return 1;
    case Verb::Skip:     return 1;   // player surface; never on screen at the same time as Search
    case Verb::Theme:    return 2;
    case Verb::Back:     return 8;
    case Verb::Details:  return 9;
    case Verb::Filter:   return 10;
    case Verb::Playlist: return 11;
    // START, the OSK's commit button. Keep this switch exhaustive and `default`-less: on the CI GCC leg an
    // uncovered enumerator is a -Wswitch warning instead of a silent -1 that renders as un-remapped key
    // text. MEASURED, do not rely on MSVC for it: deleting this case and rebuilding produced NO warning
    // here (its C4062 is off by default at this project's level); only probe_padglyph's literal caught it.
    case Verb::Menu:     return 3;
    case Verb::None:     break;
    }
    return -1;
}

// The label table. Non-ASCII labels are built with QString::fromUtf8 and never QStringLiteral:
// QStringLiteral wraps the bytes in a UTF-16 literal, so what raw \x.. UTF-8 escapes mean there depends
// on the compiler and on whether /utf-8 is in force (it is here, globally, from native/CMakeLists.txt --
// but CI also builds these probes with GCC, where the same escapes decay into separate code units and
// the string becomes mojibake). fromUtf8 decodes the bytes as UTF-8 everywhere, so use it for anything
// outside ASCII.
QString labelForSdlCode(int sdlCode, Brand b)
{
    const bool ps = (b == Brand::PlayStation);
    const bool sw = (b == Brand::Switch);
    switch (sdlCode)
    {
    // Face buttons. Switch spells these exactly like Xbox -- A/B/X/Y -- and that is not a copy-paste slip:
    // SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS defaults to "1" (and Gamepad.cpp does not set it), so SDL
    // reports a Nintendo pad's face buttons BY THE LABEL PRINTED ON THEM, not by position. SDL code 0 is
    // therefore the button marked "A" on a Switch Pro Controller, the same letter as on an Xbox pad --
    // the letters agree, only the physical positions differ. Do not "fix" these back to B/A/Y/X.
    case 0:  return ps ? QString::fromUtf8("\xe2\x9c\x95") : QStringLiteral("A");
    case 1:  return ps ? QString::fromUtf8("\xe2\x97\x8b") : QStringLiteral("B");
    case 2:  return ps ? QString::fromUtf8("\xe2\x96\xa1") : QStringLiteral("X");
    case 3:  return ps ? QString::fromUtf8("\xe2\x96\xb3") : QStringLiteral("Y");
    case 4:  return ps ? QStringLiteral("Create") : sw ? QString::fromUtf8("\xe2\x88\x92") : QStringLiteral("View");
    case 5:  return ps ? QStringLiteral("PS")      : sw ? QStringLiteral("Home") : QStringLiteral("Guide");
    case 6:  return ps ? QStringLiteral("Options") : sw ? QStringLiteral("+")    : QStringLiteral("Menu");
    case 7:  return ps ? QStringLiteral("L3") : QStringLiteral("LS");
    case 8:  return ps ? QStringLiteral("R3") : QStringLiteral("RS");
    case 9:  return ps ? QStringLiteral("L1") : sw ? QStringLiteral("L") : QStringLiteral("LB");
    case 10: return ps ? QStringLiteral("R1") : sw ? QStringLiteral("R") : QStringLiteral("RB");
    case 11: return QString::fromUtf8("\xe2\x86\x91");
    case 12: return QString::fromUtf8("\xe2\x86\x93");
    case 13: return QString::fromUtf8("\xe2\x86\x90");
    case 14: return QString::fromUtf8("\xe2\x86\x92");
    case 1000: return ps ? QStringLiteral("L2") : sw ? QStringLiteral("ZL") : QStringLiteral("LT");
    case 1001: return ps ? QStringLiteral("R2") : sw ? QStringLiteral("ZR") : QStringLiteral("RT");
    default: break;
    }
    return QString();   // Gamepad::kUnbound, and anything SDL never hands us
}

QString chip(const QString& hintKey, Brand b, int sdlCode)
{
    if (verbForHint(hintKey) == Verb::None) return hintKey;
    const QString label = labelForSdlCode(sdlCode, b);
    return label.isEmpty() ? hintKey : label;
}

} // namespace padglyphs
