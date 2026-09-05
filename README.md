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
- `native/docs/` — feature notes, including
  [**Game updates and DLC**](native/docs/game-updates-and-dlc.md) (the `updates/`
  and `dlc/` sidecar convention and what each emulator does with them).

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

### Reading on a touch screen

On a phone or a tablet the reader answers a finger, using the same gesture vocabulary the video player
does — the same travel means the same thing in both.

**Tap zones** are a preset, not a matrix. Three of them: *left goes back, right goes forward* (the default),
its mirror for a left-thumb grip, and *every tap opens the menu* for readers who would rather turn pages by
swiping alone. In all three, a tap across the top of the page opens the menu, and the middle of the page
opens it too unless you are in the menu-only preset — where everything does.

**Swipe** sideways to turn the page: leftward for the next one, rightward for the previous. A short slide is
enough, and the direction is the same in every preset — a preset mirrors where your thumb reaches, not which
way the book runs.

**Keep the screen on while reading** is a toggle, off by default, and it is released the moment you leave
the reader. On Windows it holds the display awake for as long as the reader is on screen; other platforms do
not have an inhibitor wired up yet.

Tap zones and swiping apply on **touch screens only**. A mouse, a keyboard and a remote behave exactly as
they always have — the settings are there on every device so a phone-and-television household can find them
from either, but nothing a phone stores changes what the TV does.

**Two pages side by side** shows a book as a spread whenever the window is wider than it is tall and wide
enough for two readable columns. It is a pagination change and nothing more: your place, the page numbering
and the text itself are untouched, so turning it on or off mid-book leaves you on the same words. It applies
to books; a comic's two-up spread is its own, and already knows about right-to-left series.

Not here yet, and each deliberately its own pass: **auto-scroll** (paged and smooth), the **brightness
swipe**, and **volume-key paging**.

### Book servers (OPDS)

Point the reader at a self-hosted book server that speaks OPDS 1.2 — Calibre-web, Komga, Kavita,
Ubooquity — and browse its shelves as ordinary rows. Sign-in is HTTP Basic, and the credentials stay on
the device: they are never synced and never logged.

**Read online (OPDS-PSE).** Where the server advertises the Page Streaming Extension for a comic — Komga
and Kavita both do — a volume offers **Read online** beside **Download**, never instead of it. Reading
online fetches the volume's pages as images rather than pulling a 100–500 MB archive to reach page one,
asks the server to size them for the screen where it can, and starts with the page the server says you
reached. The reader is the same one every comic opens in: the same zoom, the same double-page spread, the
same bookmarks. Your position goes back to the server, so the same library read on a phone and on a TV
stays in step — the server owns the progress for its own books, exactly as it does for other kinds of
server-held media.

If the stream stalls or a page never arrives, the reader says so and offers to try again or to download
the volume instead; a part-downloaded volume is thrown away rather than cached, so a retry really does
re-fetch. A volume you have read once is cached whole, so re-opening it works with the server switched
off. PSE is a comics extension: EPUB and PDF from an OPDS catalog are downloaded and then opened.

### ComicInfo.xml

A comic archive that carries a **`ComicInfo.xml`** at its root — the de facto standard written by ComicRack,
Mylar, Komga, Kavita and essentially every comic tagging tool — is read from it rather than guessed at from
its filename. All four container types are read the same way (`.cbz`, `.cbr`, `.cb7`, `.cbt`), and the
document is picked up in the same pass that already counts the archive's pages, so a tagged library costs no
extra scan.

Honoured: `Series`, `Number`, `Volume`, `Title`, `Summary`, `Year`/`Month`/`Day`, the creator roles
(`Writer`, `Penciller`, `Inker`, `Colorist`, `Letterer`, `CoverArtist` — collapsed into one credit list, with
the **writer** as the primary author), `Publisher`, `Genre`, `LanguageISO`, `PageCount`, `Web`, `AgeRating`
and `Manga`. `<Pages>` (per-page spread hints) is read past; nothing here ever *writes* a ComicInfo.xml —
that is a tagger's job, not a reader's.

**Precedence** is the same everywhere in the library: embedded metadata beats a sidecar, which beats filename
inference, which beats online enrichment — and your own edits beat all of them. It applies per dimension: a
document that names a `Series` decides the series and the issue number together; one with no `Title` leaves
the shelf showing the file's own name, exactly as before. A comic with **no** ComicInfo.xml is grouped
precisely as it always was, by the folder-corroborated filename rule.

`Manga` = `YesAndRightToLeft` opens the comic **right to left**: the left/right keys swap and a two-page
spread puts the earlier page on the right. The page order, numbering and your saved place do not move. A
per-series override you set wins over the document.

`AgeRating` feeds the restricted (kids) profile, which hides anything at **Mature** or above:

| ComicInfo `AgeRating` | Level | Hidden on a kids profile |
|---|---|---|
| *absent*, `Unknown`, `Rating Pending`, anything unrecognised | Unrated | no |
| `Early Childhood`, `Everyone`, `G`, `Kids to Adults` | Everyone | no |
| `Everyone 10+`, `PG` | Everyone 10+ | no |
| `Teen` | Teen | no |
| `MA15+`, `Mature 17+`, `M` | Mature | **yes** |
| `R18+`, `Adults Only 18+`, `X18+` | Adults | **yes** |

A rating this table does not recognise is **Unrated**, never the nearest-looking rung — a comic nobody rated
is never certified as rated for children. Unrated comics are shown, because every comic tagged before this
existed is one and hiding them would empty the shelf.

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

## Read aloud

