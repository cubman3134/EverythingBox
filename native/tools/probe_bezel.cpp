// Headless check of the pure bezel decoration units (src/emu/BezelSelect.h) — issue #106.
//
// Three rails this pins:
//   1. SELECTION PRECEDENCE — game-specific beats system-default beats the two legacy global tiers, and
//      empty tiers drop out of the list rather than producing "/x.png" garbage. This is what makes a
//      per-system decoration pack auto-apply while a user's existing bezels/default.png keeps working.
//   2. INFO-FILE PARSE — a RetroArch/RetroBat .cfg/.info yields the viewport ONLY when all four
//      custom_viewport_* keys are present and sane; anything partial or malformed stays invalid, so the
//      renderer falls back to the flat overlay (the no-regression guarantee) instead of placing the game
//      into a half-specified rect.
//   3. VIEWPORT MAPPING — a viewport in bezel-native pixels maps into a widget with the bezel letterboxed,
//      the game landing exactly in the cutout at the same scale as the art.
//
// BezelSelect.h is header-only, Qt-free and does no I/O, so this probe needs no QCoreApplication and no
// scratch dir (mirrors probe_stateslots). Every expected value is an INDEPENDENT hand-computed oracle,
// never produced by calling the function under test.
//
// Prints BEZEL-OK on success; any failure prints BEZEL-FAIL <cond> (line) and exits non-zero.
#include "BezelSelect.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BEZEL-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    using namespace BezelSelect;

    // ---- 1. Selection precedence --------------------------------------------------------------------
    // Full house: all four tiers, in order. Oracle written out by hand.
    {
        const std::vector<std::string> c = candidates("gb", "Tetris", "gambatte");
        CHECK(c.size() == 4);
        CHECK(c[0] == "gb/Tetris.png");   // game-specific wins
        CHECK(c[1] == "gb/default.png");  // then system default
        CHECK(c[2] == "gambatte.png");    // then legacy global-by-core
        CHECK(c[3] == "default.png");     // then legacy global default
    }
    // No rom name (launched without a title/known base) -> the game-specific tier is absent, the rest stay.
    {
        const std::vector<std::string> c = candidates("gb", "", "gambatte");
        CHECK(c.size() == 3);
        CHECK(c[0] == "gb/default.png");
        CHECK(c[1] == "gambatte.png");
        CHECK(c[2] == "default.png");
    }
    // No system -> only the two legacy global tiers, which is EXACTLY the pre-#106 behaviour. This is the
    // no-regression rail for the selection half.
    {
        const std::vector<std::string> c = candidates("", "Tetris", "gambatte");
        CHECK(c.size() == 2);
        CHECK(c[0] == "gambatte.png");
        CHECK(c[1] == "default.png");
    }
    // Nothing known at all -> just the global default. The list is never empty (there is always a last
    // resort), and it never fabricates a "/x.png" from an empty tier.
    {
        const std::vector<std::string> c = candidates("", "", "");
        CHECK(c.size() == 1);
        CHECK(c[0] == "default.png");
    }

    // ---- 1b. Decoration packs in the precedence (#187) ------------------------------------------------
    // A pack installs as bezels/<system>/<packId>/…, so it enters the SAME two per-system tiers rather than
    // getting tiers of its own — and it never outranks the loose file beside it, which is the file the user
    // put there by hand. Oracle written out in full.
    {
        const std::vector<std::string> c = candidates("snes", "Chrono Trigger", "snes9x", { "shells" });
        CHECK(c.size() == 6);
        CHECK(c[0] == "snes/Chrono Trigger.png");
        CHECK(c[1] == "snes/shells/Chrono Trigger.png");
        CHECK(c[2] == "snes/default.png");
        CHECK(c[3] == "snes/shells/default.png");
        CHECK(c[4] == "snes9x.png");
        CHECK(c[5] == "default.png");
    }
    // Two packs: the caller's ORDER decides which wins, and it is preserved verbatim at both tiers. The
    // caller sorts (DecorationPack::packsForSystem), so this is what makes the winner stable across launches
    // instead of being whatever the filesystem enumerated first.
    {
        const std::vector<std::string> c = candidates("nes", "Metroid", "", { "aaa", "bbb" });
        CHECK(c.size() == 7);
        CHECK(c[0] == "nes/Metroid.png");
        CHECK(c[1] == "nes/aaa/Metroid.png");
        CHECK(c[2] == "nes/bbb/Metroid.png");
        CHECK(c[3] == "nes/default.png");
        CHECK(c[4] == "nes/aaa/default.png");
        CHECK(c[5] == "nes/bbb/default.png");
        CHECK(c[6] == "default.png");
    }
    // No packs installed -> byte-identical to the pre-#187 list. This is the no-regression rail for the
    // feature: the default argument means every existing call site is unchanged, and so is this one.
    {
        CHECK(candidates("gb", "Tetris", "gambatte", {}) == candidates("gb", "Tetris", "gambatte"));
    }
    // A pack cannot apply without a system — its art is filed under one by definition — so the pack tiers
    // drop out entirely rather than fabricating "/shells/Tetris.png" from an empty system.
    {
        const std::vector<std::string> c = candidates("", "Tetris", "gambatte", { "shells" });
        CHECK(c.size() == 2);
        CHECK(c[0] == "gambatte.png");
        CHECK(c[1] == "default.png");
    }
    // An empty pack id is skipped rather than producing "snes//default.png", which on Windows resolves to
    // the per-system file and would silently give a nameless pack the loose file's art.
    {
        const std::vector<std::string> c = candidates("snes", "", "", { "", "shells" });
        CHECK(c.size() == 3);
        CHECK(c[0] == "snes/default.png");
        CHECK(c[1] == "snes/shells/default.png");
        CHECK(c[2] == "default.png");
    }

    // ---- 2. Info-file parse --------------------------------------------------------------------------
    // A real-shaped RetroArch .cfg: extra keys, comments, quotes, CRLF, out-of-order. Oracle by hand.
    {
        const std::string cfg =
            "# Game Boy shell bezel\r\n"
            "overlays = 1\r\n"
            "overlay0_overlay = \"gb.png\"\r\n"
            "custom_viewport_height = 1120  ; the screen\r\n"
            "custom_viewport_width  = \"1494\"\r\n"
            "custom_viewport_x = 213\r\n"
            "custom_viewport_y = 24\r\n";
        const Viewport vp = parseViewport(cfg);
        CHECK(vp.valid);
        CHECK(vp.x == 213);
        CHECK(vp.y == 24);
        CHECK(vp.w == 1494);
        CHECK(vp.h == 1120);
    }
    // Missing one of the four -> invalid. A half-specified viewport must NOT engage the viewport path.
    {
        const std::string cfg =
            "custom_viewport_width = 1494\n"
            "custom_viewport_height = 1120\n"
            "custom_viewport_x = 213\n";           // no _y
        const Viewport vp = parseViewport(cfg);
        CHECK(!vp.valid);
    }
    // Present but insane (zero width) -> invalid.
    {
        const Viewport vp = parseViewport(
            "custom_viewport_width=0\ncustom_viewport_height=100\ncustom_viewport_x=0\ncustom_viewport_y=0\n");
        CHECK(!vp.valid);
    }
    // Non-numeric value ("auto") does not silently read as 0; the whole rect stays invalid.
    {
        const Viewport vp = parseViewport(
            "custom_viewport_width=auto\ncustom_viewport_height=100\ncustom_viewport_x=0\ncustom_viewport_y=0\n");
        CHECK(!vp.valid);
    }
    // The digit guard is load-bearing exactly where 0 is a LEGAL value: a garbage x with everything else
    // valid must stay invalid. If toInt silently read "off" as 0 the rect would pass (0 >= 0) and the
    // viewport path would engage on a file that never specified x — this case is what kills that mutation.
    {
        const Viewport vp = parseViewport(
            "custom_viewport_width=320\ncustom_viewport_height=240\ncustom_viewport_x=off\ncustom_viewport_y=0\n");
        CHECK(!vp.valid);
    }
    // Case-insensitive keys, x=0/y=0 is a legitimate corner (>=0, not >0).
    {
        const Viewport vp = parseViewport(
            "CUSTOM_VIEWPORT_WIDTH=320\nCustom_Viewport_Height=240\ncustom_viewport_x=0\ncustom_viewport_y=0\n");
        CHECK(vp.valid);
        CHECK(vp.x == 0 && vp.y == 0 && vp.w == 320 && vp.h == 240);
    }
    // Empty text -> invalid (no info file case reaches here as "").
    CHECK(!parseViewport("").valid);

    // ---- 3. Viewport mapping -------------------------------------------------------------------------
    // Exact-fit widget: widget == bezel size, scale 1.0, no letterbox. Game rect == the viewport verbatim.
    {
        Viewport vp; vp.x = 213; vp.y = 24; vp.w = 1494; vp.h = 1120; vp.valid = true;
        const Mapped m = mapViewport(1920, 1080, 1920, 1080, vp);
        CHECK(m.bx == 0 && m.by == 0 && m.bw == 1920 && m.bh == 1080);
        CHECK(m.gx == 213 && m.gy == 24 && m.gw == 1494 && m.gh == 1120);
    }
    // Half-size widget with matching aspect: scale 0.5 uniformly. Oracle: every dimension halved (lround).
    {
        Viewport vp; vp.x = 200; vp.y = 20; vp.w = 1000; vp.h = 800; vp.valid = true;
        const Mapped m = mapViewport(1920, 1080, 960, 540, vp);
        CHECK(m.bx == 0 && m.by == 0 && m.bw == 960 && m.bh == 540);
        CHECK(m.gx == 100 && m.gy == 10 && m.gw == 500 && m.gh == 400);
    }
    // Pillarbox: a 100x100 bezel into a 400x200 widget. scale = min(400/100,200/100)=2.0, bezel 200x200,
    // centred x -> bx=(400-200)/2=100, by=0. Viewport (10,20,40,50) -> g=(100+20, 0+40, 80, 100).
    {
        Viewport vp; vp.x = 10; vp.y = 20; vp.w = 40; vp.h = 50; vp.valid = true;
        const Mapped m = mapViewport(100, 100, 400, 200, vp);
        CHECK(m.bx == 100 && m.by == 0 && m.bw == 200 && m.bh == 200);
        CHECK(m.gx == 120 && m.gy == 40 && m.gw == 80 && m.gh == 100);
    }
    // Degenerate inputs return a zeroed Mapped rather than dividing by zero.
    {
        Viewport vp; vp.x = 0; vp.y = 0; vp.w = 10; vp.h = 10; vp.valid = true;
        const Mapped m = mapViewport(0, 100, 400, 200, vp);
        CHECK(m.bw == 0 && m.bh == 0 && m.gw == 0 && m.gh == 0);
    }

    if (failures == 0) { std::printf("BEZEL-OK\n"); return 0; }
    std::fprintf(stderr, "BEZEL-FAIL %d checks\n", failures);
    return 1;
}
