# iOS port

The media hub on iPhone/iPad: libmpv video/audio, readers, and the JS addon catalog. Mirrors the
Android port's shape — `AppPaths::dataDir()` moves to the app's writable container, the stock
`themes2/` + first-party addons are staged into the bundle and extracted on first launch by
`AssetBootstrap`, the app is always fullscreen, and `FormFactor` resolves to **Mobile**.

**What's gated off on iOS** (all behind `Q_OS_IOS`):
- **Standalone emulators** (`EmulatorManager` is a stub) — iOS can't launch downloaded executables.
- **Downloaded libretro cores** — iOS forbids JIT and `dlopen` of downloaded code. (Cores statically
  linked into the app would be the Play-style workaround; not done.)
- **External players / QProcess** — `QProcess` does not exist on iOS. `ExternalPlayer::launchExe`
  returns false; the PC-game launch/extract paths in `MainWindow` fall back or report unavailability.
- **SDL2 gamepads** — not built (a native GCController backend would be the iOS approach).

## Rendering: the three iOS-specific fixes

1. **`MpvWidget` renders in software on iOS** (`src/video/MpvWidget.{h,cpp}`). Desktop uses a
   `QOpenGLWidget` + mpv's GL render API; on iOS the widget is a plain `QWidget` and mpv renders
   through `MPV_RENDER_API_TYPE_SW` into a `QImage` (same pattern as `theme2/MpvPreview`). OpenGL ES
   context creation fails outright in the simulator, and EAGL is deprecated on device.
2. **Widget backingstore flushes through Metal** (`main.cpp` sets `QT_WIDGETS_RHI=1` +
   `QT_WIDGETS_RHI_BACKEND=metal` before the QApplication). The default raster flush path goes
   through OpenGL ES and produces a fully-black screen even though the widget tree paints correctly.
3. **The window must be allowed to shrink to the phone screen** (`main.cpp`): a fullscreen window
   never shrinks below its layout minimum, and the desktop layout's aggregate minimum (~1117pt) is
   wider than an iPhone (402pt) — `window.setMinimumSize(1, 1)` before `showFullScreen()` lets
   fullscreen clamp to the real screen.

## Build (simulator)

Prereqs, one-time:

```sh
# Full Xcode (not just CommandLineTools), plus the *universal* iOS simulator runtime — the default
# runtime Xcode 26 installs is arm64-only and REJECTS x86_64 apps ("Failed to find matching arch");
# Qt's iOS simulator libraries are x86_64-only (every Qt release through 6.12), so the app runs
# under Rosetta in the simulator. Delete the default runtime first or the download reports
# "No needed downloadables found":
xcrun simctl runtime list                  # note the installed iOS runtime's UUID
xcrun simctl runtime delete <UUID>
xcodebuild -downloadPlatform iOS -buildVersion 26.5 -architectureVariant universal

# Qt 6.8.3 for iOS (+ host Qt for the build tools):
aqt install-qt mac desktop 6.8.3 clang_64 -m qtmultimedia qtpdf -O ~/Qt
aqt install-qt mac ios     6.8.3 ios      -m qtmultimedia qtpdf -O ~/Qt

# libmpv + its full static dependency stack from MPVKit (https://github.com/mpvkit/MPVKit):
# download every *.xcframework.zip listed as a binary target in MPVKit's Package.swift for the
# LGPL Libmpv target (Libmpv, Libav*, Libsw*, openssl, gnutls stack, libass stack, Libsmbclient,
# Libbluray, Libuavs3d, Libdovi, MoltenVK, Libshaderc, lcms2, Libplacebo, Libdav1d, Libuchardet,
# Libluajit) and unzip them all into one directory, e.g. ~/ios-deps/mpvkit/.
```

Configure + build (Xcode generator; x86_64 because that's Qt's simulator slice):

```sh
SIM=~/ios-deps/mpvkit/Libmpv.xcframework/ios-arm64_x86_64-simulator/Libmpv.framework
~/Qt/6.8.3/ios/bin/qt-cmake -S native -B build-ios -G Xcode \
  -DMYMEDIAVAULT_BUILD_APP=ON \
  -DQT_HOST_PATH="$HOME/Qt/6.8.3/macos" \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DMPV_INCLUDE_DIR="$SIM/Headers" \
  -DMPV_LIBRARY="$SIM/Libmpv" \
  -DMMV_IOS_MPV_DEPS_DIR="$HOME/ios-deps/mpvkit"
cmake --build build-ios --config Release
```

The `if(IOS)` CMake block links every framework/static-lib slice found under
`MMV_IOS_MPV_DEPS_DIR/*.xcframework/<slice>/` (device builds pick the `ios-arm64` slice
automatically) plus the required system frameworks, and stages `themes2/` + `addons/` into the
bundle as `mmv/`.

Run it:

```sh
xcrun simctl boot "iPhone 17 Pro"; open -a Simulator
xcrun simctl install booted build-ios/Release-iphonesimulator/MyMediaVault.app
xcrun simctl launch booted com.mymediavault.app
```

First launch is slow (~1 min): Rosetta ahead-of-time-translates the whole statically-linked binary
once, then it's cached.

### Driving it headlessly (uitest)

The UiTestServer works inside the simulator by pointing the pipe at an absolute path both sides can
reach:

```sh
SIMCTL_CHILD_MMV_UITEST=1 SIMCTL_CHILD_MMV_UITEST_PIPE=/tmp/mmv-ios-uitest \
  xcrun simctl launch booted com.mymediavault.app
MMV_UITEST_PIPE=mmv-ios-uitest python3 native/tools/uitest.py state
```

## Device builds (untested)

Configure with `-DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64`, drop
`CODE_SIGNING_ALLOWED=NO`, and set a development team
(`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<id>`). Qt's device slice is native arm64, and MPVKit's
`ios-arm64` slices are used automatically. Expect follow-up work: signing/provisioning, the
software video path's throughput on device (consider `hwdec=videotoolbox`), safe-area insets, and
mobile type-scale polish (panel titles overflow a phone width).

## Remaining checklist

- [ ] Safe-area insets (small unused bands at the very top/bottom of the screen)
- [ ] Mobile type scale: panel header titles overflow a 402pt-wide screen
- [ ] GCController-based gamepad input (SDL2 is desktop/Android-only here)
- [ ] `hwdec=videotoolbox` for the software render path on real devices
- [ ] Real-device build + signing (above)
