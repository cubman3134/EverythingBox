# EverythingBox — native (Qt/C++)

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
| `MainWindow` + `main.cpp` (Open Video / Audio / Game / Document / Library / Settings / Save+Load State, stacked views, transport) | **builds** -> `EverythingBox.exe` (runnable copy at `C:\EverythingBox-app`, cores auto-download to `cores\`) |
| Ports from C#: ✅ epub · ✅ PDF · ✅ audio · ✅ JS addons (Duktape) | all ported; remaining Unity-only bits intentionally dropped |
| Jellyfin servers (`Jellyfin`, `JellyfinServerStore`, `JellyfinClient`): **several servers at once**, merged into one library | **core + settings built**; `probe_jellyfin` covers the ids, the migration, the store and the union. Drive verified against local fixture servers — see below || Jellyfin servers (`Jellyfin`, `JellyfinServerStore`, `JellyfinClient`, `browse/JellyfinCatalogs`): **several servers at once**, merged into one library — browse, play, and report progress back | **built**; `probe_jellyfin` covers the ids, the migration, the store, the union, PlaybackInfo, progress, resume and segments, `probe_browse` the browse levels, `probe_leafroute` the leaf, `probe_segments` the server tier. Drive verified against local fixture servers — see below |
| Jellyfin servers (`Jellyfin`, `JellyfinServerStore`, `JellyfinClient`): **several servers at once**, merged into one library | **core + settings built**; `probe_jellyfin` covers the ids, the migration, the store and the union. Drive verified against local fixture servers — see below |
| One music library across every source (`MusicId`, `MusicMerge`, `MusicRemap`, + `Subsonic`/`JellyfinMusic`/`ServerMusic` suppliers) | **built**; `probe_musicid`, `probe_musicremap` and `probe_musicsources` cover identity, the remap and all four suppliers. Drive verified against fixture HTTP stubs — see below |

## Jellyfin servers

**Several servers, merged into one library.** Settings → Jellyfin → *Jellyfin servers…* connects a
server (address → sign-in) and lists the ones already connected. Each can be **switched off**, which
hides its rows without forgetting the sign-in, or **removed**, which forgets the sign-in and changes
nothing on the server itself. Their libraries appear together, each row labelled with the server it
came from; the same film on two servers is deliberately **two rows** (no cross-server de-duplication —
a wrong merge hides content you asked to see). A server that does not answer contributes nothing and
**blocks nothing**: the shelf is drawn from whoever did answer, and the absent one gets one line.

**Sign-ins are device-local.** Each server's access token is stored under the `jellyfin/` settings
prefix, which is carved out of the synced settings bundle (`CloudSync::isDeviceLocalKey`), so it never
leaves the machine you signed in on. It is never logged and never rendered.

**How an item id looks.** Every stored reference to a Jellyfin item is *server-qualified*:

```
jf:<serverId>:<itemId>
```

`serverId` is the server's **own `Id`** from `/System/Info/Public` — not its URL. A URL is where a
server answers from *this* device on *this* network today (`http://10.0.0.4:8096` in the living room,
`https://jf.example.com` from a phone), so keying on it would give one server two identities and would
re-key every stored row the day a certificate appeared. The server's own id is stable across all of
that and is identical from every device, so a resume position banked on the television is found by the
phone. `itemId` is the server's own id, verbatim, including any colons in it.

Everything that keys on an item uses that qualified form: resume positions, watched marks, favourites,
playlists, recents and play statistics. Rows written in the older bare `jf:<itemId>` shape are migrated
on load, once, and the migration is idempotent and never drops a reference it cannot place. It does
**nothing at all** unless exactly one server is configured — with two, a bare row is ambiguous, and
guessing would file one person's resume position against the other's copy of the film.

**Browsing.** A *Jellyfin* folder appears under Video once at least one server is connected, and opens
onto the libraries of every enabled server together: **Films / Shows → a title → (for a show) its
seasons → its episodes**. Only libraries this app can play are listed — a music library is named by the
mapping but belongs to the music surface, and collections, playlists and live-tv views are not shown at
all rather than opening onto rows nothing can play. Rows are labelled with the server they came from
**only when more than one server contributed to that level**; with a single server the second line
shows the year or the episode instead. Every level is fetched on open and re-fetched on Back — a media
server's library is the thing most likely to have changed since you last looked. Both layouts.

