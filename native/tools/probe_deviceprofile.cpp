// Headless check of the device-profile classifier + tuned defaults table (src/core/DeviceProfile.h, issue #119).
// PURE — identify() and defaultsFor() touch no hardware and no disk — so it runs under the offscreen QPA in CI.
// Prints DEVICEPROFILE-OK on success; any failure prints DEVICEPROFILE-FAIL <cond> (line) and exits non-zero.
//
// WHAT IT PINS:
//   * each KNOWN x86 DMI product string maps to the RIGHT Kind (Jupiter->Deck LCD, Galileo->Deck OLED, the ROG
//     Ally RC71L/RC72LA + "ROG Ally", the Legion Go 83E1), case- and extra-token-insensitive;
//   * an UNRECOGNISED DMI string does NOT get a wrong handheld guess — it falls through to a capability TIER;
//   * the capability triple buckets to Low/Mid/High at the DOCUMENTED thresholds (software renderer -> Low; RAM
//     >=16 AND cores>=8 -> High; RAM<8 OR cores<=4 -> Low; otherwise Mid), including the empty/unknown case;
//   * Android ro.product is Unknown in v1 (never a wrong guess), and a DMI handheld still wins over any capability;
//   * displayName/isHandheld are right per Kind (a generic tier is NOT a handheld);
//   * kindToken/kindFromToken round-trip and an unknown token is Unknown;
//   * defaultsFor returns the TUNED block for the handhelds (Deck PS2 at 2x, Ally at 3x, renderer Vulkan, vsync
//     On), a resolution-only cap for the generic Low/Mid tiers, and an ALL-UNSET no-op for High / Unknown and
//     for an emulator the table has no opinion on;
//   * the THREE-LAYER precedence via EmuGfx::resolve: per-game beats per-system beats per-device beats unset.
//
// FIXTURES ARE HAND-AUTHORED, INDEPENDENT OF THE CODE UNDER TEST: every expected Kind, threshold, multiplier and
// resolved field is written here by hand from the issue's spec and each vendor's published codename — never
// produced by running the function under test — so an assertion cannot pass merely because it re-ran that
// function.
#include "DeviceProfile.h"
#include "EmuSettings.h"

#include <QCoreApplication>
#include <cstdio>

using namespace DeviceProfile;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "DEVICEPROFILE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A capability triple, hand-built so each tier assertion isolates the inputs it means to.
static Capability cap(const QString& gpu, int cores, double ram)
{ Capability c; c.gpuRenderer = gpu; c.logicalCores = cores; c.ramGB = ram; return c; }

