// The ONE place the RetroAchievements hardcore-mode policy lives (issue #94). Hardcore is an opt-in that
// trades the emulator's rewind-the-clock comforts for the site's real achievement prestige (and, later,
// leaderboards): while a hardcore session is active, save states, rewind, fast-forward and cheats are all
// disabled. rc_client enforces most of this internally once hardcore is on; this policy is what the UI reads
// so it can grey the affordances and refuse them with a readable notice rather than let them fail silently.
//
// Pure and header-only (no Qt, no I/O): a single enum + one predicate, so every gate reads the SAME rule and
// probe_hardcore can pin it. A feature that should become blocked (or, rarely, un-blocked) in hardcore is
// added HERE, in one switch — never scattered across the call sites that consult it.
#pragma once

namespace hardcore
{
    // Every emulator affordance whose availability depends on hardcore mode. The first six are the ones the
    // issue calls out as disabled; Screenshot is a genuinely-allowed affordance included so the policy states
    // the ALLOWED side explicitly too (taking a picture never voids a hardcore run) — and so the predicate is
    // not a constant-true function no mutation can distinguish.
    enum class Feature
    {
        SaveState,    // writing a state to a slot (numbered or quick) — a hardcore run must not be rewindable
        LoadState,    // restoring a state — loading someone else's/earlier progress would void the session
        Rewind,       // the rewind ring (hold R / Select+L2)
        FastForward,  // running the core faster than real time (hold Tab / Select+R2)
        Cheats,       // the per-game cheat editor / code list
        CheatSearch,  // scanning RAM to CREATE a cheat (issue #96)
        Screenshot,   // ALLOWED in hardcore — capturing the current frame is harmless
    };

    // True when `f` is forbidden while a hardcore RetroAchievements session is active. Note this speaks only to
    // the POLICY (is this feature disallowed in hardcore at all); whether a hardcore session is actually running
    // is a separate, live question the caller answers (Achievements::hardcoreActive()) before it applies this.
    inline bool forbidsInHardcore(Feature f)
    {
        switch (f)
        {
        case Feature::SaveState:
        case Feature::LoadState:
        case Feature::Rewind:
        case Feature::FastForward:
        case Feature::Cheats:
        case Feature::CheatSearch:
            return true;
        case Feature::Screenshot:
            return false;
        }
        return false; // an unlisted feature is allowed by default (a new one is opted IN above, deliberately)
    }
}
