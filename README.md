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

All releases are listed on the [**Releases page**](https://github.com/cubman3134/MyMediaVault/releases). Builds are produced automatically by [CI](.github/workflows/release.yml) for each tagged version.

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
