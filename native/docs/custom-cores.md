# Custom cores — running a libretro core EverythingBox doesn't ship

EverythingBox picks a core for you. For every system it knows about, `SystemCatalog` lists the cores it is
willing to use and which of them is the default, and a launch never asks you to choose. That curation is the
point of the app — but until now it was also a wall. A core we deliberately exclude, a core newer than our
table, a fork you built yourself, or something that is not an emulator at all (a game engine like `2048` or
`mrboom`) simply could not run.

**Custom cores are the way out.** You point EverythingBox at a core file you already have — or pick one from
the libretro buildbot in the **All cores** browser — and it becomes a choice you can make, for one game or for a
whole system.

## Loading one

**Settings ▸ Emulator Settings ▸ Custom cores…** (both layouts — a panel on the themed home, a page on the
classic one).

- **Load a core file…** opens a file picker. Choose a `.dll` (Windows), `.so` (Linux) or `.dylib` (macOS).
- **Pick up cores dropped in that folder** registers anything you copied into `<data>/cores/custom/` by hand.
  Same result, no picker.

Either way EverythingBox reads the core's own `retro_get_system_info` for its name, its version and the file
extensions it accepts, copies the file into `<data>/cores/custom/`, and registers it. The copy matters: your
original may be in a downloads folder you clear or on a stick you unplug, and a core that vanishes is a launch
that fails weeks later for no visible reason.

Re-loading a core you have already loaded **updates** it. That is how you take a rebuild: load it again.

## What happens then

A custom core becomes a **candidate** for every system whose extensions it claims — listed **after** every
core the catalogue offers, and tagged `(custom core)` in the picker.

**It never becomes a default.** Open a NES game with no choice made and you still get FCEUmm, exactly as
before. A custom core runs only when you say so:

- **for a whole system** — Settings ▸ Emulator Settings, the system's Emulation row;
- **for one game** — that game's own Emulation row (the per-game override).

A core that declares `supports_no_game` — the game-engine cores, which have no ROM to be opened from — gets a
**Run** entry of its own in the Custom cores page instead. That is the only way in for a core with no content,
which is exactly why it earns one.

## What EverythingBox will and won't do for you

**It will tell you, once, whose code this is.** The first custom core you load shows this and never mentions
it again:

> Custom cores are not curated by EverythingBox. This one runs as native code inside the app: if it crashes,
> misbehaves or corrupts a save, that is between you and the core. Shown once — you will not be asked again.

After that, a crash in a custom core is **labelled as such in the log** (`[CUSTOM CORE — not curated]` beside
the core's name) so anyone reading the log back is not hunting for a frontend bug that is not there.

**It will refuse a file it genuinely cannot host**, in a sentence naming the file — not a crash:

- a file that is not a loadable library at all (wrong platform, wrong architecture, not a library);
- a library that loads but has no libretro entry points;
- a core that reports a libretro API version this build does not speak;
- a core that faults the moment it is asked what it is.

**It will not refuse anything else.** A core we exclude from the catalogue is still yours to load; the
exclusion is advice, not a lock.

**It will tell you what a core asked for that we can't give it** — a Vulkan or Direct3D renderer, a camera, a
MIDI device, device sensors, multi-cartridge subsystems. That is *information*, not a refusal: the core may
well run on its own fallback path. Note the honest limit here: libretro has no manifest. A core declares what
it wants by **calling the frontend back**, and only the calls it makes during `retro_set_environment` are
visible before it runs. Anything it demands later — inside `retro_init`, at load time, or on the first frame —
cannot be seen up front, so this list is what the core told us, never a guarantee that it will work.

**It will never download a core behind your back.** A custom core is only ever fetched when *you* press
Install or Update in the All cores browser (below). `custom:` cores are excluded from the launch-time buildbot
fetch entirely: if a registered core's file is gone, the launch fails saying so rather than going looking for a
replacement.

## The All cores browser

**Settings ▸ Emulator Settings ▸ Custom cores… ▸ All cores (libretro buildbot)…** (both layouts — a panel on the
themed home, a page on the classic one).

It lists every core on the [libretro buildbot](https://buildbot.libretro.com/nightly/) for this platform that
EverythingBox does **not** already ship, with the date of its latest build, and a search box. Cores the catalogue
already knows (FCEUmm, mGBA, …) are not listed: they install the normal way, the first time you open a game that
needs one.

- **Install** downloads the core, checks it, and registers it as a custom core — exactly as if you had loaded the
  file by hand. So the one-time *not curated* notice, the note about anything the core asked for that we can't
  provide, and the per-system / per-game choice all apply to it the same way.
- **Update available** appears beside a core you installed from here when the buildbot has a build with a later
  date. Choosing it fetches the new build and swaps it in.
- A core you **loaded by hand** is shown as *Installed (loaded by hand)* and is never offered an update: EverythingBox
  can't know which build your file is. Load the new file to update it.

### What it does, and what it refuses

- **Where it downloads from.** Only `https://buildbot.libretro.com` — the same host EverythingBox already fetches
  catalogue cores from. Plain http, another host, another port, or a redirect to any of those is refused before a
  byte is fetched (or the moment the redirect arrives).
- **How much.** The core list is read up to 4 MiB and at most 2,000 cores; a core download up to 256 MiB. A line
  of the list it can't read is skipped (and counted in the page's "Not listed" line) — one bad line never costs
  you the rest.
- **What names.** A core's name becomes a file name on your disk, so only plain names are accepted — letters,
  digits, `_`, `-`, `+`. Anything carrying a path (`../`, a folder, a drive, a dot) is refused, and only this
  platform's own file (`<name>_libretro.dll` on Windows, `.so` on Linux, `.dylib` on macOS) is ever taken.
- **What gets written.** Only the one core file the list names is taken out of the download, under that name, into
  `<data>/cores/custom/`. Whatever else the archive holds, and whatever folders it claims, is ignored.
- **Checked before it counts.** The downloaded core is loaded and asked what it is (`retro_get_system_info`)
  **before** it is registered. A file that won't load, or doesn't answer, is **deleted**, and you're told why.
- **Updates can't break what you have.** The new build is downloaded to a staging folder, checked, and only then
  put in place in a single rename. A download that fails, an archive without the core in it, a file that isn't a
  core, or a core that is in use by a running game all leave the **old core exactly where it was**, and say so.

## Removing one

**Remove** drops the *registration*. The copy stays in `<data>/cores/custom/`, so "Pick up cores dropped in
that folder" brings it straight back. Nothing in Settings deletes a file from your disk.

If a game or a system was set to a core you removed, that setting is simply ignored and the catalogue default
runs again — a stale choice never errors a launch out.

## Not covered yet

- **Custom cores do not sync between devices.** The registry records a path on *this* machine, and a peer that
  does not have the file would carry a choice it could never honour.
- Content-less cores are **not** recorded in Continue Watching or play statistics: both are keyed by a game,
  and a core with no content is not one. The Run entry is the durable way back to it.
