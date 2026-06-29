# Android port plan — My Media Vault

Status: **planning**. Android is feasible (unlike iOS) — it allows JIT and `dlopen`, Qt Widgets runs,
and our deps (libmpv, SDL2) build for Android. The work is mostly **toolchain + native-dependency
cross-compilation**, plus reworking the parts that assume a desktop OS.

This is buildable **from Windows** (Qt for Android + the NDK run on Windows). No Mac required.

---

## What ports, what doesn't

| Subsystem | Android | Notes |
|---|---|---|
| Media hub (Allarr catalog, Network, comics/books/PDF readers) | ✅ Ports | Qt Network/Pdf/Concurrent are cross-platform |
| libmpv video/audio | ✅ Ports | Needs libmpv cross-compiled for Android arm64 (see Phase 2) |
| In-process libretro cores | ✅ Ports | Android allows JIT + `dlopen`. `CoreManager` already fetches the Android core (`android/.../arm64-v8a/<core>_libretro_android.so`) ✔ |
| SDL2 gamepad | ✅ Ports | SDL2 has first-class Android support |
| UI (Qt Widgets) | ⚠️ Runs, touch-awkward | Widgets render on Android but are desktop-paradigm. Our arrow/remote nav helps; needs touch sizing/scroll tuning |
| **External-emulator launcher** (Dolphin/PCSX2/… via `QProcess`) | ❌ Does **not** port | Android can't spawn downloaded desktop executables. Best case: Intent-launch a *separately-installed* Android emulator app (no auto-download, no monitoring) |
| `EmulatorManager` install (download .7z/.dmg, extract via bsdtar/hdiutil/flatpak) | ❌ Desktop-only | No `tar`/`hdiutil`/`flatpak` on Android; and nothing to launch anyway |
| CloudSync (Google Drive zip) | ✅ Ports (HTTP) | Path/storage locations differ (scoped storage) |

**Bottom line:** the Android build is a **media hub + interpreter/JIT-class in-process emulators**
(NES → PSX/N64/DS-ish, whatever has an Android libretro core). The standalone modern-console emulators
(Dolphin, Cemu, Ryujinx, RPCS3, PCSX2, …) **do not come along**; they'd be replaced by optional
Intent hand-off to installed Android emulator apps.

---

## Phase 0 — Decisions (do first)
- [ ] Confirm scope: media hub + in-process cores only (recommended), vs. also Intent hand-off to
      installed Android emulators.
- [ ] Min SDK / target SDK (target API 34+ for any Play presence; but see Distribution).
- [ ] ABIs: `arm64-v8a` only (recommended) or also `armeabi-v7a`/`x86_64` (emulator testing).
- [ ] Distribution channel: **sideload APK / F-Droid** (required if we download+`dlopen` cores) vs.
      **Play Store** (then cores must be **bundled in the APK**, not downloaded — Play forbids fetching
      executable code).

## Phase 1 — Toolchain
- [ ] Install **Android Studio** (or just the commandline tools) → SDK Platform 34, Build-Tools,
      Platform-Tools (adb), and **NDK** (r26+).
- [ ] Install **Qt for Android 6.8.3** via the Qt Maintenance Tool — the `android_arm64_v8a`
      (and optionally `android_armv7`, `android_x86_64`) kits. JDK 17.
- [ ] Set `ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT`, `JAVA_HOME`.
- [ ] Smoke-test: build a trivial Qt-for-Android "hello world" to an emulator/device with
      `qt-cmake` + `androiddeployqt`.

## Phase 2 — Native dependencies (the heavy lift)
- [ ] **libmpv for Android arm64** — the real gate. Either:
  - build via mpv's `mpv-android` recipe (builds ffmpeg + libmpv with the NDK), then consume
    `libmpv.so` + headers, **or**
  - drop libmpv on Android and back `MpvWidget`'s role with **ExoPlayer/AVFoundation-style native
    playback** (bigger code change). *Recommend libmpv first.*
- [ ] **SDL2 for Android arm64** (or use the SDL2 Android prefab/AAR).
- [ ] **Qt Pdf** is part of Qt for Android — no separate build.
- [ ] Stage all `*.so` deps so `androiddeployqt` bundles them in the APK (`ANDROID_EXTRA_LIBS`).

## Phase 3 — Build system
- [x] Android branch in `native/CMakeLists.txt` (scaffolding) — `if(ANDROID)` packaging block (min/target
      SDK, `QT_ANDROID_PACKAGE_SOURCE_DIR`, `QT_ANDROID_EXTRA_LIBS` for the cross-compiled mpv/SDL2), and the
      desktop-only console `probe_*` tools are guarded out. `qt_add_executable` already makes the shared lib
      + APK target on Android. `WIN32_EXECUTABLE`/`MACOSX_BUNDLE` are no-ops off their platforms.
