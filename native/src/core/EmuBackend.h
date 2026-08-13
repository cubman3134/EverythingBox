// The emulation-backend vocabulary (RetroPark Slice 2a). ONE responsibility: name the two engines a game can
// launch on and convert to/from the stable string form persisted in LaunchOpts::Override::backend and the
// "backends/<systemId>" settings key. Shared by the store (the per-game override), Settings (the per-system /
// global default), the launcher (CorePlan::backend), and the view — so the spelling lives in exactly one place.
//
// Libretro is the historical default and the fallback for every unknown/empty value: until a user opts a game
// or system into RetroPark, every launch resolves to Libretro and behaves byte-identically to today. The
// string forms are "libretro" / "retropark" (lower-case, stable — they are written to the portable ini and to
// the CloudMerge document, so they must never change spelling).
//
// Header-only (inline): the two converters are trivial and pure, so every target that needs the vocabulary
// includes this header without a new .cpp to link.
#pragma once
#include <QString>

enum class EmuBackend { Libretro, RetroPark };

// Canonical string form (what the ini / sync document store). Libretro is the default arm.
inline QString backendToString(EmuBackend b)
{
    switch (b)
    {
        case EmuBackend::RetroPark: return QStringLiteral("retropark");
        case EmuBackend::Libretro:  break;
    }
    return QStringLiteral("libretro");
}

// Parse a stored string. ONLY the exact recognised spellings map to their backend; every unknown/empty/retired
// value maps to Libretro — the safe default that keeps an un-opted game on today's path. (Note: this collapses
// "unknown" to Libretro; a CALLER that must fall back to a NON-Libretro default instead — e.g. a per-system
// default that is itself RetroPark — must not route through here, which is exactly why LaunchOpts::resolveBackend
// checks the recognised set directly rather than delegating.)
inline EmuBackend backendFromString(const QString& s)
{
    if (s == QStringLiteral("retropark")) return EmuBackend::RetroPark;
    return EmuBackend::Libretro;
}

// Recognise a stored string WITHOUT the unknown->Libretro collapse: returns true and writes the matching backend to
// `out` only for an exact canonical spelling, false for empty/unknown/retired. This is the single home of the
// recognised-spelling set — a caller that must fall an UNRECOGNISED value back to a possibly-NON-Libretro default
// (LaunchOpts::resolveBackend, whose default can itself be RetroPark) asks here instead of hardcoding the
// "retropark"/"libretro" literals a second time.
inline bool tryBackendFromString(const QString& s, EmuBackend& out)
{
    if (s == QStringLiteral("retropark")) { out = EmuBackend::RetroPark; return true; }
    if (s == QStringLiteral("libretro"))  { out = EmuBackend::Libretro;  return true; }
    return false;
}
