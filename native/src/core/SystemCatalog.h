// The list of emulated systems: which file extensions belong to each, and the candidate libretro cores
// for it (cores[0] is the default). Used by the settings dialog and to pick a core when launching a ROM.
//
// DATA-DRIVEN (issue #92). The table below is the BUILT-IN base — the systems the app ships knowing about.
// On top of it, `<data>/systems/*.json` may ADD new systems or OVERRIDE fields of a built-in one (swap the
// default core, add an extension) WITHOUT a rebuild, the same shape #52 asks for standalone emulators. The
// merge is:
//   * a data entry whose `id` is not in the built-in table is APPENDED as a new system;
//   * a data entry whose `id` matches a built-in overrides ONLY the fields it names (field-level, so a file
//     can swap `cores` alone without restating `extensions`);
//   * a malformed file (bad JSON, wrong top-level type, an entry with no `id`) is LOGGED AND SKIPPED — it can
//     never crash startup and can never drop the built-in table.
// With NO data files present, systems() is byte-for-byte the built-in table: probe_syscatalog pins that
// round-trip (built-in -> JSON -> in-memory == built-in) and the no-regression property, because system
// resolution (a ROM/folder -> which system) is load-bearing for every launch.
//
// The pure serialize/parse/merge functions (toJson/fromJson/overlay/applyEntries/parseEntries/loadDataDir)
// are QtCore-only and take their inputs explicitly, so the probe pins them against a temp dir with no app
// state in the way — the same discipline ThemeRegistry and AssetBootstrap follow. A ready-to-copy example
// carrying a handful of extra systems ships at native/resources/systems/example-systems.json.
//
// THEME ART. A system's `id` is the key a theme binds its system art (logo / tile / background) off of. A
// theme that has no art for a JSON-added id simply falls back to the generic system tile — the same
// undeclared-view philosophy the rest of the theming applies: an id a theme has never heard of degrades to a
// plain tile, it does not error. So adding a system by data makes it browsable and launchable immediately;
// bespoke key art for it is a separate, optional theme update.
#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <functional>
#include "AppPaths.h"

struct GameSystem
{
    QString id;             // stable key for settings
    QString name;           // display name
    QStringList extensions; // lowercase, no leading dot
    QStringList cores;      // candidate libretro core base names; [0] is the default
    // Non-empty => this system runs in a standalone emulator launched as a child process (see
    // EmulatorRegistry), not an in-process libretro core. The value is the ExternalEmulator id.
    QString externalEmulator;

    // ---- data-driven routing (issue #92) — all EMPTY for the built-in table (built-ins route through the
    // hardcoded folderAliases map in RomLibrary and the forConsoleName chain below). A JSON-added system
    // carries its own routing inline so it is a first-class citizen in scan/browse/launch without editing C++.
    QStringList folderAliases; // extra folder names (lowercase, no spaces) that route to this system on scan
    QStringList consoleHints;  // console/display-name substrings that route here (forConsoleName fallback)
    QStringList bios;          // BiosCatalog system ids this system needs; carried in the schema (see note)
};

inline bool operator==(const GameSystem& a, const GameSystem& b)
{
    return a.id == b.id && a.name == b.name && a.extensions == b.extensions && a.cores == b.cores
        && a.externalEmulator == b.externalEmulator && a.folderAliases == b.folderAliases
        && a.consoleHints == b.consoleHints && a.bios == b.bios;
}
inline bool operator!=(const GameSystem& a, const GameSystem& b) { return !(a == b); }

