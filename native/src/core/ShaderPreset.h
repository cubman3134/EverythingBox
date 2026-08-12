// Slang-shader PRESET model — the pure, header-only foundation of the shader pipeline (issue #99, SLICE 1).
//
// SCOPE OF THIS HEADER (and what it deliberately is NOT): this NAMES presets and resolves which one applies.
// It does NOT load a `.slangp`, does NOT touch librashader, and does NOT render anything. The actual shader
// LOADING + the GL filter-chain that consumes the resolved id are LATER slices; librashader is not vendored
// yet. Everything here is a pure function of strings/structs so it mutation-tests under QtCore alone, exactly
// like EmuSettings.h (issue #103) which this mirrors.
//
// WHAT A PRESET REFERENCE IS. A single QString id carries the whole choice, so one field in Settings and one
// value in ShaderPresetStore can hold any of three things:
//   * ""            — UNSET (no choice recorded at this scope; resolvePreset() falls through to the next scope).
//   * "off"         — an explicit "no shader" choice (distinct from unset: it WINS resolution at its scope).
//   * a builtin id  — one of the curated registry ids below ("crt", "lcd-grid", "sharp", …).
//   * "custom:<abs>"— a user's own .slangp on disk, the absolute path carried behind the customPrefix() sentinel.
//
// CURATED REGISTRY. A small fixed list of built-ins the picker offers by default. The issue asks for "one good
// CRT, one LCD-grid, one sharp-scaler"; the four legacy VideoFilter names (off/scanlines/crt/lcd) also become
// presets so nobody's persisted setting breaks (see presetIdForLegacyFilter). `heavy` marks a Mega-Bezel-class
// chain that will humble a weak GPU — a later slice surfaces a "this is heavy" warning off this flag. `slangp`
// is the preset FILE a later slice will hand to librashader; it is descriptive metadata here, loaded by nobody.
#pragma once
#include <QString>
#include <QVector>

namespace ShaderPreset
{
    // The three kinds a preset id resolves to (see kindForId).
    enum class Kind { Off, Builtin, Custom };

    // One curated built-in preset. FIELDS ARE DATA ONLY — nothing here opens `slangp`.
    struct Entry
    {
        QString id;           // stable id persisted in Settings / ShaderPresetStore (never translated)
        QString displayName;  // human label for the picker (translatable at the UI edge, not here)
        QString slangp;       // the .slangp a LATER slice will load via librashader ("" for off)
        bool    heavy = false;// Mega-Bezel-class: a future "this will humble a weak GPU" warning reads this
    };

    // ---- sentinels ------------------------------------------------------------------------------------------
    inline QString offId()        { return QStringLiteral("off"); }      // the explicit "no shader" id
    inline QString customPrefix() { return QStringLiteral("custom:"); }  // a user .slangp path rides behind this

    // ---- the curated registry (a fixed built-in list; the ONLY source of built-in ids) ----------------------
    // Hand-authored, order = display order. The four legacy VideoFilter names map ONTO these (migration below),
    // so off/scanlines/crt/lcd all survive as a preset. Plus a sharp-scaler, plus one heavy Mega-Bezel entry so
    // the heavy flag is exercised by something real. The slangp paths are the libretro slang-shaders repo layout
    // a later slice will resolve against — descriptive only in this slice.
    // Slice 4 SHIPS the four light presets: their `slangp` is a bare filename resolved against the on-disk
    // shaders root (ShaderRenderer::shadersRoot(), extracted from the app's :/eb/shaders bundle at first use).
    // `mega-bezel` names an ECOSYSTEM path (the libretro slang-shaders layout) that this app does NOT ship — it
    // is the load-from-a-user-installed-slang-shaders-dir follow-up; until such a dir exists the resolver finds
    // no file and RetroView falls back to the plain (no-shader) draw. That keeps the heavy flag exercised by a
    // real entry without shipping a Mega-Bezel-class chain we cannot author minimally.
    inline QVector<Entry> registry()
    {
        return {
            { offId(),                    QStringLiteral("Off (no shader)"),  QString(),                                                false },
            { QStringLiteral("scanlines"),QStringLiteral("Scanlines"),        QStringLiteral("scanlines.slangp"),                       false },
            { QStringLiteral("crt"),      QStringLiteral("CRT"),              QStringLiteral("crt.slangp"),                             false },
            { QStringLiteral("lcd-grid"), QStringLiteral("LCD Grid"),         QStringLiteral("lcd-grid.slangp"),                        false },
            { QStringLiteral("sharp"),    QStringLiteral("Sharp Scaler"),     QStringLiteral("sharp-bilinear.slangp"),                  false },
            { QStringLiteral("mega-bezel"),QStringLiteral("Mega Bezel"),      QStringLiteral("bezel/Mega_Bezel/Presets/MBZ__3__STD.slangp"), true },
        };
    }