// identify() with no capability signal at all — so a DMI/Android assertion sees ONLY the string path (an
// all-zero capability would itself resolve to GenericLow, which must NOT mask a positive DMI match).
static Kind idDmi(const QString& dmi)
{ return identify(dmi, QString(), cap(QString(), 0, 0.0)).kind; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- known x86 handhelds: exact and with vendor's extra tokens / casing --------------------------------
    CHECK(fromDmiProduct(QStringLiteral("Jupiter")) == Kind::SteamDeckLCD);
    CHECK(fromDmiProduct(QStringLiteral("Galileo")) == Kind::SteamDeckOLED);
    CHECK(fromDmiProduct(QStringLiteral("ROG Ally RC71L_RC71L")) == Kind::RogAlly);   // Asus product name w/ tokens
    CHECK(fromDmiProduct(QStringLiteral("RC72LA")) == Kind::RogAlly);                 // Ally X board code
    CHECK(fromDmiProduct(QStringLiteral("83E1")) == Kind::LegionGo);                  // Lenovo Legion Go model no.
    CHECK(fromDmiProduct(QStringLiteral("Legion Go")) == Kind::LegionGo);            // Lenovo friendly name
    CHECK(fromDmiProduct(QStringLiteral("jupiter")) == Kind::SteamDeckLCD);          // case-insensitive

    // identify() lets a DMI handheld WIN over the capability path (a strong desktop cap must not override a Deck).
    CHECK(identify(QStringLiteral("Jupiter"), QString(), cap(QStringLiteral("AMD Radeon"), 16, 32.0)).kind
          == Kind::SteamDeckLCD);
    CHECK(idDmi(QStringLiteral("Galileo")) == Kind::SteamDeckOLED);

    // ---- an UNRECOGNISED machine falls to a TIER, never a wrong handheld -----------------------------------
    CHECK(fromDmiProduct(QStringLiteral("To Be Filled By O.E.M.")) == Kind::Unknown);
    CHECK(fromDmiProduct(QString()) == Kind::Unknown);
    // With a desktop-class capability, that unknown DMI resolves to GenericHigh (a tier), not a handheld.
    CHECK(identify(QStringLiteral("Some Random Desktop"), QString(), cap(QStringLiteral("GeForce RTX 4080"), 24, 64.0)).kind
          == Kind::GenericHigh);
    CHECK(isHandheld(identify(QStringLiteral("Some Random Desktop"), QString(),
                              cap(QStringLiteral("GeForce RTX 4080"), 24, 64.0)).kind) == false);

    // ---- Android ro.product is Unknown in v1 (never a wrong guess) -----------------------------------------
    CHECK(fromAndroidProduct(QStringLiteral("aya_neo")) == Kind::Unknown);
    // A DMI-less machine with a mid capability + an unrecognised android string -> the tier, not a guess.
    CHECK(identify(QString(), QStringLiteral("retroid_pocket"), cap(QStringLiteral("Adreno"), 8, 12.0)).kind
          == Kind::GenericMid);

    // ---- capability tiers at the DOCUMENTED thresholds -----------------------------------------------------
    // software renderer -> Low, whatever the CPU/RAM.
    CHECK(tierFromCapability(cap(QStringLiteral("llvmpipe (LLVM 15)"), 32, 64.0)) == Kind::GenericLow);
    CHECK(tierFromCapability(cap(QStringLiteral("Microsoft Basic Render Driver"), 32, 64.0)) == Kind::GenericLow);
    CHECK(isSoftwareRenderer(QStringLiteral("SwiftShader Device")) == true);
    CHECK(isSoftwareRenderer(QStringLiteral("AMD Radeon RX 6600")) == false);
    // High: RAM>=16 AND cores>=8.
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon RX 6600"), 8, 16.0)) == Kind::GenericHigh);   // exact boundary
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon RX 6600"), 12, 32.0)) == Kind::GenericHigh);
    // Just under High on either axis -> Mid (not High, not Low): RAM in [8,16) with cores>=5.
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon"), 6, 16.0)) == Kind::GenericMid);            // cores 6 (5..7 band), RAM ok
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon"), 8, 12.0)) == Kind::GenericMid);            // 8 cores but RAM 12<16 -> Mid
    // Low: RAM<8 OR cores<=4.
    CHECK(tierFromCapability(cap(QStringLiteral("Intel UHD"), 4, 16.0)) == Kind::GenericLow);         // cores 4 -> Low
    CHECK(tierFromCapability(cap(QStringLiteral("Intel UHD"), 12, 6.0)) == Kind::GenericLow);         // RAM 6<8 -> Low
    // Mid: the band between — RAM in [8,16), cores in [5,7], no software renderer.
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon Vega"), 6, 8.0)) == Kind::GenericMid);
    CHECK(tierFromCapability(cap(QStringLiteral("Radeon Vega"), 7, 12.0)) == Kind::GenericMid);
    // Empty/unknown capability -> Low (0 cores <=4, 0 RAM <8): the safe direction.
    CHECK(tierFromCapability(cap(QString(), 0, 0.0)) == Kind::GenericLow);

    // ---- displayName / isHandheld -------------------------------------------------------------------------
    CHECK(isHandheld(Kind::SteamDeckLCD) == true);
    CHECK(isHandheld(Kind::SteamDeckOLED) == true);
    CHECK(isHandheld(Kind::RogAlly) == true);
    CHECK(isHandheld(Kind::LegionGo) == true);
    CHECK(isHandheld(Kind::GenericLow) == false);
    CHECK(isHandheld(Kind::GenericHigh) == false);
    CHECK(isHandheld(Kind::Unknown) == false);
    CHECK(displayName(Kind::SteamDeckOLED) == QStringLiteral("Steam Deck (OLED)"));
    CHECK(displayName(Kind::Unknown) == QStringLiteral("Unknown device"));

    // ---- token round-trip ---------------------------------------------------------------------------------
    const Kind allKinds[] = { Kind::Unknown, Kind::SteamDeckLCD, Kind::SteamDeckOLED, Kind::RogAlly,
                              Kind::LegionGo, Kind::GenericLow, Kind::GenericMid, Kind::GenericHigh };
    for (Kind k : allKinds) CHECK(kindFromToken(kindToken(k)) == k);
    CHECK(kindFromToken(QStringLiteral("no-such-profile")) == Kind::Unknown);
    CHECK(kindToken(Kind::RogAlly) == QStringLiteral("rog-ally"));

    // ---- defaultsFor: the tuned table (hand-authored expected multipliers) ---------------------------------
    auto prof = [](Kind k){ Profile p; p.kind = k; p.displayName = displayName(k); p.isHandheld = isHandheld(k); return p; };

    // A Deck gets PS2 at 2x (NOT 3x), GC at 2x, PS1 at 4x, DC at 2x, on Vulkan with vsync On.
    {
        const EmuGfx::Settings ps2 = defaultsFor(prof(Kind::SteamDeckLCD), QStringLiteral("pcsx2"));
        CHECK(ps2.resMultiplier == 2);
        CHECK(ps2.renderer == EmuGfx::Renderer::Vulkan);
        CHECK(ps2.vsync == EmuGfx::Vsync::On);
        CHECK(defaultsFor(prof(Kind::SteamDeckOLED), QStringLiteral("dolphin")).resMultiplier == 2);
        CHECK(defaultsFor(prof(Kind::SteamDeckLCD), QStringLiteral("duckstation")).resMultiplier == 4);
        CHECK(defaultsFor(prof(Kind::SteamDeckLCD), QStringLiteral("flycast")).resMultiplier == 2);
    }
    // An Ally / Legion Go steps up: PS2 at 3x, GC at 3x, PS1 at 5x.
    {
        const EmuGfx::Settings ps2 = defaultsFor(prof(Kind::RogAlly), QStringLiteral("pcsx2"));
        CHECK(ps2.resMultiplier == 3);
        CHECK(ps2.renderer == EmuGfx::Renderer::Vulkan);
        CHECK(defaultsFor(prof(Kind::LegionGo), QStringLiteral("dolphin")).resMultiplier == 3);
        CHECK(defaultsFor(prof(Kind::RogAlly), QStringLiteral("duckstation")).resMultiplier == 5);
    }
    // Generic Low/Mid cap resolution but leave the renderer alone (unset).
    {
        const EmuGfx::Settings low = defaultsFor(prof(Kind::GenericLow), QStringLiteral("pcsx2"));
        CHECK(low.resMultiplier == 2);
        CHECK(low.renderer == EmuGfx::Renderer::Unset);
        CHECK(low.vsync == EmuGfx::Vsync::Unset);
        CHECK(defaultsFor(prof(Kind::GenericMid), QStringLiteral("pcsx2")).resMultiplier == 3);
        CHECK(defaultsFor(prof(Kind::GenericMid), QStringLiteral("dolphin")).renderer == EmuGfx::Renderer::Unset);
    }
    // High + Unknown: ALL-UNSET no-op (the emulator/user value stands — regression-free).
    CHECK(defaultsFor(prof(Kind::GenericHigh), QStringLiteral("pcsx2")).isEmpty());
    CHECK(defaultsFor(prof(Kind::Unknown), QStringLiteral("dolphin")).isEmpty());
    // An emulator the table has no opinion on -> all-unset even on a handheld.
    CHECK(defaultsFor(prof(Kind::SteamDeckLCD), QStringLiteral("cemu")).isEmpty());
    CHECK(defaultsFor(prof(Kind::RogAlly), QStringLiteral("rpcs3")).isEmpty());

    // ---- THREE-LAYER precedence: per-game > per-system > per-device > unset ---------------------------------
    // Hand-built layers, each setting a DIFFERENT lever plus a shared one, so the winner of each is unambiguous.
    {
        EmuGfx::Settings device;  device.resMultiplier = 2; device.renderer = EmuGfx::Renderer::Vulkan; device.vsync = EmuGfx::Vsync::On;
        EmuGfx::Settings system;  system.resMultiplier = 4; system.aspect   = EmuGfx::Aspect::R16_9;    // overrides device res
        EmuGfx::Settings game;    game.resMultiplier   = 6;                                             // overrides both res
        // The chain GameLauncher applies:
        const EmuGfx::Settings r = EmuGfx::resolve(game, EmuGfx::resolve(system, device));
        CHECK(r.resMultiplier == 6);                         // per-game wins the contested lever
        CHECK(r.aspect == EmuGfx::Aspect::R16_9);            // per-system fills a lever the game left unset
        CHECK(r.renderer == EmuGfx::Renderer::Vulkan);       // per-device fills a lever neither game nor system set
        CHECK(r.vsync == EmuGfx::Vsync::On);                 // per-device survives to the bottom
        CHECK(r.msaa == 0);                                  // unset in all three -> stays unset (no edit)

        // per-system beats per-device on the SAME lever, independent of the game layer.
        const EmuGfx::Settings sd = EmuGfx::resolve(system, device);
        CHECK(sd.resMultiplier == 4);                        // system's 4 beats device's 2
        CHECK(sd.renderer == EmuGfx::Renderer::Vulkan);      // device fills what system left unset

        // An all-unset device layer is a pure no-op: resolve(perGame, resolve(perSystem, {})) == resolve(perGame, perSystem).
        const EmuGfx::Settings noDevice = EmuGfx::resolve(game, EmuGfx::resolve(system, EmuGfx::Settings{}));
        const EmuGfx::Settings twoLayer = EmuGfx::resolve(game, system);
        CHECK(noDevice == twoLayer);
    }

    if (failures == 0) std::printf("DEVICEPROFILE-OK\n");
    else               std::fprintf(stderr, "DEVICEPROFILE had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
