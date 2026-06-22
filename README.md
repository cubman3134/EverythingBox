# Project Goliath

A native, cross-platform **media hub** — video, audio (with playlists), libretro
emulation, EPUB/PDF readers, and a sandboxed JavaScript addon system — built as a
**Qt 6 / C++** shell (the architecture Kodi/Stremio/RetroArch use).

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
cmake -S native -B build -DGOLIATH_BUILD_APP=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" ^
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" ^
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release
```

The libretro frontend + its `probe_core` harness build with just CMake + a C++17
compiler (no Qt); the full app is gated behind `-DGOLIATH_BUILD_APP=ON`.