    // ---- registry lookups (pure) ----------------------------------------------------------------------------
    inline bool isBuiltinId(const QString& id)
    {
        for (const Entry& e : registry()) if (e.id == id) return true;
        return false;
    }

    // The entry for a built-in id, or a default-constructed (empty-id) Entry if the id is not curated.
    inline Entry entryForId(const QString& id)
    {
        for (const Entry& e : registry()) if (e.id == id) return e;
        return Entry{};
    }

    // Whether the id names a curated HEAVY (Mega-Bezel-class) preset. A non-registry id is not heavy.
    inline bool isHeavyId(const QString& id)
    {
        for (const Entry& e : registry()) if (e.id == id) return e.heavy;
        return false;
    }

    // ---- custom (on-disk) presets (pure) --------------------------------------------------------------------
    // Wrap an absolute .slangp path into a preset id, and unwrap it. The wrap keeps the single-string field able
    // to hold either a builtin id or a disk path with no ambiguity (a builtin id never contains ':').
    inline QString customPresetId(const QString& absoluteSlangpPath) { return customPrefix() + absoluteSlangpPath; }
    inline bool    isCustomId(const QString& id)   { return id.startsWith(customPrefix()); }
    inline QString customPath(const QString& id)   { return isCustomId(id) ? id.mid(customPrefix().size()) : QString(); }

    // What a stored id resolves to. "" and "off" are BOTH Kind::Off (nothing to load); the difference between
    // unset and explicit-off matters only to resolvePreset(), never to the loader that reads this.
    inline Kind kindForId(const QString& id)
    {
        if (id.isEmpty() || id == offId()) return Kind::Off;
        if (isCustomId(id))                return Kind::Custom;
        return Kind::Builtin;
    }

    // ---- legacy VideoFilter migration (pure) ----------------------------------------------------------------
    // Seed the shader world from the existing Settings::videoFilter() so an upgrading user's Scanlines/CRT/LCD
    // choice carries over. The legacy ids are exactly "scanlines" / "crt" / "lcd" / "off" (RetroView), with the
    // default (and anything unrecognised) meaning no shader. Each legacy name maps to its like-named preset.
    inline QString presetIdForLegacyFilter(const QString& videoFilterId)
    {
        if (videoFilterId == QStringLiteral("scanlines")) return QStringLiteral("scanlines");
        if (videoFilterId == QStringLiteral("crt"))       return QStringLiteral("crt");
        if (videoFilterId == QStringLiteral("lcd"))       return QStringLiteral("lcd-grid");
        return offId();   // "", "off", or any unknown value -> no shader
    }

    // ---- scope resolution (pure) ----------------------------------------------------------------------------
    // Per-game override wins; else the per-system default; else the global default. An EMPTY string means
    // "unset at this scope" and falls through — this is the #51/#103 layering, mirroring EmuGfx::resolve. Note
    // "off" is NOT empty: a per-game "off" beats a per-system "crt", which is the point of an explicit off.
    inline QString resolvePreset(const QString& perGame, const QString& perSystem, const QString& globalDefault)
    {
        if (!perGame.isEmpty())   return perGame;
        if (!perSystem.isEmpty()) return perSystem;
        return globalDefault;
    }
}