- [ ] Fill in `native/android/AndroidManifest.xml` + `res/` (see `native/android/README.md`).
- [ ] Cross-compile + point `-DMPV_LIBRARY=`/`-DSDL2_LIBRARY=` at the Android `.so` deps (Phase 2).
- [ ] `AndroidManifest.xml`: app name/icon, `INTERNET` permission, storage access (Scoped Storage /
      `MANAGE_EXTERNAL_STORAGE` or SAF for ROM/media folders), gamepad/`android.hardware.gamepad`.
- [ ] Bundle the addon JS (`addons/aiocatalog`) and themes as Android assets; resolve them via
      `assets:/` or copy to app files dir on first run (see Phase 4 storage).

## Phase 4 — Code changes
- [x] **CoreManager per-OS** — done (`bcf25eb`): fetches `android/.../arm64-v8a/<core>_libretro_android.so`.
- [x] **Storage paths** — done. All writable-data paths now go through `AppPaths::dataDir()`
      (`native/src/core/AppPaths.h`): desktop = the executable dir (unchanged, portable), Android =
      `QStandardPaths::AppDataLocation`. Switched every `applicationDirPath()` use (cores, emulators,
      saves, states, downloads, the shared `.ini`, logs, addons/themes roots) across 20 files. Only this
      one function changes per platform.
- [x] **External emulators gated** — the two entry points are behind `#if !defined(Q_OS_ANDROID)`: the
      Settings "Emulators" panel row is hidden, and `openGamePath`'s external-system branch shows a "not
      supported on Android" message instead of launching. `EmulatorManager` still compiles (harmless,
      unreachable); fully excluding it from the Android target + an Intent-based hand-off
      (`QJniObject` → `Intent` to an installed RetroArch/Dolphin-Android, passing the ROM URI) is a
      follow-up.
- [ ] **SystemCatalog**: external-emulator systems (Dolphin/Cemu/… `externalEmulator` set) have no
      in-process path → either hide them or route to the Intent launcher on Android.
- [ ] **Gamepad**: confirm SDL2 controller mapping on Android; expose touch on-screen controls for the
      libretro view (RetroView) — phones have no buttons.
- [ ] **UI/touch**: larger hit targets, momentum scrolling, back-button → app Back, immersive
      full-screen for video/RetroView, handle rotation + the Android lifecycle (pause/resume the core
      and mpv on `applicationStateChanged`).
- [ ] **Permissions flow**: request storage at runtime (API 23+); pick ROM/media folders via SAF.

## Phase 5 — Package & test
- [ ] `androiddeployqt` → debug APK; install to a device/emulator via `adb install`.
- [ ] Verify: catalog browse, a movie (libmpv), a comic/book/PDF, and an NES/SNES/GBA ROM
      (CoreManager downloads the `_android.so` core, RetroView runs it with JIT).
- [ ] Sign a release APK (keystore) for distribution.

## Android TV
The same APK runs on Android TV (Shield, Chromecast/Google TV, smart TVs) — it's the same runtime, no
separate target. This app is a **strong TV fit**: it's already D-pad/remote-navigable (TV's hard
requirement — no touch), controllers are common on TV (good for the in-process cores), and a libmpv media
hub on a television is the natural use case. The phone weakness (touch-awkward Widgets) is a non-issue on TV.

For a proper TV experience / Play-on-TV, the manifest needs:
- [ ] `LEANBACK_LAUNCHER` intent-filter + `android:banner` (320×180) → shows on the TV home screen.
- [ ] `<uses-feature touchscreen required="false">` and `<uses-feature software.leanback required="false">`
      → installable on TVs.
- [ ] Confirm D-pad (`KEYCODE_DPAD_*` → Qt arrow keys) reaches focus/navigation; handle TV Back.
- [ ] (Optional) `armeabi-v7a` ABI for very old 32-bit TV boxes — `arm64-v8a` covers modern devices.

Without the leanback bits it still **sideloads and runs** on a TV; it just won't appear in the TV launcher
rows. External standalone emulators stay gated off (in-process cores + a controller are the TV story).

## Distribution note
Downloading a core `.so` and `dlopen`-ing it is **against Google Play policy** (the RetroArch reason).
Options: (a) ship **sideload APK / F-Droid** and keep the download-on-demand model, or (b) for Play,
**bundle the cores in the APK** (`assets/cores/…`, copied to files dir on first run) and disable the
downloader on Android.

## Effort estimate
- Phase 1: ~half a day. Phase 2 (libmpv): **1–3 days** (the unknown). Phases 3–4: ~3–5 days.
  Phase 5 + polish: ongoing. The modern-console emulators are explicitly **out of scope**.
