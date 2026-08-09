// probe_overrides — issue #95, per-game core-option and input-remap overrides.
//
// Two things are proven here, and the second is the point of the whole feature:
//   1. OverrideLayer, the pure baseline+delta layering, gets the precedence and the no-leak store rule right.
//   2. The Settings store that persists deltas keeps them in a SEPARATE keyspace, so a game-scoped override
//      never mutates the per-core baseline (opt/*) or the global/per-system binding, and the next game on the
//      same core inherits the baseline, not the previous game's delta. That is issue #95's #1 correctness rail.
//
// Fixtures are hand-built maps with hand-computed expected results — never derived by running the function
// under test — so an assertion cannot be a fixed point of the code it guards. The probe's data dir is its own
// per-process scratch (EB_ISOLATED_DATA_DIR), so Settings writes touch nothing real.
#include "../src/core/OverrideLayer.h"
#include "../src/core/Settings.h"
#include "../src/libretro/libretro.h" // RETRO_DEVICE_ID_JOYPAD_*
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

using Map = OverrideLayer::Map;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. Pure layering: effective() precedence --------------------------------------------------------
    {
        const Map baseline{ {"a", "1"}, {"b", "2"}, {"c", "3"} };
        const Map d1{ {"b", "20"} };
        const Map d2{ {"c", "30"}, {"d", "40"} };

        const Map eff = OverrideLayer::effective(baseline, { d1, d2 });
        // Hand-computed: a untouched (1), b from d1 (20), c from d2 (30), d added by d2 (40).
        check(eff.value("a") == "1", "effective keeps an untouched baseline key");
        check(eff.value("b") == "20", "effective applies a lower layer's override");
        check(eff.value("c") == "30", "effective applies a higher layer's override");
        check(eff.value("d") == "40", "effective adds a key present only in a layer");
        check(eff.size() == 4, "effective adds exactly the one new key");

        // Later layer wins when two layers touch the same key.
        const Map lo{ {"c", "99"} }, hi{ {"c", "30"} };
        check(OverrideLayer::effective(baseline, { lo, hi }).value("c") == "30",
              "effective: the later (higher-precedence) layer wins a conflict");

        // A game with no delta sees the baseline exactly — the property the next game on a core relies on.
        check(OverrideLayer::effective(baseline, Map{}) == baseline,
              "effective with an empty delta is the baseline unchanged");
    }

    // ---- 2. Pure layering: the no-leak store rules (normalizeDelta / withKey) -----------------------------
    {
        const Map baseline{ {"a", "1"}, {"b", "2"}, {"c", "3"} };
        // desired sets a back to its baseline (should be DROPPED), b to a new value (KEPT), e that the
        // baseline never had (KEPT as an override away from an unstated default).
        const Map desired{ {"a", "1"}, {"b", "20"}, {"e", "5"} };
        const Map norm = OverrideLayer::normalizeDelta(baseline, desired);
        check(!norm.contains("a"), "normalizeDelta drops a key equal to the baseline (the no-leak rail)");
        check(norm.value("b") == "20", "normalizeDelta keeps a key that differs from the baseline");
        check(norm.value("e") == "5", "normalizeDelta keeps a key absent from the baseline");
        check(norm.size() == 2, "normalizeDelta keeps exactly the two genuine overrides");

        // withKey: a differing value is stored; a value equal to the baseline ERASES the row (per-row reset).
        const Map added = OverrideLayer::withKey(Map{}, "b", "2", "20");
        check(added.value("b") == "20" && added.size() == 1, "withKey stores an override that differs");
        const Map reset = OverrideLayer::withKey(added, "b", "2", "2");
        check(!reset.contains("b"), "withKey erases the row when the value returns to the baseline (reset)");
    }

    // ---- 3. Store: per-game CORE-OPTION delta never touches the per-core baseline -------------------------
    {
        const char* core = "mgba";
        const char* key  = "mgba_gb_model";
        Settings::setOptionValue(core, key, "gbc");                 // per-core baseline
        Settings::setOptionValue(core, "mgba_skip_bios", "OFF");    // a second baseline key, no game override

        const QString tokA = Settings::gameToken("addon:gameA");
        const QString tokB = Settings::gameToken("addon:gameB");
        check(!tokA.isEmpty() && tokA != tokB, "gameToken is non-empty and distinguishes two games");

        Settings::setGameOptionValue(tokA, core, key, "sgb");       // game A overrides the model
        // The baseline is UNTOUCHED — the whole rail. A leak would have rewritten opt/<core>/<key>.
        check(Settings::optionValue(core, key) == "gbc",
              "a game-scoped core-option delta does not mutate the per-core baseline");
        check(Settings::gameOptionValue(tokA, core, key) == "sgb", "the game delta round-trips");
        check(Settings::gameHasOption(tokA, core, key), "gameHasOption reports the override present");

        const Map deltaA = Settings::gameOptionDelta(tokA, core);
        check(deltaA.size() == 1 && deltaA.value(key) == "sgb", "gameOptionDelta returns exactly the override");

        // Game B, launched on the SAME core, has no delta — it must see the baseline, not A's override.
        check(Settings::gameOptionDelta(tokB, core).isEmpty(), "a different game inherits an empty delta");

        // Effective sets, composed through the pure layer, are what each game actually runs.
        const Map baseline{ {key, "gbc"}, {"mgba_skip_bios", "OFF"} };
        const Map effA = OverrideLayer::effective(baseline, deltaA);
        check(effA.value(key) == "sgb" && effA.value("mgba_skip_bios") == "OFF",
              "game A's effective options are baseline + its delta");
        const Map effB = OverrideLayer::effective(baseline, Settings::gameOptionDelta(tokB, core));
        check(effB == baseline, "game B's effective options are the untouched baseline");

        // A reset (clear) removes the override and leaves the baseline intact.
        Settings::clearGameOptionValue(tokA, core, key);
        check(!Settings::gameHasOption(tokA, core, key), "clearGameOptionValue removes the override");
        check(Settings::optionValue(core, key) == "gbc",
              "the per-core baseline is still intact after a game override is cleared");
    }

    // ---- 4. Store: per-game INPUT binding layering (game -> system -> global -> default) ------------------
    {
        const int port = 1;                                  // player 2's oddball mapping is game-scoped too
        const int rid  = RETRO_DEVICE_ID_JOYPAD_A;
        const int DEF  = 7;

        Settings::setInputScope(QString());  Settings::setPadBinding(port, rid, 100);   // global
        Settings::setInputScope("snes");     Settings::setPadBinding(port, rid, 200);   // per-system

        const QString tok = Settings::gameToken("addon:gameA");
        Settings::setGamePadBinding(tok, port, rid, 300);                               // per-game

        // Gameplay resolution: system scope active AND the game layer active (as RetroView sets at launch).
        Settings::setInputScope("snes");  Settings::setInputGameScope(tok);
        check(Settings::padBinding(port, rid, DEF) == 300, "the per-game binding wins over system and global");

        Settings::clearGamePadBinding(tok, port, rid);
        check(Settings::padBinding(port, rid, DEF) == 200, "clearing the game binding falls through to system");

        Settings::setInputScope(QString());  // no system scope now
        check(Settings::padBinding(port, rid, DEF) == 100, "with no game or system binding, the global wins");

        const int freshRid = RETRO_DEVICE_ID_JOYPAD_X;       // nothing ever bound here
        check(Settings::padBinding(port, freshRid, DEF) == DEF, "an unbound button returns the hard default");

        // No-leak for input: the game write above never touched the global or per-system stores. Re-set the
        // game binding, then read each lower layer with the game layer OFF — both must be their own values.
        Settings::setGamePadBinding(tok, port, rid, 300);
        Settings::setInputGameScope(QString());              // game layer off
        Settings::setInputScope(QString());
        check(Settings::padBinding(port, rid, DEF) == 100, "the global binding is unchanged by a game write");
        Settings::setInputScope("snes");
        check(Settings::padBinding(port, rid, DEF) == 200, "the per-system binding is unchanged by a game write");
    }

    if (g_failures == 0) { std::printf("OVERRIDES-OK\n"); return 0; }
    std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
    return 1;
}
