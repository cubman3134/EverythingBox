// Headless check of the cross-emulator graphics settings mapping (src/core/EmuSettings.h, issue #103) and the
// per-game override store (src/core/EmuGfxStore). QtCore-only — the mapping is PURE (no disk, no live emulator),
// the store is a QSettings wrapper over the isolated everythingbox.ini — so it runs under the offscreen QPA in
// CI. Prints EMUSETTINGS-OK on success; any failure prints EMUSETTINGS-FAIL <cond> (line) and exits non-zero.
//
// WHAT IT PINS:
//   * configEdits maps each quartet lever onto the RIGHT (file, section, key, value) for Dolphin / PCSX2 /
//     DuckStation, and Flycast's resolution;
//   * the resolution lever FORMATS per emulator: a MULTIPLIER integer for Dolphin/PCSX2/DuckStation, a PIXEL
//     HEIGHT (multiplier x 480 native lines) for Flycast — 2x is "2" for the first three and "960" for Flycast;
//   * an unsupported lever yields NO edit (PCSX2 has no MSAA, Flycast no aspect, Dolphin no "auto" backend);
//   * a single set lever yields exactly ONE edit; an all-unset Settings yields none; an unknown emulator none;
//   * resolve() layers a per-game override over a per-system default field-by-field;
//   * toJson/fromJson round-trip and omit unset fields;
//   * the store round-trips a record by key, an absent key reads empty, a clear is a plain delete (device-local,
//     no husk), and the per-system default is reachable via systemKey/systemDefault.
//
// FIXTURES ARE HAND-AUTHORED, INDEPENDENT OF THE CODE UNDER TEST: every expected (section, key, value) is
// written here by hand from each emulator's documented config — NOT produced by running configEdits — and the
// raw store leaf is addressed by an MD5 taken with QCryptographicHash directly, not via EmuGfxStore::hashKey. An
// assertion therefore cannot pass merely because it re-ran the function it is checking.
#include "EmuSettings.h"
#include "EmuGfxStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace EmuGfx;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "EMUSETTINGS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The value of the edit matching (file, section, key), or "<none>" if there is no such edit. Hand-driven lookup,
// so an assertion names exactly the tuple it expects.
static QString ev(const QVector<ConfigEdit>& edits, const QString& file, const QString& section, const QString& key)
{
    for (const ConfigEdit& e : edits)
        if (e.file == file && e.section == section && e.key == key) return e.value;
    return QStringLiteral("<none>");
}

