# Project Goliath — native (Qt/C++)

The re-platform off Unity. A native cross-platform media hub: video (everything, via **libmpv**),
emulation (**libretro** cores, including hardware-rendered ones), plus the readers/addons ported from the
Unity C# code. This is the architecture Kodi/Stremio/RetroArch use — a native shell rather than a game
engine — which is what makes both all-format video and libretro first-class.

## Why native (vs. the Unity build)
- **libretro** cores are a stable C ABI — you `LoadLibrary`/`dlopen` and call function pointers. No engine
  bridge. Hardware-rendered cores (Dolphin) get a real GL context, so they don't crash like SK.Libretro.
- **Video** is a native player (libmpv) embedded in a window — plays MKV/HEVC/AV1/AC3/etc., streams large
  files, no texture-bridge friction.

## Status
| Piece | State |
|---|---|
| `LibretroCore` (load core, init, run frames, BGRA video, audio/input routing, **core options**, **save states**) | **builds + inits real cores (mGBA), verified with MSVC.** Option harvesting verified on mGBA/Snes9x/Mupen64Plus-Next. |
| `probe_core` console harness | builds; loads/inits cores, dumps core options, save-state round-trip test |
| `MpvWidget` (libmpv render API -> Qt OpenGL surface; play/pause/stop/seek; audio-only "now playing" overlay) | **builds + runs** - video + audio (mp3/flac/ogg/wav/...) in the window; `probe_audio` verifies the audio path |
| Music playlist / folder queue (`MainWindow`: track list panel, prev/next, auto-advance on EOF) | **builds + deployed** - open one track to queue its folder, or multi-select tracks |
| Addon system (`AddonManager` + `JsAddon`/Duktape + `LibraryView` + `HomeView`): JS media-source addons, sandboxed host API, catalogs-by-type, drill-down, per-addon settings, `.addon` install | **builds + verified** - `probe_addon` runs the bundled **AIO Catalog** addon; music (MusicBrainz) + drill-down verified live |
| `RetroView` (core -> window, keyboard + **gamepad (SDL2)** -> RetroPad, **audio via QAudioSink**, **F2/F4 save/load**) | **builds** - emulation video + sound in the same window |
| `Gamepad` (SDL2 GameController -> RetroPad, hot-plug, analog sticks) | **builds**; SDL2 runtime load verified. Live pad test pending hardware. |
| Settings: per-system **core selection** + auto-download, per-core **options** editor | **builds + deployed** |
| `EpubBook` + `EbookView` (unzip + OPF/spine/TOC parse; page-by-page XHTML render, contents panel, font sizing, per-book resume) | **builds + verified** - parses the bundled Austen book (64 chapters, 61 TOC entries) via `probe_epub` |
| `PdfView` (QtPdf/PDFium: page-by-page render, zoom/fit-width, per-file resume) | **builds + verified** - QtPdf renders a round-tripped PDF via `probe_pdf` (cross-platform PDFium) |
| Input: remapping UI (controller + keyboard, per-port profiles), multi-player ports 1–4, rumble, turbo/autofire | **builds + deployed** - SDL enum/defaults cross-checked; live pad behaviour pending hardware |
| `MainWindow` + `main.cpp` (Open Video / Audio / Game / Document / Library / Settings / Save+Load State, stacked views, transport) | **builds** -> `goliath.exe` (runnable copy at `C:\Goliath-app`, cores auto-download to `cores\`) |
| Ports from C#: ✅ epub · ✅ PDF · ✅ audio · ✅ JS addons (Duktape) | all ported; remaining Unity-only bits intentionally dropped |

## Layout
```
native/
  CMakeLists.txt            libretro lib + probe_core always; Qt app + other probes behind -DGOLIATH_BUILD_APP=ON
  src/libretro/             LibretroCore.{h,cpp} + libretro.h  (no deps; load/run cores, options, save states)
  src/video/                MpvWidget                 (libmpv -> Qt OpenGL surface; video + audio + now-playing)
  src/emu/                  RetroView                 (core -> window, input routing, audio, save states)
  src/input/                Gamepad (SDL2), Keymap    (per-port remap, multiplayer, rumble, turbo)
  src/ebook/                EpubBook, EbookView       (EPUB parse + page-by-page reader)
  src/pdf/                  PdfView                   (QtPdf / PDFium)
  src/addons/               AddonModels, AddonContext, JsAddon (Duktape), AddonManager
  src/core/                 Settings, CoreManager, SystemCatalog
  src/ui/                   MainWindow, SettingsDialog, ControllerRemapDialog, LibraryView
  src/main.cpp              app entry
  third_party/              miniz (zip), duktape/ (JS engine)
  addons/aiocatalog/        bundled AIO Catalog addon (TMDB / IGDB / MusicBrainz)
  tools/probe_*.cpp         console verification harnesses (core / epub / pdf / audio / addon)
```

## Build
Prereqs: **CMake** + a C++17 compiler (MSVC/Clang/GCC). The libretro lib + probe build with just that:
```
cmake -S native -B build
cmake --build build --config Release
build/Release/probe_core <core.dll> [rom]      # e.g. mgba_libretro.dll some.gba
```
### Full app — toolchain is installed and the build is verified
Already set up on this machine:
- **Qt6 6.8.3 (MSVC)** -> `C:\Qt\6.8.3\msvc2022_64`  (installed via `aqt`)
- **libmpv** -> `C:\mpv-dev` (`include\`, `libmpv.lib` MSVC import lib, `libmpv-2.dll`)
- **SDL2 2.30.11 (VC)** -> `C:\SDL2` (`include\`, `lib\x64\SDL2.lib`, `lib\x64\SDL2.dll`) — gamepad input (optional;
  build omits controller support if not found)
- **QtPdf module** (PDF reading) — installed into the Qt prefix via `aqt install-qt windows desktop 6.8.3
  win64_msvc2022_64 -m qtpdf` (PDFium bundled; cross-platform). Deploy needs `Qt6Pdf.dll` + `Qt6PdfWidgets.dll`.

Configure + build (this exact command builds `goliath.exe` cleanly):
```
cmake -S native -B build -DGOLIATH_BUILD_APP=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" ^
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" ^
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release
```
Make it runnable (copy Qt + mpv + SDL2 DLLs next to the exe):
```
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe build\Release\goliath.exe --release
copy C:\mpv-dev\libmpv-2.dll build\Release\
copy C:\SDL2\lib\x64\SDL2.dll build\Release\
build\Release\goliath.exe
```
A ready-to-run copy is already deployed at **`C:\Goliath-app\goliath.exe`** — double-click it,
**Open Video…**, and pick an MKV.

To regenerate the libmpv MSVC import lib (if you replace the DLL): dump its `mpv_*` exports to `mpv.def`
(`LIBRARY libmpv-2.dll` / `EXPORTS` / one symbol per line), then
`lib /def:mpv.def /machine:x64 /out:libmpv.lib`.

## Roadmap
1. **libretro frontend** — ✅ load/init/run/video/audio/input, ✅ core options, ✅ save states. Verify a ROM
   end to end (`probe_core mgba_libretro.dll game.gba` — also exercises the save-state round-trip).
   Note: **software-rendered cores only** so far (`RetroView` blits frames). Hardware-rendered cores
   (Dolphin, GL/Vulkan N64) need a shared GL context via `RETRO_ENVIRONMENT_SET_HW_RENDER` — not done yet.
   Mesen / Mesen-S are excluded from the catalog (their Windows builds fault on a worker thread here).
2. **libmpv video** — ✅ embedded (render API) in a Qt OpenGL surface; "video" items route to it.
3. **Qt UI** — the media-hub shell (library, browser, now-playing), reusing the hub's behavior. [partial: shell + settings]
4. **Port the C# logic** — ✅ epub (`EpubBook`/`EbookView`, Qt rich-text), ✅ PDF (`PdfView`, QtPdf/PDFium),
   ✅ audio (libmpv), ✅ JS addon system (`AddonManager`/`JsAddon` on **Duktape**, `LibraryView`). The addon
   contract matches the Unity one (manifest.json + main.js; getCatalog/search; host log/httpGet/getStorage/
   setStorage). ✅ Per-call **execution timeout** (5s, via Duktape DUK_USE_EXEC_TIMEOUT_CHECK) so a runaway
   addon can't hang the UI; ✅ per-source **enable/disable** (checkboxes in the Library, persisted);
   ✅ **per-addon settings**: a manifest `settings` schema (text/password/checkbox) renders a form in the
   Library's "Configure…" dialog; values persist per addon and the script reads them via `getConfig(key)`.
   ✅ **catalogs by media type** (manifest `catalogs`) + **drill-down** (`getDetail` — TV show → episodes,
   album → tracks) + flexible **`httpRequest`** (POST/headers, for IGDB/Twitch & SteamGridDB) + **pagination**
   (`page` arg + `hasMore`, infinite scroll). Addon invocations run **off the GUI thread** (QtConcurrent;
   each call gets its own fresh Duktape context, so no interpreter state is shared) — the UI never blocks on
   addon network calls. A **Home** landing view (`HomeView`) browses catalogs by type with poster thumbnails.
   Ships an **AIO Catalog** addon:
   Movies/TV (TMDB), Games (IGDB), Music (MusicBrainz) — music verified live, the rest gated on API keys set
   in Configure…. (Associating local files with catalog items to play them is the next step.)
5. **Input + audio** — ✅ keyboard + ✅ gamepad (SDL2, hot-plug, analog) → libretro `onInput`; ✅ QAudioSink
   draining `onAudio`; ✅ controller + keyboard remapping UI; ✅ multi-player (ports 1–4); ✅ per-port controller
   *and* keyboard profiles (each player remaps independently); ✅ rumble (libretro `GET_RUMBLE_INTERFACE` →
   SDL haptics, per port); ✅ turbo/autofire (per-port, per-button, adjustable speed). Next: on-screen
   input config polish; input-latency tuning.
6. **Platforms** — desktop (Win/macOS/Linux) first; Android/iOS after; web is out (native code).

## What does NOT carry over
The Unity-specific code (MonoBehaviour UIs, SK.Libretro) is replaced. The portable C# *logic* (epub/PDF
parsing, addon models) is reimplemented or wrapped. The validated **LibVLCSharp** decode proof and the
**pure-C# remux** proof both informed this — but the native shell uses libmpv/libretro directly instead.
