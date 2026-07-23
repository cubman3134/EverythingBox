# My Media Vault

A native, cross-platform **media hub** — video, audio (with playlists), libretro
emulation, EPUB/PDF readers, and a sandboxed JavaScript addon system — built as a
**Qt 6 / C++** shell (the architecture Kodi/Stremio/RetroArch use).

## Download

Grab the latest build for your platform:

| Platform | Download | Notes |
|----------|----------|-------|
| **Windows** (x64) | [**MyMediaVault-windows-x64.zip**](https://github.com/cubman3134/MyMediaVault/releases/latest/download/MyMediaVault-windows-x64.zip) | Unzip anywhere and run `MyMediaVault.exe`. |
| **macOS** (Apple Silicon) | [**MyMediaVault-macos-arm64.dmg**](https://github.com/cubman3134/MyMediaVault/releases/latest/download/MyMediaVault-macos-arm64.dmg) | Unsigned build — first launch: right-click the app → **Open**. |
| **Linux** (x86_64) | [**MyMediaVault-linux-x86_64.AppImage**](https://github.com/cubman3134/MyMediaVault/releases/latest/download/MyMediaVault-linux-x86_64.AppImage) | `chmod +x` the file and run it. |
| **Android / Android TV** (arm64) | [**MyMediaVault-android-arm64.apk**](https://github.com/cubman3134/MyMediaVault/releases/latest/download/MyMediaVault-android-arm64.apk) | Sideload it; runs on phones, tablets, and Android TV (Shield, Google TV, smart TVs). Media hub + in-process libretro cores; standalone emulators are desktop-only. See [Android / Android TV](#android--android-tv). |
| **iOS / iPadOS** (arm64) | [**MyMediaVault-ios-arm64.ipa**](https://github.com/cubman3134/MyMediaVault/releases/latest/download/MyMediaVault-ios-arm64.ipa) | Unsigned — sideload with [AltStore](https://altstore.io) or [Sideloadly](https://sideloadly.io) (they re-sign it with your Apple ID). Media hub (video/audio/readers/addons); emulation is unavailable on iOS. See [`native/docs/ios-port.md`](native/docs/ios-port.md). |

Current version: **0.5.0**. All releases are listed on the [**Releases page**](https://github.com/cubman3134/MyMediaVault/releases). Desktop builds are produced automatically by [CI](.github/workflows/release.yml) for each tagged version; Android is built from source (below).

This repository is the native app. The previous Unity implementation has been
removed; its portable logic (EPUB/PDF parsing, the addon model) was reimplemented
in C++.

## Where things are

Everything lives under [`native/`](native/):

- `native/src/` — the app: libmpv video/audio (`video/`), libretro emulation
  (`libretro/`, `emu/`), input (`input/`), readers (`ebook/`, `pdf/`), the JS
  addon system (`addons/`), and the Qt UI (`ui/`).
- `native/third_party/` — vendored single-file deps (miniz, Duktape).
- `native/tools/` — console probe harnesses that verify each subsystem headlessly.
- `native/addons/` — a bundled sample media-source addon.

See **[`native/README.md`](native/README.md)** for the toolchain, build commands,
and current status.

## Quick build

```
cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" ^
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" ^
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release
```

The libretro frontend + its `probe_core` harness build with just CMake + a C++17
compiler (no Qt); the full app is gated behind `-DMYMEDIAVAULT_BUILD_APP=ON`.

## Android / Android TV

The same APK targets phones, tablets, and **Android TV** (Shield, Chromecast / Google TV, smart TVs). The
app is fully **D-pad / remote navigable**, so it's a natural fit on a TV; game controllers (SDL2) drive the
in-process libretro emulators. The launcher entries for both phone (`LAUNCHER`) and TV
(`LEANBACK_LAUNCHER`) are in [`native/android/AndroidManifest.xml`](native/android/AndroidManifest.xml).

What's on Android: the media hub (libmpv video/audio, comics/books/PDF, the addon catalog) and **in-process
libretro cores** (Android allows JIT + `dlopen`; `CoreManager` fetches the right `…_android.so`). The
**standalone modern-console emulators** (Dolphin/PCSX2/RPCS3/…) are **desktop-only** — Android can't launch
downloaded desktop executables — and are gated off the Android build.

The **APK is built automatically by CI** (`android` in [`release.yml`](.github/workflows/release.yml)) and
attached to each release — it installs the Qt-for-Android kit + NDK, **self-provisions `libmpv`** (a prebuilt
arm64 build + its ffmpeg stack from the Jellyfin/jdtech AAR on Maven Central, with mpv headers from the mpv
repo), and runs `qt-cmake` → `androiddeployqt`. (Override the prebuilt via the `LIBMPV_AAR_URL` repo
variable.) Sideload the APK on a phone/tablet/TV; it's unsigned-for-Play, so distribution is sideload / F-Droid.

To **build locally** instead, you need the same: the Android toolchain and `libmpv`/`SDL2` cross-compiled
for the target ABI:

```
# 1) Install Android SDK + NDK and the Qt-for-Android 6.8.3 arm64 kit.
# 2) Cross-compile libmpv (+ffmpeg) and SDL2 for arm64-v8a.
# 3) Configure with the Qt-for-Android qt-cmake, pointing MPV_LIBRARY/SDL2_LIBRARY at the .so builds:
"$QT/android_arm64_v8a/bin/qt-cmake" -S native -B build-android -DMYMEDIAVAULT_BUILD_APP=ON \
  -DMPV_INCLUDE_DIR=… -DMPV_LIBRARY=…/libmpv.so \
  -DSDL2_INCLUDE_DIR=… -DSDL2_LIBRARY=…/libSDL2.so
cmake --build build-android                       # produces the APK via androiddeployqt
```

The CMake `if(ANDROID)` block wires the APK metadata (min/target SDK, version, `QT_ANDROID_EXTRA_LIBS`,
and the manifest/res under [`native/android/`](native/android/)). Cores download to the app's private data
dir at runtime — note that downloading + `dlopen`-ing cores is against Google Play policy, so distribute via
**sideload / F-Droid**, or bundle cores into the APK for Play. Full step-by-step plan and the remaining
checklist: [`native/docs/android-port.md`](native/docs/android-port.md).
