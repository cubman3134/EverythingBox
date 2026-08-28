// Pure translation from an on-screen keyboard hint to the controller button a player should press.
//
// A theme authors its help bar as keyboard chips ("Enter", "Esc", "I", "/") and themes come from a public
// registry, so the app does not control what a chip says. This is the one place those strings become
// buttons: hint -> UI verb -> RetroPad id -> (the pad's LIVE binding, supplied by the caller) -> a label
// spelled the way the connected brand spells it. Everything here is a pure function over its arguments —
// no SDL, no widgets, no state — so probe_padglyph can pin the whole table headlessly.
//
// A string this file does not recognise is handed straight back. That is deliberate: a third-party theme's
// own chip is the author's text, and inventing a button for it would be a lie the player then presses.
#pragma once
#include <QString>

namespace padglyphs
{

// How the connected controller spells its buttons. Generic is Xbox's spelling — the de-facto lingua franca
// in frontends — and is what an unrecognised pad resolves to.
enum class Brand { Xbox, PlayStation, Switch, Generic };

// The app verbs a help chip can name. None means "not one of ours" (an arrow chip, a third-party string).
enum class Verb { None, Confirm, Back, Details, Search, Filter, Playlist, Theme, Skip };

Brand   brandFromName(const QString& name);   // "xbox"|"playstation"|"switch" -> enum; else Generic
QString nameForBrand(Brand b);                // the inverse, lowercase

// The hint strings the app owns. Search and Skip share a RetroPad button because they never appear on the
// same surface (Search is the browse UI, Skip is the video player).
Verb verbForHint(const QString& hintKey);

// The RetroPad id (RETRO_DEVICE_ID_JOYPAD_*) a verb rides. -1 for Verb::None.
int  retroIdForVerb(Verb v);

// An SDL_GameControllerButton code (as stored by Gamepad::binding) spelled for a brand.
// NOTE: a Nintendo pad's face codes are LABEL-indexed, not positional (see the comment on the table in
// PadGlyphs.cpp) — which is why the Switch face letters equal the Xbox ones. Do not "correct" them. Empty for
// Gamepad::kUnbound and for anything outside the table — an empty label is how chip() knows to pass through.
QString labelForSdlCode(int sdlCode, Brand b);

// The one call a consumer makes. Returns `hintKey` unchanged when the verb is unknown or nothing is bound
// to it; otherwise the brand-correct label for `sdlCode`.
QString chip(const QString& hintKey, Brand b, int sdlCode);

} // namespace padglyphs
