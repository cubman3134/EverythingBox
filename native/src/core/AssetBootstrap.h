#pragma once
#include <QString>

// First-run asset extraction (subsystem D, Task 2). A fresh install boots into an empty writable data dir
// (AppPaths::dataDir()) — on Android the APK's read-only assets are the only copy of the stock themes and
// first-party addons, so nothing is on disk until we extract it. This is that extractor.
//
// It is a pure function of (sourceRoot, dataDir, appVersion) so it is testable with any source dir (the probe
// hands it a temp dir; the app hands it "assets:/mmv" on Android). QtCore-only, like FormFactor.
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
}
