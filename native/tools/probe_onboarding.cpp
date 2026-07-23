// Headless check of the first-run onboarding router (onboarding/drive-restore T1). The pure decision that the
// choice screen, the inject point, and T2's restore flow all route through is mmv::onboardingRoute — a total
// function over five bools (src/core/OnboardingRoute.h). This pins its table verbatim from the plan so the UI
// layers can never drift from it, and asserts CloudSync::signInAvailable() is desktop-true / Android-false.
//
// No Qt event loop, no display, no widgets — the router is header-only and CloudSync::signInAvailable() is a
// compile-time platform gate, so this links lean (Qt6::Core for CloudSync's TU) and runs anywhere.
//
// Prints ONBOARDING-OK on success; any failure prints ONBOARDING-FAIL <cond> and exits non-zero.
#include "OnboardingRoute.h"
#include "CloudSync.h"

#include <QtGlobal>
#include <cstdio>

using mmv::OnboardingRoute;
using mmv::onboardingRoute;
using mmv::RestorePullStage;
using mmv::restorePullStage;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "ONBOARDING-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    // The router's five inputs: onboardingRoute(hasLocalProfiles, restorePicked, signInOk, remoteHasProfiles,
    // signInAvailable). The choice screen never even shows once onboarding/done is set — that flag short-circuits
    // ahead of the router at the inject point — so the table below is the "not yet onboarded" first-run space.

    // 1. No local profiles, nothing picked yet, first run => show the choice screen.
    CHECK(onboardingRoute(/*hasLocal*/false, /*restore*/false, /*ok*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);

    // 2. Restore picked but sign-in isn't available on this platform (Android) => Decline (caller -> Fresh).
    CHECK(onboardingRoute(false, /*restore*/true, false, false, /*avail*/false) == OnboardingRoute::Decline);
    // Decline is independent of whether the (never-attempted) sign-in "would" have found remote profiles.
    CHECK(onboardingRoute(false, true, false, /*remote*/true, /*avail*/false) == OnboardingRoute::Decline);

    // 3. Restore picked, signed in, but the cloud is empty => Fresh (this device seeds the cloud).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/true, /*remote*/false, /*avail*/true)
          == OnboardingRoute::Fresh);

    // 4. Restore picked, signed in, cloud populated => Picker (pick a restored profile).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/true, /*remote*/true, /*avail*/true)
          == OnboardingRoute::Picker);

    // 5. Restore picked, sign-in FAILED => back to the choice screen (never a dead end; token/refresh kept).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/false, /*remote*/true, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);

    // 5b. T2 semantics: the restore flow feeds the WHOLE auth+pull leg into signInOk (finishOnboardingRestore's
    //     `restoreOk`), so a pull/network failure AFTER a successful sign-in reuses the SAME ChoiceScreen row as a
    //     sign-in failure — no new enum value. The distinction (retry the sign-in vs. retry the pull) is only in
    //     the caller's notice text; the ROUTE is identical (back to the choice screen, token kept, done=false).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok(=auth&&pull)*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);
    //     And the post-sign-in Picker-vs-Fresh IS the router's remoteHasProfiles branch (restored rows vs. seed):
    CHECK(onboardingRoute(false, true, /*ok*/true, /*remote*/true,  true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(false, true, /*ok*/true, /*remote*/false, true) == OnboardingRoute::Fresh);

    // 6. Existing/returning user: local profiles already exist => straight to the picker, no choice screen,
    //    regardless of any restore inputs (the choice screen is first-run only).
    CHECK(onboardingRoute(/*hasLocal*/true, false, false, false, true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(/*hasLocal*/true, true,  true,  false, true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(/*hasLocal*/true, true,  false, false, false) == OnboardingRoute::Picker);

    // 6b. T3 DATA-SAFETY: the signed-in restore's pull-stage classifier over CloudSync::Status'
    //     (reached, listReached, hasRemote). The findFile network-error window is closed here — a FAILED
    //     bundle file-query (listReached==false) must be treated as UNPROVEN-empty and route to Retry (the
    //     choice screen), NEVER to Seed. A Seed would let the exit-push clobber the user's real backup with a
    //     fresh library. This is the exact decision onboardingRestorePull runs, pinned so it can't drift.

    //     The critical case: reached, but the file-query itself errored (hasRemote is therefore false but UNPROVEN).
    //     This must NOT seed a fresh library — it routes to Retry (the choice screen).
    CHECK(restorePullStage(/*reached*/ true, /*listReached*/ false, /*hasRemote*/ false) == RestorePullStage::Retry);
    CHECK(restorePullStage(/*reached*/ true, /*listReached*/ false, /*hasRemote*/ false) != RestorePullStage::Seed);

    //     A PROVEN-empty cloud (query succeeded, no bundle) is the ONLY thing that seeds fresh.
    CHECK(restorePullStage(/*reached*/ true, /*listReached*/ true,  /*hasRemote*/ false) == RestorePullStage::Seed);

    //     Reached AND query succeeded AND a bundle exists -> download + apply it.
    CHECK(restorePullStage(/*reached*/ true, /*listReached*/ true,  /*hasRemote*/ true)  == RestorePullStage::HasBundle);

    //     Folder unreachable at all -> Retry (never a seed), independent of the downstream flags.
    CHECK(restorePullStage(/*reached*/ false, /*listReached*/ false, /*hasRemote*/ false) == RestorePullStage::Retry);
    CHECK(restorePullStage(/*reached*/ false, /*listReached*/ true,  /*hasRemote*/ true)  == RestorePullStage::Retry);

    //     Exhaustive: across every (reached, listReached, hasRemote), Seed occurs iff all-proven-and-empty, so a
    //     query failure (listReached==false) can NEVER reach Seed — the property the whole T3 fix defends.
    for (int m = 0; m < 8; ++m) {
        const bool reached = m & 4, listReached = m & 2, hasRemote = m & 1;
        const RestorePullStage stage = restorePullStage(reached, listReached, hasRemote);
        CHECK((stage == RestorePullStage::Seed) == (reached && listReached && !hasRemote));
        if (!listReached) CHECK(stage == RestorePullStage::Retry); // no proof of emptiness -> never Seed/HasBundle
    }

    // 7. signInAvailable(): true on desktop, false under Q_OS_ANDROID (the codebase platform-gate idiom). The
    //    compile-guard branch is exercised by the pure param above; here we pin the live static matches the
    //    build's platform so the two can't disagree.
#ifdef Q_OS_ANDROID
    CHECK(CloudSync::signInAvailable() == false);
#else
    CHECK(CloudSync::signInAvailable() == true);
#endif

    if (failures == 0) { std::puts("ONBOARDING-OK"); return 0; }
    std::fprintf(stderr, "ONBOARDING: %d check(s) failed\n", failures);
    return 1;
}
