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
    return Verb::None;   // arrow chips and third-party text: the caller's own string survives
}

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
    case Verb::None:     break;
    }
    return -1;
}

QString labelForSdlCode(int sdlCode, Brand b)
{
    const bool ps = (b == Brand::PlayStation);
    const bool sw = (b == Brand::Switch);
    switch (sdlCode)
    {
    case 0:  return ps ? QString::fromUtf8("\xe2\x9c\x95") : sw ? QStringLiteral("B") : QStringLiteral("A");
    case 1:  return ps ? QString::fromUtf8("\xe2\x97\x8b") : sw ? QStringLiteral("A") : QStringLiteral("B");
    case 2:  return ps ? QString::fromUtf8("\xe2\x96\xa1") : sw ? QStringLiteral("Y") : QStringLiteral("X");
    case 3:  return ps ? QString::fromUtf8("\xe2\x96\xb3") : sw ? QStringLiteral("X") : QStringLiteral("Y");
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