**Continue Watching.** What the server says you are part-way through is merged into the home list as
its own section, beside the local recents and the Trakt shelves. Items you have not started are not in
it. It never holds up the home screen: the rows arrive and the list re-renders.

**Playing.** The server decides how its own file is played. Opening a row asks
`/Items/<id>/PlaybackInfo`; if the first media source supports direct play or direct stream, the file
is handed to the player as it stands, and otherwise the **server's own transcode URL** is used — which
is how Jellyfin's transcoding becomes a benefit here on day one rather than something to reimplement.
A source the server will offer neither for is refused honestly instead of opening an empty player.

**The stream URL is a credential.** It carries the access token in its query, because that is the only
way to hand a file to the player. So it is minted at the moment the player is handed it and is **never
written down**: a browse row carries no URL at all, and what a recents entry, a favourite or a playlist
records is the qualified id — which is also what makes such a row survive the token being rotated or
the server moving behind a certificate. `probe_jellyfin` drives the real recents store and then reads
every byte of the ini it produced to prove the token is not in it.

**Progress is the server's.** While a server item plays, its position is reported to the server that
owns it (`/Sessions/Playing`, `…/Progress`, `…/Stopped`) from the same throttled hook that would have
written a local resume position — and **no local resume row is written for it**, because two
authorities for one number means the last device to close wins. On open, the position comes from the
server's own `UserData.PlaybackPositionTicks` and beats any local mark, **including when it is zero**:
a film finished on a phone reports zero, and a local mark that overrode it would restart every
re-watch two minutes from the end. If the server cannot be reached for that one read, the local mark
is used and the film still plays.

**Intro and credit skipping.** A server running Jellyfin 10.10 or later has already detected them, and
`/MediaSegments/<id>` is read as **one more provider tier** beside the `.edl` sidecar, named chapters
and what you have taught the app yourself — ranked below a hand-written `.edl`, above a chapter title.
The existing skip chip and auto-skip act on it with no new UI at all. A server too old to have the
endpoint simply contributes one fewer tier.

**Not yet.** Quick Connect (address plus username and password is what exists today), SyncPlay,
downloads from the server, and live-tv endpoints. A Jellyfin **music** library is named by the browse
mapping but is not browsed here.
## One music library

