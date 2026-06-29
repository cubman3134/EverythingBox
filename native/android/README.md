# Android package source (`QT_ANDROID_PACKAGE_SOURCE_DIR`)

This directory overlays files into the generated Android project at build time (via `androiddeployqt`).
It's referenced from `native/CMakeLists.txt` only when configuring for Android, so it has no effect on the
desktop build.

When the Android port is actually wired up (see `../docs/android-port.md`), add here:

- `AndroidManifest.xml` — app id/name/icon, permissions (`INTERNET`, storage/SAF for ROM & media folders),
  `android.hardware.gamepad`, immersive full-screen for video/RetroView. **Android TV:** add a
  `LEANBACK_LAUNCHER` intent-filter + `android:banner` (320×180) so it appears on the TV home screen, and
  declare `<uses-feature android:name="android.hardware.touchscreen" android:required="false"/>` and
  `<uses-feature android:name="android.software.leanback" android:required="false"/>` so it installs on TVs.
- `res/` — launcher icons, themes, strings.
- `src/` — any Java/Kotlin glue (e.g. an `Activity` subclass, or an Intent helper to hand a ROM off to an
  installed Android emulator app as the replacement for the desktop external-emulator launcher).
- `assets/` — bundled addon JS (`addons/aiocatalog`) and themes, copied to the app data dir on first run
  (the executable dir isn't writable on Android; runtime paths go through `AppPaths::dataDir()`).

Until then this is just a placeholder so the configured path exists.