namespace SystemCatalog
{
    // The BUILT-IN table. systems() below is this merged with any <data>/systems/*.json. Kept as its own
    // accessor so the probe (and the merge) can name the base explicitly.
    inline const QList<GameSystem>& builtinSystems()
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
            // Atari 5200 needs its own machine + the 5200 BIOS (auto-fetched, see BiosCatalog "a5200"); its .a52
            // used to fall through to the Atari 800 core, which ran it as an 800 (wrong). Keep it above atari800.
            { "a5200",   "Atari 5200",                        { "a52" },
                         { "a5200" } },
            { "a7800",   "Atari 7800",                        { "a78" },
                         { "prosystem" } },
            // Handy boots without the Lynx boot ROM (Beetle Lynx needs lynxboot.img), so it's the default.
            { "lynx",    "Atari Lynx",                        { "lnx" },
                         { "handy", "mednafen_lynx" } },
            { "ngp",     "Neo Geo Pocket / Color",            { "ngp", "ngc", "ngpc" },
                         { "mednafen_ngp" } },
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
            { "atari800",     "Atari 800",           { "atr", "atx", "car", "cas" }, { "atari800" } },
            { "apple2",       "Apple II",            { "woz", "do", "po", "2mg", "nib", "dsk" }, { "applewin" } },
            // Amstrad CPC: cap32 embeds the CPC firmware (no external BIOS). .dsk collides with Apple II, so
            // claim the unambiguous tape/cartridge formats and route .dsk via the "Amstrad" console hint.
            { "amstradcpc",   "Amstrad CPC",         { "cdt", "cpr" },        { "cap32", "crocods" } },
            { "tic80",        "TIC-80 (fantasy)",    { "tic" },               { "tic80" } },
            { "uzebox",       "Uzebox",              { "uze" },               { "uzem" } },
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
            // launched as a child process. .iso is first claimed by saturn (listed above), so a loose .iso
            // with no hint routes to saturn via the extension router; gc and the PS2/PSP/Xbox entries (also
            // .iso, below) rely on #53's folder-authoritative scan to pick up their own .iso in their folder.
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
            // PSP: #53 makes the ROM FOLDER authoritative on scan, so PSP can now declare its shared formats
            // (.iso with GameCube, .pbp/.chd with PlayStation) and its psp/ folder picks them up. Loose-file
            // routing by extension is UNCHANGED: forExtension() is first-match-wins and the earlier systems
            // (saturn/gc own .iso, psx owns .chd/.pbp) are listed above, so a hint-less loose file still
            // resolves exactly as before — these later entries never displace the incumbent.
            { "psp",     "PlayStation Portable (PPSSPP)",
                         { "iso", "cso", "pbp", "chd", "dax", "prx" },
                         {}, "ppsspp" },
            { "psvita",  "PlayStation Vita (Vita3K)",
                         { "vpk" },
                         {}, "vita3k" },
            // PS3 games are usually folders or .pkg; .iso/etc are disambiguated by the console hint. .pkg is
            // unambiguous here (Vita uses .vpk), so claim it for the "is this a game?" check.
            { "ps3",     "PlayStation 3 (RPCS3)",
                         { "pkg" },
                         {}, "rpcs3" },
            // PS2 .iso/.chd/.cso all collide with earlier systems (GameCube/PlayStation/PSP). #53 makes the
            // ps2/ folder authoritative on scan, so PS2 can now declare them and its folder picks them up.
            // Loose-file routing is unchanged (first-match-wins keeps saturn/gc for .iso, psx for .chd, psp
            // for .cso — all listed above); the console hint ("PlayStation 2") still routes hint-carrying files.
            { "ps2",     "PlayStation 2 (PCSX2)",
                         { "iso", "chd", "cso" },
                         {}, "pcsx2" },
            // Dreamcast: .gdi/.cdi/.lst are unambiguous; #53 lets it also declare .chd/.cue/.iso (shared with
            // PlayStation/Saturn) since the dreamcast/ folder is now authoritative on scan. Loose files by
            // extension still resolve to the earlier incumbents (psx for .chd/.cue, saturn for .iso).
            { "dreamcast", "Dreamcast (Flycast)",
                         { "gdi", "cdi", "lst", "chd", "cue", "iso" },
                         {}, "flycast" },
            // Original Xbox. .xiso is unique; #53 lets the xbox/ folder also claim .iso (collides with
            // GameCube/PS2) authoritatively on scan. Loose .iso still routes to saturn (first-match-wins).
            { "xbox",    "Xbox (xemu)",
                         { "xiso", "iso" },
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

    // ---- pure: string-array field <-> QStringList -------------------------------------------------------
    // Read a JSON array-of-strings field into a QStringList (trimmed, empties dropped, optionally lowercased).
    // A non-array value yields an empty list. Kept in one place so every field parses identically.
    inline QStringList jsonStrList(const QJsonValue& v, bool lower)
    {
        QStringList out;
        if (!v.isArray()) return out;
        for (const QJsonValue& e : v.toArray())
        {
            if (!e.isString()) continue;
            const QString s = lower ? e.toString().trimmed().toLower() : e.toString().trimmed();
            if (!s.isEmpty()) out.push_back(s);
        }
        return out;
    }

    // ---- pure: GameSystem <-> canonical JSON ------------------------------------------------------------
    // Canonical serialization: id + name always written; every other field written ONLY when non-empty, so
    // there is exactly one spelling per system and fromJson(toJson(s)) == s (probe_syscatalog pins this).
    inline QJsonObject toJson(const GameSystem& s)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("name"), s.name);
        auto putArr = [&](const char* key, const QStringList& v) {
            if (v.isEmpty()) return;
            QJsonArray a;
            for (const QString& e : v) a.push_back(e);
            o.insert(QLatin1String(key), a);
        };
        putArr("extensions", s.extensions);
        putArr("cores", s.cores);
        if (!s.externalEmulator.isEmpty())
            o.insert(QStringLiteral("externalEmulator"), s.externalEmulator);
        putArr("folderAliases", s.folderAliases);
        putArr("consoleHints", s.consoleHints);
        putArr("bios", s.bios);
        return o;
    }

    // Overlay the fields PRESENT in `o` onto `base`, returning the result. A key that is absent leaves the
    // base value untouched (this is what makes an override field-level — a file may swap `cores` alone). The
    // single primitive behind both "add a new system" (base = default {}) and "override a built-in".
    // Extensions / folderAliases / consoleHints are lowercased (routing is case-insensitive); cores, id,
    // externalEmulator and bios ids keep their case.
    inline GameSystem overlay(const GameSystem& base, const QJsonObject& o)
    {
        GameSystem s = base;
        if (o.contains(QStringLiteral("id")))               s.id = o.value(QStringLiteral("id")).toString().trimmed();
        if (o.contains(QStringLiteral("name")))             s.name = o.value(QStringLiteral("name")).toString();
        if (o.contains(QStringLiteral("extensions")))       s.extensions = jsonStrList(o.value(QStringLiteral("extensions")), true);
        if (o.contains(QStringLiteral("cores")))            s.cores = jsonStrList(o.value(QStringLiteral("cores")), false);
        if (o.contains(QStringLiteral("externalEmulator"))) s.externalEmulator = o.value(QStringLiteral("externalEmulator")).toString().trimmed();
        if (o.contains(QStringLiteral("folderAliases")))    s.folderAliases = jsonStrList(o.value(QStringLiteral("folderAliases")), true);
        if (o.contains(QStringLiteral("consoleHints")))     s.consoleHints = jsonStrList(o.value(QStringLiteral("consoleHints")), true);
        if (o.contains(QStringLiteral("bios")))             s.bios = jsonStrList(o.value(QStringLiteral("bios")), false);
        return s;
    }

    // A full parse from a standalone object (default base). fromJson(toJson(s)) == s.
    inline GameSystem fromJson(const QJsonObject& o) { return overlay(GameSystem{}, o); }

    // ---- pure: merge a set of data entries over a base list ---------------------------------------------
    // For each entry: a non-object, or one whose `id` is empty/missing, is reported via `warn` and skipped
    // (never dropping the base). An entry whose id matches a base system overrides its named fields; a new id
    // is appended. Deterministic: entries are applied in the order given, later winning on the same id.
    inline QList<GameSystem> applyEntries(QList<GameSystem> base, const QJsonArray& entries,
                                          const std::function<void(const QString&)>& warn = {})
    {
        auto note = [&](const QString& m) { if (warn) warn(m); };
        int idx = 0;
        for (const QJsonValue& v : entries)
        {
            const int here = idx++;
            if (!v.isObject()) { note(QStringLiteral("entry %1 is not an object — skipped").arg(here)); continue; }
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty()) { note(QStringLiteral("entry %1 has no \"id\" — skipped").arg(here)); continue; }

            int found = -1;
            for (int i = 0; i < base.size(); ++i) if (base[i].id == id) { found = i; break; }
            if (found >= 0) base[found] = overlay(base[found], o);   // override named fields of a built-in
            else            base.push_back(overlay(GameSystem{}, o)); // append a new system
        }
        return base;
    }

    // ---- pure: read one file's bytes into an entry array ------------------------------------------------
    // A systems file is EITHER a JSON array of system objects OR a single system object (wrapped into a
    // one-element array). Unparseable bytes fail with a reason in *err and yield no entries — the
    // "malformed => logged and skipped" primitive. A top-level scalar (a bare number/string/bool/null) is not
    // a separate case: Qt's parser rejects it as a parse ERROR above, so a document that parses is always an
    // array or an object.
    inline bool parseEntries(const QByteArray& bytes, QJsonArray* out, QString* err)
    {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
        if (pe.error != QJsonParseError::NoError)
        {
            if (err) *err = QStringLiteral("not valid JSON (%1 at offset %2)").arg(pe.errorString()).arg(pe.offset);
            return false;
        }
        if (doc.isArray()) { if (out) *out = doc.array(); return true; }
        QJsonArray a; a.push_back(doc.object());  // a lone object -> a one-element array
        if (out) *out = a;
        return true;
    }

    // ---- pure(ish): load a whole <data>/systems directory over a base -----------------------------------
    // Reads *.json in name order (so the merge is deterministic and later files override earlier ones),
    // applying each file's entries over the accumulating catalog. A file that fails to parse is reported via
    // `warn` and skipped — the base survives intact. An empty/absent dir returns the base unchanged. Only
    // QtCore file I/O, so probe_syscatalog drives it against a temp directory.
    inline QList<GameSystem> loadDataDir(const QString& dir, const QList<GameSystem>& base,
                                         const std::function<void(const QString&)>& warn = {})
    {
        if (dir.isEmpty()) return base;
        QDir d(dir);
        if (!d.exists()) return base;

        QList<GameSystem> out = base;
        const QFileInfoList files = d.entryInfoList(QStringList{ QStringLiteral("*.json") },
                                                    QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : files)
        {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
            {
                if (warn) warn(QStringLiteral("%1: cannot open — skipped").arg(fi.fileName()));
                continue;
            }
            const QByteArray bytes = f.readAll();
            f.close();

            QJsonArray entries;
            QString perr;
            if (!parseEntries(bytes, &entries, &perr))
            {
                if (warn) warn(QStringLiteral("%1: %2 — skipped").arg(fi.fileName(), perr));
                continue;
            }
            out = applyEntries(out, entries,
                               [&](const QString& m) { if (warn) warn(QStringLiteral("%1: %2").arg(fi.fileName(), m)); });
        }
        return out;
    }

    // The default location the app merges from: <data>/systems.
    inline QString dataSystemsDir() { return AppPaths::dataDir() + QStringLiteral("/systems"); }

    // The merged catalog: the built-in table with <data>/systems/*.json applied over it. Computed ONCE on
    // first use (a new data file needs an app restart to take effect, like ES-DE). With no data files this is
    // the built-in table exactly — the no-regression rail probe_syscatalog pins.
    inline const QList<GameSystem>& systems()
    {
        static const QList<GameSystem> merged = loadDataDir(
            dataSystemsDir(), builtinSystems(),
            [](const QString& m) { qWarning("SystemCatalog: %s", qUtf8Printable(m)); });
        return merged;
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
        // Word-boundary match for short, ambiguous keys like "nes" that are substrings of unrelated
        // console names ("Sega Ge-nes-is"). Split on non-alphanumerics and test whole tokens.
        const auto words = n.split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
        auto hasWord = [&](const char* s) { return words.contains(QLatin1String(s)); };
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
        else if (has("famicom") || has("nintendo entertainment system") || hasWord("nes"))
                                                                          id = QStringLiteral("nes"); // after snes; word-boundary "nes" so "Sega Genesis" doesn't match it
        else if (has("dreamcast"))                                        id = QStringLiteral("dreamcast");
        else if (has("saturn"))                                           id = QStringLiteral("saturn");
        else if (has("sega cd 32x") || has("mega cd 32x") || has("mega-cd 32x")) id = QStringLiteral("segacd32x");
        else if (has("sega cd") || has("mega cd") || has("mega-cd") || has("segacd")) id = QStringLiteral("segacd");
        else if (has("sega 32x") || n == QLatin1String("32x"))            id = QStringLiteral("32x");
        else if (has("genesis") || has("mega drive") || has("master system")
                 || has("game gear"))                                     id = QStringLiteral("genesis");
        else if (has("sg-1000") || has("sg1000"))                         id = QStringLiteral("sg1000");
        else if (has("pc engine cd") || has("turbografx cd") || has("turbografx-cd") || has("turbo grafx cd")
                 || has("tg-cd") || has("tgcd") || has("pce cd") || has("pce-cd") || has("pcecd")) id = QStringLiteral("pcecd");
        else if (has("supergrafx") || has("super grafx") || has("pc engine") || has("turbografx") || has("turbo grafx")) id = QStringLiteral("pce");
        else if (has("wonderswan"))                                       id = QStringLiteral("ws");
        else if (has("virtual boy") || has("virtualboy"))                 id = QStringLiteral("virtualboy");
        else if (has("atari st"))                                         id = QStringLiteral("atarist"); // before atari 2600
        else if (has("atari 7800") || has("7800"))                        id = QStringLiteral("a7800");
        else if (has("atari 2600") || has("2600"))                        id = QStringLiteral("a2600");
        else if (has("atari lynx") || has("lynx"))                        id = QStringLiteral("lynx");
        else if (has("atari 5200") || has("5200"))                        id = QStringLiteral("a5200");
        else if (has("atari 800") || has("atari 8-bit") || has("atari800")) id = QStringLiteral("atari800");
        else if (has("apple ii") || has("apple //") || has("apple 2"))     id = QStringLiteral("apple2");
        else if (has("naomi 2") || has("naomi2"))                         id = QStringLiteral("naomi2");
        else if (has("naomi"))                                            id = QStringLiteral("naomi");
        else if (has("daphne") || has("laserdisc"))                       id = QStringLiteral("daphne");
        else if (has("pokemon mini") || has("poke mini"))                 id = QStringLiteral("pokemini");
        else if (has("supervision") || has("watara"))                    id = QStringLiteral("supervision");
        else if (has("game & watch") || has("game and watch") || has("game&watch")) id = QStringLiteral("gameandwatch");
        else if (has("neo geo pocket") || has("neogeo pocket") || has("ngpc") || hasWord("ngp")) id = QStringLiteral("ngp");
        else if (has("neo geo cd") || has("neogeo cd"))                   id = QStringLiteral("neogeocd");
        else if (has("neo geo") || has("neogeo"))                         id = QStringLiteral("neogeo");
        else if (has("pc-fx") || has("pcfx"))                             id = QStringLiteral("pcfx");
        else if (has("3do"))                                             id = QStringLiteral("3do");
        // Classic consoles & home computers
        else if (has("colecovision") || has("coleco"))                    id = QStringLiteral("coleco");
        else if (has("amstrad") || has("cpc"))                            id = QStringLiteral("amstradcpc");
        else if (has("tic-80") || has("tic80"))                           id = QStringLiteral("tic80");
        else if (has("uzebox"))                                           id = QStringLiteral("uzebox");
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
        if (!id.isEmpty()) return byId(id);

        // Data-driven fallback (issue #92): the hardcoded chain above resolves every BUILT-IN system (all of
        // which carry empty consoleHints, so none is reached here — built-in routing is byte-for-byte what it
        // was). A JSON-added system declares its own console-name hints, and a shelf labelled with one of them
        // routes to it by the same case-insensitive substring test the chain above uses (has()). The hints are
        // already lowercased at parse; first declaring system wins.
        for (const GameSystem& s : systems())
            for (const QString& hint : s.consoleHints)
                if (!hint.isEmpty() && n.contains(hint))
                    return &s;
        return nullptr;
    }
}
