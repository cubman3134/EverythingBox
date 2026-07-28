#pragma once
#include <QString>

// First-run asset extraction (subsystem D, Task 2). A fresh install boots into an empty writable data dir
// (AppPaths::dataDir()) — on Android the APK's read-only assets are the only copy of the stock themes and
// first-party addons, so nothing is on disk until we extract it. This is that extractor.
//
// It is a pure function of (sourceRoot, dataDir, appVersion) so it is testable with any source dir (the probe
// hands it a temp dir; the app hands it "assets:/eb" on Android). QtCore-only, like FormFactor.
//
// Semantics (probe_bootstrap pins these verbatim):
//   * A stamp file `dataDir/.assets-version` records the version whose assets were last extracted.
//   * stamp == appVersion            -> full no-op (nothing copied, mtimes preserved).
//   * fresh (no stamp) OR bumped ver -> themes2 STOCK is REFRESHED (source themes overwrite same-named files);
//                                       addons are COPY-IF-ABSENT (an addon dir already on disk is never
//                                       clobbered — user-configured addons survive an upgrade); stamp rewritten.
//   * user-added theme dirs (present on disk, absent in source) are never touched by a refresh.
//   * sourceRoot does not exist      -> clean no-op (no dirs created, no stamp written).
//
// Returns true iff it extracted/refreshed anything (i.e. wrote the stamp); false on a no-op.
namespace AssetBootstrap
{
    bool run(const QString& sourceRoot, const QString& dataDir, const QString& appVersion);

    // Finish the themes2/XMB -> themes2/Triple rename ON DISK (roadmap #57). run() is additive by design (it
    // never deletes a theme dir, so user themes survive an upgrade), which means an upgraded install keeps the
    // retired themes2/XMB folder beside the freshly-extracted themes2/Triple. XMB/theme.json already declared
    // "name": "Triple", so ThemeEngine::availableThemes() returns TWO folders whose display name is identical
    // and the picker offers "Triple" twice — the stale row storing a folder no fresh device will ever have.
    //
    // This is completing a rename, not deleting a user's theme, and the guard is what makes that true: it
    // removes ThemeChoice::kRenamedFrom ONLY when ThemeChoice::kFallbackTheme is genuinely installed (a
    // theme.json on disk). If Triple is absent, XMB is the user's only copy of that theme and is left alone.
    //
    // Returns true iff it removed the stale folder.
    bool retireRenamedTheme(const QString& dataDir);
}