// Independent oracle for the store's key hashing (md5-hex over UTF-8), so the probe addresses a game's raw ini
// blob without calling EmuGfxStore::hashKey.
static QString md5hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// A single-lever Settings, so each mapping assertion isolates one lever (exactly one edit expected).
static Settings resOnly(int m)      { Settings s; s.resMultiplier = m; return s; }
static Settings aspectOnly(Aspect a){ Settings s; s.aspect = a; return s; }
static Settings vsyncOnly(Vsync v)  { Settings s; s.vsync = v; return s; }
static Settings rendOnly(Renderer r){ Settings s; s.renderer = r; return s; }
static Settings msaaOnly(int n)     { Settings s; s.msaa = n; return s; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    const QString dGfx = QStringLiteral("User/Config/GFX.ini");     // Dolphin graphics ini
    const QString dIni = QStringLiteral("User/Config/Dolphin.ini"); // Dolphin core ini (backend)
    const QString pIni = QStringLiteral("inis/PCSX2.ini");
    const QString gs   = QStringLiteral("EmuCore/GS");
    const QString dsIni = QStringLiteral("settings.ini");

    // ==== 1. RESOLUTION — multiplier vs pixel height, and exactly one edit per emulator ======================
    {
        // Multiplier emulators: value is the multiplier itself.
        CHECK(configEdits(QStringLiteral("dolphin"), resOnly(2)).size() == 1);
        CHECK(ev(configEdits(QStringLiteral("dolphin"), resOnly(2)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("InternalResolution")) == QStringLiteral("2"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), resOnly(2)), pIni, gs,
                 QStringLiteral("upscale_multiplier")) == QStringLiteral("2"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), resOnly(2)), dsIni, QStringLiteral("GPU"),
                 QStringLiteral("ResolutionScale")) == QStringLiteral("2"));

        // Pixel-height emulator: Flycast's rend.Resolution is vertical lines = multiplier x 480 (native). So 2x
        // is 960, NOT 2 — the contrast the issue calls out. 1x is exactly the native 480 (pins the x480 factor).
        CHECK(configEdits(QStringLiteral("flycast"), resOnly(2)).size() == 1);
        CHECK(ev(configEdits(QStringLiteral("flycast"), resOnly(2)), QStringLiteral("emu.cfg"),
                 QStringLiteral("config"), QStringLiteral("rend.Resolution")) == QStringLiteral("960"));
        CHECK(ev(configEdits(QStringLiteral("flycast"), resOnly(1)), QStringLiteral("emu.cfg"),
                 QStringLiteral("config"), QStringLiteral("rend.Resolution")) == QStringLiteral("480"));
    }

    // ==== 2. ASPECT RATIO — the enum spelling per emulator; 4:3 and 16:9 are DISTINCT (kills a swap) =========
    {
        CHECK(ev(configEdits(QStringLiteral("dolphin"), aspectOnly(Aspect::R16_9)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("1"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), aspectOnly(Aspect::R4_3)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("2"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), aspectOnly(Aspect::Auto)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("0"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), aspectOnly(Aspect::Stretch)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("3"));

        CHECK(ev(configEdits(QStringLiteral("pcsx2"), aspectOnly(Aspect::R16_9)), pIni, gs,
                 QStringLiteral("AspectRatio")) == QStringLiteral("16:9"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), aspectOnly(Aspect::R4_3)), pIni, gs,
                 QStringLiteral("AspectRatio")) == QStringLiteral("4:3"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), aspectOnly(Aspect::Auto)), pIni, gs,
                 QStringLiteral("AspectRatio")) == QStringLiteral("Auto 4:3/3:2"));

        CHECK(ev(configEdits(QStringLiteral("duckstation"), aspectOnly(Aspect::R16_9)), dsIni, QStringLiteral("Display"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("16:9"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), aspectOnly(Aspect::Auto)), dsIni, QStringLiteral("Display"),
                 QStringLiteral("AspectRatio")) == QStringLiteral("Auto (Game Native)"));
    }

    // ==== 3. VSYNC — the boolean spelling per emulator (True/False, 1/0, true/false) ========================
    {
        CHECK(ev(configEdits(QStringLiteral("dolphin"), vsyncOnly(Vsync::Off)), dGfx, QStringLiteral("Hardware"),
                 QStringLiteral("VSync")) == QStringLiteral("False"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), vsyncOnly(Vsync::On)), dGfx, QStringLiteral("Hardware"),
                 QStringLiteral("VSync")) == QStringLiteral("True"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), vsyncOnly(Vsync::Off)), pIni, gs,
                 QStringLiteral("VsyncEnable")) == QStringLiteral("0"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), vsyncOnly(Vsync::On)), pIni, gs,
                 QStringLiteral("VsyncEnable")) == QStringLiteral("1"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), vsyncOnly(Vsync::Off)), dsIni, QStringLiteral("Display"),
                 QStringLiteral("VSync")) == QStringLiteral("false"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), vsyncOnly(Vsync::On)), dsIni, QStringLiteral("Display"),
                 QStringLiteral("VSync")) == QStringLiteral("true"));
    }

    // ==== 4. RENDERER — Dolphin backend strings, PCSX2 GSRendererType ints, DuckStation strings =============
    {
        CHECK(ev(configEdits(QStringLiteral("dolphin"), rendOnly(Renderer::Vulkan)), dIni, QStringLiteral("Core"),
                 QStringLiteral("GFXBackend")) == QStringLiteral("Vulkan"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), rendOnly(Renderer::D3D11)), dIni, QStringLiteral("Core"),
                 QStringLiteral("GFXBackend")) == QStringLiteral("D3D"));
        CHECK(ev(configEdits(QStringLiteral("dolphin"), rendOnly(Renderer::OpenGL)), dIni, QStringLiteral("Core"),
                 QStringLiteral("GFXBackend")) == QStringLiteral("OGL"));

        CHECK(ev(configEdits(QStringLiteral("pcsx2"), rendOnly(Renderer::Vulkan)), pIni, gs,
                 QStringLiteral("Renderer")) == QStringLiteral("14"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), rendOnly(Renderer::OpenGL)), pIni, gs,
                 QStringLiteral("Renderer")) == QStringLiteral("12"));
        CHECK(ev(configEdits(QStringLiteral("pcsx2"), rendOnly(Renderer::D3D11)), pIni, gs,
                 QStringLiteral("Renderer")) == QStringLiteral("3"));

        CHECK(ev(configEdits(QStringLiteral("duckstation"), rendOnly(Renderer::Vulkan)), dsIni, QStringLiteral("GPU"),
                 QStringLiteral("Renderer")) == QStringLiteral("Vulkan"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), rendOnly(Renderer::OpenGL)), dsIni, QStringLiteral("GPU"),
                 QStringLiteral("Renderer")) == QStringLiteral("OpenGL"));
    }

    // ==== 5. MSAA — Dolphin/DuckStation take it; PCSX2 has no MSAA lever =====================================
    {
        CHECK(ev(configEdits(QStringLiteral("dolphin"), msaaOnly(4)), dGfx, QStringLiteral("Settings"),
                 QStringLiteral("MSAA")) == QStringLiteral("4"));
        CHECK(ev(configEdits(QStringLiteral("duckstation"), msaaOnly(4)), dsIni, QStringLiteral("GPU"),
                 QStringLiteral("Multisamples")) == QStringLiteral("4"));
        // Unsupported: PCSX2 MSAA emits nothing (skipped, not an error).
        CHECK(configEdits(QStringLiteral("pcsx2"), msaaOnly(4)).isEmpty());
    }

    // ==== 6. UNSUPPORTED LEVER / EMULATOR yields NO edit (a tripwire: the absence is the behaviour) ==========
    {
        CHECK(configEdits(QStringLiteral("flycast"), aspectOnly(Aspect::R16_9)).isEmpty()); // Flycast: no aspect map
        CHECK(configEdits(QStringLiteral("flycast"), msaaOnly(4)).isEmpty());
        CHECK(configEdits(QStringLiteral("dolphin"), rendOnly(Renderer::Auto)).isEmpty());  // Dolphin: no "auto" backend
        CHECK(configEdits(QStringLiteral("pcsx2"), Settings{}).isEmpty());                  // all-unset -> nothing
        CHECK(configEdits(QStringLiteral("no-such-emulator"), resOnly(4)).isEmpty());       // unknown id -> nothing
    }

    // ==== 7. resolve() — per-game field over per-system default field-by-field ==============================
    {
        Settings sys; sys.resMultiplier = 2; sys.aspect = Aspect::R16_9; sys.renderer = Renderer::OpenGL;
        Settings game; game.resMultiplier = 4;                         // overrides res only
        const Settings r = resolve(game, sys);
        CHECK(r.resMultiplier == 4);                                   // per-game wins
        CHECK(r.aspect == Aspect::R16_9);                             // system default flows through
        CHECK(r.renderer == Renderer::OpenGL);                       // system default flows through
        CHECK(r.vsync == Vsync::Unset);                              // unset in both -> unset

        // Both unset -> unset (don't touch).
        CHECK(resolve(Settings{}, Settings{}).isEmpty());
        // Per-game with nothing set leaves the system default intact.
        CHECK(resolve(Settings{}, sys) == sys);
    }

    // ==== 8. toJson/fromJson round-trip; unset fields omitted ================================================
    {
        Settings s; s.resMultiplier = 4; s.aspect = Aspect::Stretch; s.vsync = Vsync::Off;
        s.renderer = Renderer::D3D12; s.msaa = 2;
        CHECK(fromJson(toJson(s)) == s);
        // A single-lever record only carries that lever's key (omit-empty -> one spelling per record).
        const QJsonObject one = toJson(aspectOnly(Aspect::R4_3));
        CHECK(one.contains(QStringLiteral("aspect")) && one.size() == 1);
        CHECK(toJson(Settings{}).isEmpty());                          // all-unset serializes to {}
    }

    // ==== 9. Store — round-trip by key; absent reads empty; clear is a plain delete (no husk) ================
    {
        const QString key = QStringLiteral("romlib:C:/roms/ps2/Gran Turismo 4.iso");
        Settings s; s.resMultiplier = 3; s.renderer = Renderer::Vulkan; s.vsync = Vsync::Off;
        EmuGfxStore::set(key, s);

        CHECK(EmuGfxStore::get(key) == s);
        CHECK(EmuGfxStore::has(key));
        CHECK(EmuGfxStore::get(QStringLiteral("romlib:C:/roms/ps2/Untouched.iso")).isEmpty());
        CHECK(!EmuGfxStore::has(QStringLiteral("romlib:C:/roms/ps2/Untouched.iso")));

        // Raw leaf under emugfx/items/<md5(key)> — addressed by the independent md5 oracle, carrying the omit-empty
        // JSON (res/renderer/vsync present; aspect/msaa absent).
        QSettings st(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("emugfx/items/") + md5hex(key);
        CHECK(st.contains(leaf));
        const QJsonObject blob = QJsonDocument::fromJson(st.value(leaf).toString().toUtf8()).object();
        CHECK(blob.value(QStringLiteral("res")).toInt() == 3);
        CHECK(blob.value(QStringLiteral("renderer")).toString() == QStringLiteral("vulkan"));
        CHECK(blob.value(QStringLiteral("vsync")).toString() == QStringLiteral("off"));
        CHECK(!blob.contains(QStringLiteral("aspect")));

        // Clear: device-local + unsynced -> a PLAIN row delete, no husk (contrast LaunchOptionsStore).
        EmuGfxStore::reset(key);
        CHECK(EmuGfxStore::get(key).isEmpty());
        CHECK(!EmuGfxStore::has(key));
        QSettings st2(iniPath, QSettings::IniFormat);
        CHECK(!st2.contains(leaf));                                   // the row is gone, not a husk
    }

    // ==== 10. Per-system default reachable via systemKey/systemDefault, and independent of a game override ===
    {
        Settings sysDef; sysDef.resMultiplier = 2; sysDef.aspect = Aspect::R16_9;
        EmuGfxStore::set(EmuGfxStore::systemKey(QStringLiteral("ps2")), sysDef);
        CHECK(EmuGfxStore::systemDefault(QStringLiteral("ps2")) == sysDef);
        // A game with no override resolves to the system default.
        CHECK(resolve(EmuGfxStore::get(QStringLiteral("romlib:C:/roms/ps2/NoOverride.iso")),
                      EmuGfxStore::systemDefault(QStringLiteral("ps2"))) == sysDef);
        // A different system has no default.
        CHECK(EmuGfxStore::systemDefault(QStringLiteral("gc")).isEmpty());
    }

    if (failures == 0) std::printf("EMUSETTINGS-OK\n");
    else               std::fprintf(stderr, "EMUSETTINGS: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
