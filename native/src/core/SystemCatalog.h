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
}
