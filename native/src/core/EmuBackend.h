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

// RetroPark Slice 2b/3b: which emulated systems the RetroPark backend can actually run real content for. Three
// shapes are supported today:
//   * "nes"  — the shipped libretro shim hardwires fceumm_libretro.dll (a DRIVEN core, CPU pixels on D3D11);
//   * "gc"   — the Dolphin PRESENTING core (GPU-rendered GameCube/Wii, read back on a Vulkan runtime, Slice 3b).
//   * "n64"  — the libretro shim driving Mupen64Plus-Next: a HW-render libretro core (the shim GL-reads the core's
//              framebuffer back to CPU pixels), so it stays on the DRIVEN path like NES (D3D11, not presenting).
// This one predicate is the single home of that fact: the per-game picker offers the RetroPark option only where
// it returns true, and the launcher routes a RetroPark backend to the in-process path (clamping it back to the
// system's normal launch — libretro core or standalone emulator — where it returns false, below). Data-driven on
// the canonical SystemCatalog id, so broadening support when the shim/vehicles grow is a one-line change here
// rather than a hunt through the picker + launcher. NOT gated on EB_HAVE_RETROPARK — it is pure vocabulary every
// target (incl. the headless probe) can reason about.
inline bool retroParkSupportsSystem(const QString& systemId)
{
    return systemId == QStringLiteral("nes") || systemId == QStringLiteral("gc")
        || systemId == QStringLiteral("n64");
}

// RetroPark: which supported systems feed the ABSTRACT PAD (pad_buttons/pad_axes: analog sticks + gamepad buttons)
// rather than the NES keys[]-only path. True for the systems whose cores read the abstract pad — the GC (Dolphin)
// PRESENTING core and the N64 (Mupen64Plus-Next) libretro shim (both have an analog stick + a full button
// cluster). NES stays false (its FCEUmm shim reads only keys[]). RetroParkView::feedInput routes a gamepad system
// through the abstract-pad build+feed; a non-gamepad supported system (nes) through the keys[] path. Pure
// vocabulary, no EB_HAVE_RETROPARK.
inline bool retroParkSystemUsesGamepad(const QString& systemId)
{
    return systemId == QStringLiteral("gc") || systemId == QStringLiteral("n64");
}

// RetroPark Slice 3b: the CORE KIND a RetroPark-backed system runs on — DRIVEN (CPU pixels the runtime uploads,
// on the proven headless D3D11 runtime; the NES/fceumm-shim path) vs PRESENTING (the core renders on the GPU
// itself and the runtime reads its frame back, which only works on a headless VULKAN runtime; the Dolphin/gc
// path). The runtime's graphics API must be chosen at rp_runtime_create — BEFORE any core is loaded — so the
// launcher must know this KIND up front and thread it to the view (which maps it via rpapi::runtimeApiForCore).
// The single home of the per-system driven-vs-presenting fact, beside retroParkSupportsSystem: a system this
// returns true for is only meaningful when retroParkSupportsSystem is also true. "gc" is presenting; every other
// supported system (today just "nes") is driven, so the default is false. Pure vocabulary, no EB_HAVE_RETROPARK.
inline bool retroParkSystemIsPresenting(const QString& systemId)
{
    return systemId == QStringLiteral("gc");
}

// RetroPark Slice 2b: the launch safety net. After the backend is resolved (per-game override over per-system /
// global default), clamp it to what the system can actually run: a RetroPark backend on a system RetroPark does
// NOT support (see retroParkSupportsSystem) falls back to Libretro so a stale synced per-game override — or a
// global/per-system RetroPark default carried in from another machine — can never brick an unsupported launch.
// The stored preference is untouched; it is simply not honoured where unsupported. Libretro is never altered.
// Shared by GameLauncher::prepareCore and probe_launchopts so this decision is mutation-tested in exactly one
// place. (Note: this is the LIBRETRO-arm clamp — the standalone arm applies the same support gate inline before
// its external-emulator early-return, since a standalone system has no libretro core to fall back TO.)
inline EmuBackend clampBackendToSystem(EmuBackend backend, const QString& systemId)
{
    if (backend == EmuBackend::RetroPark && !retroParkSupportsSystem(systemId))
        return EmuBackend::Libretro;
    return backend;
}

// RetroPark Slice 3b: the STANDALONE-arm presenting-divert decision, factored pure so probe_launchopts can
// mutation-test it (GameLauncher::prepareCore isn't headless-constructible). A standalone system (today gc →
// external Dolphin) diverts to the in-process RetroPark PRESENTING path instead of launching its external emulator
// ONLY when all THREE hold:
//   1. RetroPark supports the system (retroParkSupportsSystem),
//   2. the resolved backend is RetroPark (per-game override over per-system / global default), AND
//   3. the local-only Dolphin vehicle is actually staged on THIS machine (vehiclePresent).
// The third gate is the 3b safety net: dolphin_present.dll is LOCAL-ONLY (git-ignored, absent on most machines and
// on CI / a fresh clone), so without it the presenting route would hard-fail "Dolphin core not installed" and lose
// external Dolphin entirely — a user who sets the GLOBAL/per-system default to RetroPark (natural for NES) would
// have every GC game captured and bricked. When the vehicle is ABSENT we must NOT divert; the caller falls through
// to the unchanged external-Dolphin launch, which is then the automatic fallback everywhere the vehicle isn't
// present. The caller supplies vehiclePresent (a QFileInfo::exists on <coresDir>/dolphin_present/dolphin_present.dll
// — the same cores-dir resolver RetroParkView loads the core from) so this stays pure/testable. NOT the libretro
// arm: NES's shim is built into EB, needs no local vehicle, and keeps clampBackendToSystem above. Pure vocabulary,
// no EB_HAVE_RETROPARK.
inline bool retroParkStandaloneDivert(const QString& systemId, EmuBackend resolvedBackend, bool vehiclePresent)
{
    return retroParkSupportsSystem(systemId)
        && resolvedBackend == EmuBackend::RetroPark
        && vehiclePresent;
}
