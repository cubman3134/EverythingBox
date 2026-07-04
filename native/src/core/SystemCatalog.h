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
            { "genesis", "Genesis / Mega Drive / SMS / GG",   { "md", "gen", "smd", "sms", "gg" },
                         { "genesis_plus_gx", "picodrive" } },
            { "n64",     "Nintendo 64",                       { "n64", "z64", "v64", "ndd" },
                         { "mupen64plus_next", "parallel_n64" } },
            // PlayStation runs in standalone DuckStation (auto-downloaded). The libretro cores are kept here
            // only so removing the externalEmulator line restores the in-process path.
            { "psx",     "PlayStation",                       { "cue", "chd", "pbp", "m3u", "ccd", "exe" },
                         { "swanstation", "mednafen_psx_hw", "pcsx_rearmed" }, "duckstation" },
            // Sega Saturn: disc images; needs a Saturn BIOS (auto-fetched, see BiosCatalog "saturn"). Beetle
            // Saturn is the accuracy/compatibility default; Kronos is the OpenGL (hardware-rendered) alternative.
            { "saturn",  "Sega Saturn",                       { "cue", "chd", "ccd", "m3u", "iso" },
                         { "mednafen_saturn", "kronos" } },
            { "pce",     "PC Engine / TurboGrafx-16",         { "pce", "sgx" },
                         { "mednafen_pce", "mednafen_pce_fast" } },
            { "ws",      "WonderSwan",                        { "ws", "wsc" },
                         { "mednafen_wswan" } },
            { "virtualboy", "Virtual Boy",                    { "vb" },
                         { "mednafen_vb" } },
            { "a2600",   "Atari 2600",                        { "a26" },
                         { "stella" } },
            // ---- 8/16-bit consoles & home computers (in-process libretro cores, auto-downloaded) ----------
            // Several share file extensions with each other / earlier systems (e.g. VIC-20 and C64 both use
            // .prg/.d64), so those collisions are resolved by the console hint (forConsoleName) when a game is
            // opened from its shelf; the extensions here are the reasonably-unambiguous ones.
            { "sg1000",       "Sega SG-1000",        { "sg" },                 { "genesis_plus_gx", "bluemsx" } },
            { "coleco",       "ColecoVision",        { "col" },                { "gearcoleco", "bluemsx" } },
            { "vectrex",      "Vectrex",             { "vec" },                { "vecx" } },
            { "intellivision","Intellivision",       { "int" },                { "freeintv" } },
            { "odyssey2",     "Magnavox Odyssey 2",  { "o2" },                 { "o2em" } },
            { "channelf",     "Fairchild Channel F", { "chf" },                { "freechaf" } },
            { "amiga",        "Commodore Amiga",     { "adf", "adz", "hdf", "uae", "dms" }, { "puae", "puae2021" } },
            { "atarist",      "Atari ST",            { "msa", "stx", "dim" },  { "hatari" } },
            { "pc98",         "NEC PC-9801",         { "d98", "fdi", "hdi", "98d" }, { "np2kai" } },
            { "x1",           "Sharp X1",            { "dx1", "2d", "2hd" },   { "x1" } },
            { "zxspectrum",   "Sinclair ZX Spectrum",{ "tzx", "z80", "szx", "rzx", "scl", "trd", "sna" }, { "fuse" } },
            { "c64",          "Commodore 64",        { "d64", "t64", "prg", "crt", "g64", "x64", "p00" }, { "vice_x64", "vice_x64sc" } },
            { "msdos",        "MS-DOS",              { "dosz", "com", "bat", "conf" }, { "dosbox_pure", "dosbox_core" } },
            { "vic20",        "Commodore VIC-20",    { "20", "40", "60", "a0", "b0" }, { "vice_xvic" } },
            // More consoles / computers / arcade / CD systems. CD and arcade systems share cue/chd/iso/zip
            // with earlier systems, so they claim only unambiguous extensions (often none) and are routed by
            // the console hint (forConsoleName) when launched from their shelf.
            { "atari800",     "Atari 800",           { "atr", "atx", "car", "a52", "cas" }, { "atari800" } },
            { "apple2",       "Apple II",            { "woz", "do", "po", "2mg", "nib", "dsk" }, { "applewin" } },
            { "pokemini",     "Pokemon Mini",        { "min" },               { "pokemini" } },
            { "supervision",  "Watara Supervision",  { "sv" },                { "potator" } },
            { "gameandwatch", "Nintendo Game & Watch", { "mgw" },             { "gw" } },
            { "neogeo",       "Neo Geo",             { "neo" },               { "geolith", "fbneo" } },
            { "32x",          "Sega 32X",            { "32x" },               { "picodrive", "genesis_plus_gx" } },
            { "daphne",       "Daphne (Laserdisc)",  { "daphne" },            { "dirksimple" } },
            { "segacd",       "Sega CD / Mega-CD",   { },                     { "genesis_plus_gx", "picodrive" } },
            { "segacd32x",    "Sega CD 32X",         { },                     { "picodrive" } },
            { "pcecd",        "PC Engine CD / TurboGrafx-CD", { },            { "mednafen_pce", "mednafen_pce_fast" } },
            { "pcfx",         "NEC PC-FX",           { },                     { "mednafen_pcfx" } },
            { "neogeocd",     "Neo Geo CD",          { },                     { "neocd" } },
            { "3do",          "3DO",                 { },                     { "opera" } },
            { "cdtv",         "Commodore CDTV",      { },                     { "puae", "puae2021" } },
            { "naomi",        "Sega Naomi",          { },                     {}, "flycast" },
            { "naomi2",       "Sega Naomi 2",        { },                     {}, "flycast" },
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
            // PS2 .iso/.chd/.cso all collide with earlier systems (GameCube/PlayStation/PSP), so this claims
            // no extensions - PS2 games route here via the console hint ("PlayStation 2") set from the shelf.
            { "ps2",     "PlayStation 2 (PCSX2)",
                         {},
                         {}, "pcsx2" },
            // Dreamcast .chd/.cue collide with PlayStation; claim only the unambiguous formats here, and let
            // the console hint route .chd/.cue games opened from the Dreamcast shelf.
            { "dreamcast", "Dreamcast (Flycast)",
                         { "gdi", "cdi", "lst" },
                         {}, "flycast" },
            // Original Xbox. .iso collides with GameCube/PS2, so claim only .xiso; .iso games route via the
            // console hint ("Xbox" -> xbox).
            { "xbox",    "Xbox (xemu)",
                         { "xiso" },
                         {}, "xemu" },
            // Xbox 360. .iso collides with GameCube/PS2/Xbox; claim .xex (unique to 360) + .zar, and route
            // .iso games via the console hint ("Xbox 360" -> xbox360).
            { "xbox360", "Xbox 360 (Xenia)",
                         { "xex", "zar" },
                         {}, "xenia" },
            // Atari Jaguar + Jaguar CD (BigPEmu). Cart formats are unique here; Jaguar CD images (.cue/.cdi)
            // collide with PlayStation/Dreamcast and route via the console hint ("Atari Jaguar" -> jaguar).
            { "jaguar",  "Atari Jaguar / Jaguar CD (BigPEmu)",
                         { "j64", "jag", "abs", "cof" },
                         {}, "bigpemu" },
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
        else if (has("dreamcast"))                                        id = QStringLiteral("dreamcast");
        else if (has("saturn"))                                           id = QStringLiteral("saturn");
        else if (has("sega cd 32x") || has("mega cd 32x") || has("mega-cd 32x")) id = QStringLiteral("segacd32x");
        else if (has("sega cd") || has("mega cd") || has("mega-cd") || has("segacd")) id = QStringLiteral("segacd");
        else if (has("sega 32x") || n == QLatin1String("32x"))            id = QStringLiteral("32x");
        else if (has("genesis") || has("mega drive") || has("master system")
                 || has("game gear"))                                     id = QStringLiteral("genesis");
        else if (has("sg-1000") || has("sg1000"))                         id = QStringLiteral("sg1000");
        else if (has("pc engine cd") || has("turbografx cd") || has("turbo grafx cd") || has("tg-cd")) id = QStringLiteral("pcecd");
        else if (has("supergrafx") || has("super grafx") || has("pc engine") || has("turbografx") || has("turbo grafx")) id = QStringLiteral("pce");
        else if (has("wonderswan"))                                       id = QStringLiteral("ws");
        else if (has("virtual boy") || has("virtualboy"))                 id = QStringLiteral("virtualboy");
        else if (has("atari st"))                                         id = QStringLiteral("atarist"); // before atari 2600
        else if (has("atari 2600") || has("2600"))                        id = QStringLiteral("a2600");
        else if (has("atari 800") || has("atari 8-bit") || has("atari800")) id = QStringLiteral("atari800");
        else if (has("apple ii") || has("apple //") || has("apple 2"))     id = QStringLiteral("apple2");
        else if (has("naomi 2") || has("naomi2"))                         id = QStringLiteral("naomi2");
        else if (has("naomi"))                                            id = QStringLiteral("naomi");
        else if (has("daphne") || has("laserdisc"))                       id = QStringLiteral("daphne");
        else if (has("pokemon mini") || has("poke mini"))                 id = QStringLiteral("pokemini");
        else if (has("supervision") || has("watara"))                    id = QStringLiteral("supervision");
        else if (has("game & watch") || has("game and watch") || has("game&watch")) id = QStringLiteral("gameandwatch");
        else if (has("neo geo pocket") || has("neogeo pocket"))           id = QString();          // NGP/NGPC: no core yet
        else if (has("neo geo cd") || has("neogeo cd"))                   id = QStringLiteral("neogeocd");
        else if (has("neo geo") || has("neogeo"))                         id = QStringLiteral("neogeo");
        else if (has("pc-fx") || has("pcfx"))                             id = QStringLiteral("pcfx");
        else if (has("3do"))                                             id = QStringLiteral("3do");
        // Classic consoles & home computers
        else if (has("colecovision") || has("coleco"))                    id = QStringLiteral("coleco");
        else if (has("vectrex"))                                          id = QStringLiteral("vectrex");
        else if (has("intellivision"))                                    id = QStringLiteral("intellivision");
        else if (has("odyssey") || has("videopac"))                       id = QStringLiteral("odyssey2");
        else if (has("channel f") || has("channelf") || has("fairchild")) id = QStringLiteral("channelf");
        else if (has("amiga"))                                            id = QStringLiteral("amiga");
        else if (has("pc-9801") || has("pc-9800") || has("pc-98") || has("pc98")) id = QStringLiteral("pc98");
        else if (has("sharp x1") || n == QLatin1String("x1"))             id = QStringLiteral("x1");
        else if (has("zx spectrum") || has("spectrum") || has("sinclair")) id = QStringLiteral("zxspectrum");
        else if (has("cdtv"))                                            id = QStringLiteral("cdtv");  // before commodore->c64
        else if (has("vic-20") || has("vic20") || has("vic"))             id = QStringLiteral("vic20"); // before commodore->c64
        else if (has("commodore 64") || has("c64") || has("commodore"))   id = QStringLiteral("c64");
        else if (has("ms-dos") || has("msdos") || n == QLatin1String("dos")) id = QStringLiteral("msdos");
        else if (has("xbox 360") || has("xbox360"))                       id = QStringLiteral("xbox360");
        else if (has("xbox"))                                            id = QStringLiteral("xbox");
        else if (has("jaguar"))                                          id = QStringLiteral("jaguar"); // Jaguar + Jaguar CD
        // PlayStation last (after Vita/PSP). Specific consoles before the generic PS1 match.
        else if (has("playstation 3") || has("ps3"))                      id = QStringLiteral("ps3");
        else if (has("playstation 2") || has("ps2"))                      id = QStringLiteral("ps2");
        else if (has("playstation 4") || has("playstation 5"))            id = QString(); // no emulator yet
        else if (has("playstation") || has("psx") || has("ps1") || has("psone")) id = QStringLiteral("psx");
        return id.isEmpty() ? nullptr : byId(id);
    }
}
