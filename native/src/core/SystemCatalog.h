// The list of emulated systems: which file extensions belong to each, and the candidate libretro cores
// for it (cores[0] is the default). Used by the settings dialog and to pick a core when launching a ROM.
#pragma once
#include <QString>
#include <QStringList>
#include <QList>

struct GameSystem
{
    QString id;             // stable key for settings
    QString name;           // display name
    QStringList extensions; // lowercase, no leading dot
    QStringList cores;      // candidate libretro core base names; [0] is the default
    // Non-empty => this system runs in a standalone emulator launched as a child process (see
    // EmulatorRegistry), not an in-process libretro core. The value is the ExternalEmulator id.
    QString externalEmulator;
};

namespace SystemCatalog
{
    inline const QList<GameSystem>& systems()
    {
        static const QList<GameSystem> list = {
            { "gba",     "Game Boy Advance",                  { "gba" },
                         { "mgba", "vbam", "gpsp" } },
            { "gb",      "Game Boy / Color",                  { "gb", "gbc", "sgb", "dmg" },
                         { "gambatte", "mgba", "sameboy" } },
            // Note: the Mesen / Mesen-S cores are intentionally omitted - their buildbot Windows builds
            // fault on a worker thread inside this frontend (uncatchable by our per-call SEH guard).
            { "nes",     "NES / Famicom",                     { "nes", "fds", "unif", "unf" },
                         { "fceumm", "nestopia" } },
            { "snes",    "SNES / Super Famicom",              { "sfc", "smc", "bs", "st" },
                         { "snes9x", "bsnes_mercury_balanced" } },
            { "genesis", "Genesis / Mega Drive / SMS / GG",   { "md", "gen", "smd", "sms", "gg", "sg" },
                         { "genesis_plus_gx", "picodrive" } },
            { "n64",     "Nintendo 64",                       { "n64", "z64", "v64", "ndd" },
                         { "mupen64plus_next", "parallel_n64" } },
            { "psx",     "PlayStation",                       { "cue", "chd", "pbp", "m3u", "ccd", "exe" },
                         { "swanstation", "mednafen_psx_hw", "pcsx_rearmed" } },
            { "pce",     "PC Engine / TurboGrafx-16",         { "pce", "sgx" },
                         { "mednafen_pce", "mednafen_pce_fast" } },
            { "ws",      "WonderSwan",                        { "ws", "wsc" },
                         { "mednafen_wswan" } },
            { "a2600",   "Atari 2600",                        { "a26" },
                         { "stella" } },
            // Standalone (not a libretro core): GameCube/Wii are GPU-rendered, so they run in Dolphin,
            // launched as a child process. .iso is unclaimed by any system above; if a PS2/PSP system is
            // added later (also .iso) these will need ROM-folder disambiguation, ES-DE style.
            { "gc",      "GameCube / Wii (Dolphin)",
                         { "rvz", "iso", "gcm", "gcz", "ciso", "wia", "wbfs", "wad" },
                         {}, "dolphin" },
            { "3ds",     "Nintendo 3DS (Azahar)",
                         { "3ds", "cci", "cxi", "cia", "3dsx" },
                         {}, "azahar" },
            { "nds",     "Nintendo DS (melonDS)",
                         { "nds", "dsi", "srl" },
                         {}, "melonds" },
            { "wiiu",    "Wii U (Cemu)",
                         { "wud", "wux", "wua", "rpx" },
                         {}, "cemu" },
            { "switch",  "Nintendo Switch (Ryujinx)",
                         { "nsp", "xci", "nca", "nro" },
                         {}, "ryujinx" },
            // PSP shares .iso with GameCube and .pbp/.chd with PlayStation; since routing is by extension
            // (first match wins, and those systems are listed earlier), PSP can only safely claim its
            // unambiguous formats here. Disambiguating .iso/.pbp by console needs platform-aware routing.
            { "psp",     "PlayStation Portable (PPSSPP)",
                         { "cso", "dax", "prx" },
                         {}, "ppsspp" },
            { "psvita",  "PlayStation Vita (Vita3K)",
                         { "vpk" },
                         {}, "vita3k" },
            // PS3 games are usually folders or .pkg; .iso/etc are disambiguated by the console hint. .pkg is
            // unambiguous here (Vita uses .vpk), so claim it for the "is this a game?" check.
            { "ps3",     "PlayStation 3 (RPCS3)",
                         { "pkg" },
                         {}, "rpcs3" },
        };
        return list;
    }

    inline const GameSystem* forExtension(const QString& extLower)
    {
        for (const auto& s : systems())
            if (s.extensions.contains(extLower))
                return &s;
        return nullptr;
    }

    inline const GameSystem* byId(const QString& id)
    {
        for (const auto& s : systems())
            if (s.id == id)
                return &s;
        return nullptr;
    }

    // Map a console/platform display name (as the catalog labels it, e.g. "PSP", "GameCube", "Wii U") to a
    // system. Lets a game route to the right emulator when its file extension is shared across consoles
    // (PSP .iso vs GameCube .iso). Returns nullptr for consoles we don't emulate (caller falls back to the
    // extension). Order matters: the most specific names are tested first.
    inline const GameSystem* forConsoleName(const QString& consoleName)
    {
        const QString n = consoleName.toLower().trimmed();
        if (n.isEmpty()) return nullptr;
        auto has = [&](const char* s) { return n.contains(QLatin1String(s)); };
        QString id;
        if      (has("vita"))                                              id = QStringLiteral("psvita");
        else if (has("psp") || has("playstation portable"))               id = QStringLiteral("psp");
        else if (has("switch"))                                           id = QStringLiteral("switch");
        else if (has("wii u") || has("wiiu"))                             id = QStringLiteral("wiiu");
        else if (has("wii") || has("gamecube") || has("gcn"))             id = QStringLiteral("gc");
        else if (has("3ds"))                                              id = QStringLiteral("3ds");
        else if (has("nintendo ds") || has("nds"))                        id = QStringLiteral("nds");
        else if (has("nintendo 64") || has("n64"))                        id = QStringLiteral("n64");
        else if (has("snes") || has("super nintendo") || has("super famicom")) id = QStringLiteral("snes");
        else if (has("game boy advance") || has("gba"))                   id = QStringLiteral("gba");
        else if (has("game boy") || has("gbc"))                           id = QStringLiteral("gb");
        else if (has("famicom") || has("nes"))                            id = QStringLiteral("nes"); // after snes
        else if (has("genesis") || has("mega drive") || has("master system")
                 || has("game gear") || has("sg-1000"))                   id = QStringLiteral("genesis");
        else if (has("pc engine") || has("turbografx") || has("turbo grafx")) id = QStringLiteral("pce");
        else if (has("wonderswan"))                                       id = QStringLiteral("ws");
        else if (has("atari 2600") || has("2600"))                        id = QStringLiteral("a2600");
        // PlayStation last (after Vita/PSP). Specific consoles before the generic PS1 match.
        else if (has("playstation 3") || has("ps3"))                      id = QStringLiteral("ps3");
        else if (has("playstation 2") || has("playstation 4") || has("playstation 5")) id = QString(); // no emulator yet
        else if (has("playstation") || has("psx") || has("ps1") || has("psone")) id = QStringLiteral("psx");
        return id.isEmpty() ? nullptr : byId(id);
    }
}
