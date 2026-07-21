# Android package source (`QT_ANDROID_PACKAGE_SOURCE_DIR`)

This directory is the **committed** overlay for the generated Android project (`AndroidManifest.xml`, `res/`,
`src/` glue). It has no effect on the desktop build — `native/CMakeLists.txt` only references it when
configuring for Android.

**It is NOT the directory `androiddeployqt` reads directly.** At configure time the `if(ANDROID)` block copies
this dir verbatim into a build-tree staging root (`<build>/android-package/`), then generates
`assets/mmv/` under it = the stock `themes2/` plus every first-party addon dir under `native/addons/`.
`QT_ANDROID_PACKAGE_SOURCE_DIR` points at that staged copy, so the committed tree is never mutated and the
generated `assets/mmv/` never lands in git. `androiddeployqt` bundles `assets/` into the APK (read-only);
`AssetBootstrap::run("assets:/mmv", AppPaths::dataDir(), <version>)` (called from `main.cpp` on first launch)
extracts them into the writable data dir — stock themes are refreshed on a version bump, user-configured
addons are copy-if-absent (never clobbered). See `AssetBootstrap.{h,cpp}` and `probe_bootstrap.cpp`.

Still to add here as the port is completed (see `../docs/android-port.md`):

- `AndroidManifest.xml` — app id/name/icon, permissions (`INTERNET`, storage/SAF for ROM & media folders),
  `android.hardware.gamepad`, immersive full-screen for video/RetroView. **Android TV:** add a
  `LEANBACK_LAUNCHER` intent-filter + `android:banner` (320×180) so it appears on the TV home screen, and
  declare `<uses-feature android:name="android.hardware.touchscreen" android:required="false"/>` and
  `<uses-feature android:name="android.software.leanback" android:required="false"/>` so it installs on TVs.
- `res/` — launcher icons, themes, strings.
- `src/` — any Java/Kotlin glue (e.g. an `Activity` subclass, or an Intent helper to hand a ROM off to an
  installed Android emulator app as the replacement for the desktop external-emulator launcher).
`assets/mmv/` is **already wired** (this task): CMake stages it and `AssetBootstrap` extracts it on first run —
do NOT add a committed `assets/` dir here, it is generated into the build-tree staging copy.

Until the manifest/res/src glue lands, this dir is a minimal placeholder so the configured path exists.
