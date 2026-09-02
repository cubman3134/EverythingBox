<p align="center">
  <img src="native/resources/appicon.png" width="132" alt="EverythingBox">
</p>

<p align="center">
  <a href="https://discord.gg/bW7KMVhgwH"><img src="https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://www.reddit.com/r/EverythingBox/"><img src="https://img.shields.io/badge/Reddit-r%2FEverythingBox-FF4500?logo=reddit&logoColor=white" alt="Reddit"></a>
  <a href="https://www.patreon.com/c/TheEverythingBox"><img src="https://img.shields.io/badge/Patreon-support-FF424D?logo=patreon&logoColor=white" alt="Patreon"></a>
</p>

# EverythingBox

A native, cross-platform **media hub** — video, audio (with playlists), libretro
emulation, book and comic readers, and a sandboxed JavaScript addon system — built
as a **Qt 6 / C++** shell (the architecture Kodi/Stremio/RetroArch use).

## Download

Grab the latest build for your platform:

| Platform | Download | Notes |
|----------|----------|-------|
| **Windows** (x64) | [**EverythingBox-windows-x64.zip**](https://github.com/cubman3134/EverythingBox/releases/latest/download/EverythingBox-windows-x64.zip) | Unzip anywhere and run `EverythingBox.exe`. |
| **macOS** (Apple Silicon) | [**EverythingBox-macos-arm64.dmg**](https://github.com/cubman3134/EverythingBox/releases/latest/download/EverythingBox-macos-arm64.dmg) | Unsigned build — first launch: right-click the app → **Open**. |
| **Linux** (x86_64) | [**EverythingBox-linux-x86_64.AppImage**](https://github.com/cubman3134/EverythingBox/releases/latest/download/EverythingBox-linux-x86_64.AppImage) | `chmod +x` the file and run it. |
| **Android / Android TV** (arm64) | [**EverythingBox-android-arm64.apk**](https://github.com/cubman3134/EverythingBox/releases/latest/download/EverythingBox-android-arm64.apk) | Sideload it; runs on phones, tablets, and Android TV (Shield, Google TV, smart TVs). Media hub + in-process libretro cores; standalone emulators are desktop-only. See [Android / Android TV](#android--android-tv). |
| **iOS / iPadOS** (arm64) | [**EverythingBox-ios-arm64.ipa**](https://github.com/cubman3134/EverythingBox/releases/latest/download/EverythingBox-ios-arm64.ipa) | Unsigned — sideload with [AltStore](https://altstore.io) or [Sideloadly](https://sideloadly.io) (they re-sign it with your Apple ID). Media hub (video/audio/readers/addons); emulation is unavailable on iOS. See [`native/docs/ios-port.md`](native/docs/ios-port.md). |

Latest release: **0.5.0** (in-development builds carry a higher patch number — see `project(… VERSION …)` in [`native/CMakeLists.txt`](native/CMakeLists.txt)). All releases are listed on the [**Releases page**](https://github.com/cubman3134/EverythingBox/releases). Desktop builds are produced automatically by [CI](.github/workflows/release.yml) for each tagged version; Android is built from source (below).

This repository is the native app. The previous Unity implementation has been
removed; its portable logic (EPUB/PDF parsing, the addon model) was reimplemented
in C++.

## Where things are

Everything lives under [`native/`](native/):

- `native/src/` — the app: libmpv video/audio (`video/`), libretro emulation
  (`libretro/`, `emu/`), input (`input/`), readers (`ebook/`, `pdf/`), the JS
  addon system (`addons/`), and the Qt UI (`ui/`).
- `native/third_party/` — vendored deps (miniz, Duktape, the LZMA SDK, unarr).
- `native/tools/` — console probe harnesses that verify each subsystem headlessly.
- `native/addons/` — a bundled sample media-source addon.

Feature notes for things with a server on the other end live in [`docs/`](docs/) —
see [**Audiobookshelf**](docs/audiobookshelf.md) for connecting an Audiobookshelf
library.

See **[`native/README.md`](native/README.md)** for the toolchain, build commands,
and current status, and
**[`native/docs/play-on-device.md`](native/docs/play-on-device.md)** for handing playback
between two EverythingBoxes on the same network.

## Quick build

```
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" ^
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" ^
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release
```

The libretro frontend + its `probe_core` harness build with just CMake + a C++17
compiler (no Qt); the full app is gated behind `-DEVERYTHINGBOX_BUILD_APP=ON`.

## Reading formats

The reader opens these directly — no conversion step, no external tool:

| | Formats |
|---|---|
| **Books** | `.epub` · `.fb2` (and the zipped `.fb2.zip` / `.fbz`) · `.mobi` · `.azw` · `.azw3` (KF8) · `.txt` · `.md` · `.pdf` |
| **Comics** | `.cbz` · `.cbr` · `.cb7` · `.cbt` (and a bare `.zip` of page images) |

All of them share one reader: the same pagination, font sizing, contents panel, bookmarks, per-book resume
and reading stats, and all of them are picked up by the local **reading library** scan (`.cb7` and `.cbt`
open but are not scanned — reaching page one of either costs a whole-archive extraction).

Two limits worth stating plainly:

- **DRM-protected books are not supported and never will be.** A `.mobi`/`.azw3` bought from a store is
  refused with a message saying so; this is for your own DRM-free files. Nothing here removes DRM.
- **RAR5 `.cbr` files are not readable yet.** The vendored RAR decoder ([unarr](https://github.com/selmf/unarr))
  handles RAR 2.9/3.x/4.x; a RAR5 archive is refused by name rather than reported as damaged. Repacking as
  `.cbz` opens it.

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
"$QT/android_arm64_v8a/bin/qt-cmake" -S native -B build-android -DEVERYTHINGBOX_BUILD_APP=ON \
  -DMPV_INCLUDE_DIR=… -DMPV_LIBRARY=…/libmpv.so \
  -DSDL2_INCLUDE_DIR=… -DSDL2_LIBRARY=…/libSDL2.so
cmake --build build-android                       # produces the APK via androiddeployqt
```

The CMake `if(ANDROID)` block wires the APK metadata (min/target SDK, version, `QT_ANDROID_EXTRA_LIBS`,
and the manifest/res under [`native/android/`](native/android/)). Cores download to the app's private data
dir at runtime — note that downloading + `dlopen`-ing cores is against Google Play policy, so distribute via
**sideload / F-Droid**, or bundle cores into the APK for Play. Full step-by-step plan and the remaining
checklist: [`native/docs/android-port.md`](native/docs/android-port.md).

## Home rows

The home screen's rows are yours to arrange. **Settings ▸ Home screen ▸ Choose home rows** opens a list of
the rows your home can show; each one can be moved up, moved down, hidden, or capped to the first *N* items,
and **Add row…** offers the ones that are not on it yet. It is driven entirely with a D-pad or the arrow
keys — there is no drag-and-drop to reach for.

- **Nothing changes until you change it.** A profile that has never opened the editor sees exactly the home
  it saw before, in the order the app ships with. **Reset to default** puts it back at any time.
- **It is per profile, and it syncs.** The arrangement rides the same profile sync as your favourites and
  playlists, so a new device inherits it. If two devices are edited apart, the most recent arrangement wins
  and rows only one of them knew about are kept, never dropped.
- **Your theme still decides what it can show.** The list orders and hides rows among the ones the active
  home actually draws; it cannot add a row a theme has no place for. The classic home arranges its shelves
  (Continue watching, Airing Soon, You Missed, Favorites, and — once you add them — Downloaded, a playlist,
  or a saved filter); a themed home arranges its media-type categories and catalogue tiles. Rows that belong
  to the other layout are kept in your list and simply skipped, so switching layouts never loses them. Theme
  authors: see [`native/themes2/THEME_FORMAT.md`](native/themes2/THEME_FORMAT.md).

## After a crash

On Windows a crash records itself. `crash_report.log` gets the faulting module and offset, the
registers and the call chain — code addresses only, never memory contents — and it is written to
**two** places, the same bytes in each:

- **beside the executable**, so a copy you unzipped somewhere keeps its own record;
- **`%LOCALAPPDATA%\EverythingBox\crash_report.log`**, a fixed per-user file that every build on
  the machine appends to. This is the one to attach to an issue, and the one that still exists
  after the copy that crashed has been deleted.

Both are created empty at startup, so a zero-byte `crash_report.log` means the reporter was armed
and the run was clean.

## Community

There is a Discord for EverythingBox: **[join here](https://discord.gg/bW7KMVhgwH)**.

It exists because most of what people need is not a bug report. Setup problems,
"is this supposed to happen?", which libretro core to use for a given system,
add-on and theme authoring — those are conversations, and an issue thread is a
poor place to have one. Ask in `#support`, and say which platform you are on and
which area you are in (video, audio, emulation, add-ons, themes, readers); a
vague question gets a vague answer. `#announcements` carries every release.

There is also a subreddit: **[r/EverythingBox](https://www.reddit.com/r/EverythingBox/)**.
Same conversations, slower and searchable — the better place for a question whose
answer the next person should be able to find, and for showing off a setup, a
theme, or an addon. Flair support posts `🆘 Support` and flip them to `✅ Solved`
when they are.

Reproducible bugs and feature proposals still belong in the
[issue tracker](https://github.com/cubman3134/EverythingBox/issues) — those need
to stay searchable and stay open until they are fixed, which chat is bad at.

EverythingBox is funded on **[Patreon](https://www.patreon.com/c/TheEverythingBox)**.
The app is free and every feature ships to everyone; the $5 Supporter tier is a
thank-you — the 💖 Supporter role and private lounge on the Discord, and the
matching flair on the subreddit.

## Licence

EverythingBox is free software under the **[GNU General Public License v3.0](LICENSE)** — use it,
study it, change it, and share it, including commercially. Derivative works must stay under the same
licence and ship their source.

[`NOTICE`](NOTICE) carries the copyright statement and the third-party terms. A built binary links
Qt, libmpv and SDL2, and the app downloads emulator cores and add-ons at runtime — none of which the
GPL grant here can relicense. Read it before redistributing builds.