The reader can narrate a book. Press **Read aloud** in the reader's controls and it starts from
where you are; the spoken paragraph is highlighted, the page turns when the narrator leaves it,
and paragraph back/forward, pause and stop are the page keys and the controls beside it. There
is no second bookmark: the spoken paragraph *is* your reading position, so stopping leaves you
on the page it reached, that position is what a restart resumes from, and reading progress
accrues exactly as it does when you turn the pages yourself.

Speed is the same per-book preference an audiobook uses — set it once for a title and it holds
whether you are listening to a narrator or to your computer. Pitch is left alone.

**The voices are your operating system's.** EverythingBox bundles no speech engine and downloads
nothing: it speaks through SAPI on Windows, AVSpeech on macOS and iOS, the Android speech
service, and speech-dispatcher on Linux. Quality varies a great deal between platforms and
between the voices installed on one — add more in your system's own speech settings, then step
through them with the **Voice** control in the reader. If a build of EverythingBox was compiled
against a Qt without the TextToSpeech module, or the platform offers no engine at all, the
read-aloud controls are simply not there.

Works wherever the reader has structured text: EPUB, MOBI, FB2, TXT and Markdown, and a PDF read
in text mode. Comics have nothing to read. Screen-off and background listening are not here yet.

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
## Trackers (anime and manga)

**AniList** keeps your anime and manga progress in step with the app: finish a chapter or an episode here
and the chapter/episode count on your AniList list moves with it. Trakt (above) keeps film and general TV;
these two never write to each other.

**Setting it up.** The app ships no AniList client of its own yet, so you register a free one:

1. Sign in at [anilist.co](https://anilist.co) and open **Settings -> Developer -> Create New Client**.
2. Give it any name, and set its **redirect URL** to the loopback address `127.0.0.1` (the app opens a
   one-shot local listener on a random port and hands the browser back to it; AniList accepts the bare
   loopback host).
3. Copy the **Client ID** and **Client Secret** into **Settings -> General -> AniList** and press
   **Connect to AniList**. A browser opens; approve, and the tab tells you when you can close it.

Your client id, secret and tokens are stored **on this device only** and are deliberately excluded from
cloud sync, so you enter them once per machine. (They are also excluded from a settings *Discard*: linking
an account from inside the settings screen is not something backing out of it should undo.)

**Linking a series.** The first time you finish a chapter or an episode of something that is not linked
yet, the app searches AniList by title and offers the matches. Pick one and it is remembered; that link
*does* sync, so linking a series on the TV means the phone never asks. If the thing is not on AniList,
answer **This is not on AniList - stop asking** and you will not be asked again. Either answer can be
changed later from the item's detail page: **Track...** offers *Refresh from AniList*, *Link to a different
entry...* and *Unlink*.

**What is sent, and when.** One update per series per 30 seconds, so a fast reader does not spend the
account's rate limit; anything that cannot be sent is written to disk and delivered on the next launch, so
an offline session is not lost. The last chapter/episode of a series sets its status to **Completed** -
but only when AniList's own count agrees that it *was* the last one. A **score** is sent only if you have
actually rated the item here: AniList reads a zero as "rated zero", not as "unrated", so an unrated item
sends no score at all rather than wiping one you set by hand.

**Refreshing.** *Refresh from AniList* reconciles both ways, **furthest wins**: if AniList is ahead
(you read three chapters in another app), the app catches up; if the app is ahead, AniList is pushed to.
Neither side is ever moved backwards.

**Not there yet:** MyAnimeList and Kitsu (the seam is built for them and the ids are reserved), a
zero-config built-in client so no registration is needed, list browsing, and recommendations.

## Gestures

On a phone or tablet the video player answers to the gestures every mobile player uses. They register on
**touch form factors only** — with a mouse, a keyboard or a TV remote nothing here applies and nothing about
those inputs changes.

- **Swipe up and down** — on the **right** half for volume, on the **left** half for brightness, with the
  level shown while you move. Brightness is in-app: it dims the picture itself, so it works on platforms with
  no screen-brightness API of their own.
- **Swipe across** — scrub, with the time you are at and the time you would land on. Lifting your finger
  commits it; sliding back to where you started cancels it and nothing moves.
- **Double-tap** — the left or right third skips back or forward. It skips by the **same interval** as the
  skip buttons (Settings > Playback > *Audio jump interval*), not a second setting. Double-tap the middle to
  play or pause.
- **Hold** — double speed for as long as you hold, back to normal when you let go.
- **Pinch** — cycle how the video fits the screen: fit, fill, stretch.
- **Lock** — the lock button on the player suspends every gesture, for listening with the screen in a pocket.
  While it is locked a tap does nothing but bring the button back so you can unlock.

Each family can be switched off in **Settings > General > Gestures**, on either layout. The same section sets
how wide a band along the screen edge is left alone (24 px by default) so the system's own back and
notification swipes are never fought, and no gesture registers while a menu or overlay is open.

The readers have their own touch handling — tap zones for page turns, a swipe to turn, and pinch-to-zoom in
the PDF and comic views.

## When something won't open

A press that ends without opening anything leaves a mark that stays, rather than a message you have to be
looking at to catch. The toast still appears; what is new is that it is no longer the only trace.

- The item's **detail page** carries the failure — what went wrong and when it was last tried — with
  **Try again** beside it, **Choose another source…** where the item has more than one release, and
  **Dismiss** when you have had enough of it.
- The item's **row on the shelf** carries a small **⚠ didn't open** marker, so a glance down a folder shows
  which thing refused instead of inviting you to press it again blind.

The mark clears the moment the item opens, when you dismiss it, or on its own after seven days. It is
**local to this device and never synced**: it is a fact about this machine's last attempt on this network,
and the same title may open perfectly well on another device on the same account.

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