**Four suppliers, one library.** The local music folder (#74), every Subsonic server (#193), every
enabled **Jellyfin** server's music, and the **EverythingBox server's music shelf** are folded into a
single artist list by `MusicMerge`. Someone who owns the same record in three places sees **one row**
with three copies behind it — not three parallel libraries. Everything below the merge is rendered by
the same three browse builders the local library has always used: there is no second artist list, no
second album row, no second track row and no second player.

**Tracks stay per-source.** Artists and albums unify; each copy keeps its own track list, because each
copy is a different set of files.

**Matching is conservative, because a wrong merge hides music.** A missed merge shows a duplicate,
which is untidy and completely recoverable by looking at it; a wrong one makes a record you own
unreachable with nothing on screen to say why. So MusicBrainz ids decide it wherever a source reports
them (Jellyfin's `ProviderIds`, Subsonic's `musicBrainzId`, the shelf's own `meta`, and the local
library's tags), the year is a **gate** and the track count only ever breaks a tie, and anything the
rules cannot answer confidently stays two rows. Two rows from the *same* supplier never merge: that
source has already grouped its own library, and fusing two records it kept apart would hide one of them.

**When it is wrong, you say so.** "These are not the same album" and "this is the same album as…" are
recorded per pair and win outright over every automatic rule, in both directions, across suppliers.

**Which copy plays.** *Settings → Play music from*: this device, any music server, or one server in
particular. The album page lists every copy with the source it is on and — where the source reports it
— its format and bitrate, so the FLAC on the NAS is distinguishable from the 128k copy on the phone. A
Subsonic copy shows no format, because that API does not tell us and a guess would defeat the point of
the line.

**Favourites, playlists, playtime and scrobbles follow the merged identity**, not the per-source id, so
changing which copy plays does not fragment a listening history (`MusicRemap`).

**A slow supplier costs nothing.** Each supplier's artist list is fetched under its own deadline, and a
server that is switched off or does not answer contributes nothing and blocks nothing — the library is
whatever the suppliers that did answer say it is, and the local folder is on screen immediately either
way.

**What a stored music key looks like.** Nothing new is minted for a merged row: it is rendered under
one of its copies' real keys, so everything downstream keeps working unchanged.

```
<artist><US>t<US><album>                    the local library         (US = 0x1F)
sub<US><server uuid><US><kind><US><id>      a Subsonic server
jf:<serverId>:<itemId>                      a Jellyfin server         (#160's own scheme, unchanged)
ebs<US><source id><US><kind><US><id>        the EverythingBox server's music shelf
```

The four families are mutually unreadable **by construction** — field count, first field and second
field together — so a key from one supplier can never resolve against another, and no local key can
parse as a remote one however a user names their band. `probe_musicsources` drives every pair.

**A credential is never stored.** The index holds an **id**; a stream URL — which for Jellyfin carries
`api_key` and for Subsonic carries `t`/`s` — is minted at the moment the player is handed it and
written nowhere, because the index is copied into queues and a queue is persisted.

## Layout
```
native/
  CMakeLists.txt            libretro lib + probe_core always; Qt app + other probes behind -DEVERYTHINGBOX_BUILD_APP=ON
  src/libretro/             LibretroCore.{h,cpp} + libretro.h  (no deps; load/run cores, options, save states)
  src/video/                MpvWidget                 (libmpv -> Qt OpenGL surface; video + audio + now-playing)
  src/emu/                  RetroView                 (core -> window, input routing, audio, save states)
  src/input/                Gamepad (SDL2), Keymap    (per-port remap, multiplayer, rumble, turbo)
  src/ebook/                EpubBook, Fb2Book, MobiBook/MobiHeader, TextBook, EbookView
                                                      (EPUB/FB2/MOBI-AZW3/text-Markdown parse + reader)
  src/pdf/                  PdfView                   (QtPdf / PDFium)
  src/addons/               AddonModels, AddonContext, JsAddon (Duktape), AddonManager
  src/core/                 Settings, CoreManager, SystemCatalog
  src/ui/                   MainWindow, SettingsDialog, ControllerRemapDialog, LibraryView
  src/main.cpp              app entry
  systems/recipes/          per-system launch recipes (#190: core options, firmware, content) - see docs/retro-computers.md
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

Configure + build (this exact command builds `EverythingBox.exe` cleanly):
```
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" ^
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" ^
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release
```
That build is runnable **in place** — a post-build step runs `windeployqt` over the exe and copies the
libmpv/SDL2 DLLs next to it, so no manual deploy is needed:
```
build\Release\EverythingBox.exe
```
(Turn the step off with `-DEB_WINDEPLOYQT=OFF`; then the exe needs `C:\Qt\6.8.3\msvc2022_64\bin`,
`C:\mpv-dev` and the SDL2 DLL on `PATH`, and Qt's QML/platform plugins found some other way.)
A ready-to-run copy is already deployed at **`C:\EverythingBox-app\EverythingBox.exe`** — double-click it,
**Open Video…**, and pick an MKV.

To regenerate the libmpv MSVC import lib (if you replace the DLL): dump its `mpv_*` exports to `mpv.def`
(`LIBRARY libmpv-2.dll` / `EXPORTS` / one symbol per line), then
`lib /def:mpv.def /machine:x64 /out:libmpv.lib`.

## Following a series (issue #155)

Star-shaped "I like this" is the favourite. **Follow** is the other half: *tell me when this grows*.

**What can be followed.** Any series-shaped row from any source — an addon catalogue series, a podcast feed
from the bundled Podcasts addon, a manga or comic series, a local-library show. Anything you can drill into
that is not a leaf. An episode, a track, a chapter, a film, a console folder or a playlist folder cannot be
followed, on either layout: the rule is one function (`follow::isFollowable`, `src/core/FollowPlan.h`) and
both the themed detail pill and the classic long-press menu ask it, so the verb cannot appear in one place
and not the other.

**Where the verb is.**

- *Themed layout*: the **Follow** pill on a series' detail page, beside Favorite. It shows the unread count
  once there is one, and grows a **Mark all seen** pill while there is something to clear.
- *Classic layout*: long-press / right-click a series row — Follow, Mark all seen, Check for new items now.

**The schedule.** A background pass asks each followed series' source what children it has now. Settings ▸
General ▸ Following offers **every 6 hours / every 12 hours / once a day (default) / once a week / only when
I ask**, plus **Check for new items now**. The pass is deliberately polite, and every clause of that is
pinned by `probe_follow` against a fake clock:

- one request in flight per source, and a minimum five-second gap between two requests to the same source —
  so forty followed shows on one addon cost it one request every five seconds, once a day, not forty at once;
- a jittered start (deterministic per install, bounded by the smaller of one tenth of the interval and 15
  minutes), so a household's boxes do not wake their shared sources on the same second;
- **skipped while anything is playing** — a film, an album, a game holding the emulator's frame loop;
- **skipped on a metered connection** by default (there is a setting to allow it);
- a source that fails is written off for that cycle — every other series it holds is skipped rather than
  retried — and asked again on the **next** cycle.

A skip does not consume the pass: it is deferred to the next tick, so an evening of playback delays the
day's check rather than losing it. "Check now" is a deliberate press and bypasses the playing/metered gates
(never the per-source ones).

**What counts as new.** Children that were not in this device's last snapshot of that series. The very first
check is a silent **baseline** — following a twenty-year-old podcast does not dump a thousand rows on your
home screen. A child that *disappeared* is not news either (a feed that only publishes its last 60 episodes
drops old ones constantly). A source that does not give its children stable ids cannot be diffed per child,
and degrades honestly to one row saying the series changed.

**The New shelf.** A peer of Recents and Continue, on both layouts, newest first. It lists unseen children
across every series you follow, and a followed tile carries an unread badge. Marking a child watched or read
— the completion states that already exist — takes it off the shelf and off the badge; "Mark all seen"
clears a whole series. The **You Missed** rows from Trakt (issue #25) land on this same shelf, deduplicated
by item id, because "an episode you were waiting for is out" and "a series you follow grew" are the same
sentence and do not want two headings. The uncapped *You Missed* folder under the video catalogue is
unchanged.

**What syncs and what does not.** The follow mark itself is per profile and **syncs** — it is a statement
about you, merged exactly as a favourite is (newest wins, with a deletion tombstone a newer re-follow beats).
What each device has already *seen* does **not** sync: that is a claim about a fetch this box performed, and
a peer re-derives its own snapshot silently on its first check.

**Not here yet.** This is increment 1. Still to come: **notifications** (a grouped system notification per
refresh cycle, off by default, with a per-series mute — the `FollowScheduler::newItemsFound` /
`cycleFinished` signals are the seam it will consume) and **optional auto-download** with a keep-last-N
retention rule (per series, off by default, non-metered only). Following individual *authors* rather than
series remains out of scope, as does any server-side push — this is local polling only.

## Roadmap
1. **libretro frontend** — ✅ load/init/run/video/audio/input, ✅ core options, ✅ save states. Verify a ROM
   end to end (`probe_core mgba_libretro.dll game.gba` — also exercises the save-state round-trip).
   Note: **OpenGL** hardware-rendered cores now run too — `RETRO_ENVIRONMENT_SET_HW_RENDER` is accepted for
   GL/GLES cores (N64 GLideN64, Beetle PSX HW, Flycast, ...); `RetroView` gives the core an offscreen
   `QOpenGLContext` + FBO and reads it back into the software paint path (no native GL child surface, so it
   stays clear of the fullscreen-compositing bugs). Vulkan/D3D HW cores are still declined (fall back to
   software). Mesen / Mesen-S are excluded from the catalog (their Windows builds fault on a worker thread here).
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
