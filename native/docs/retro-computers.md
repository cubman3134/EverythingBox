# Retro computers — what launches with no setup, and what firmware you have to supply

A console is "insert cartridge, press play". A retro **computer** is not: it wants a machine model, a memory
size, firmware, a disk in the right drive, and often a typed command. Every one of these systems has a
community-standard answer, and the cores already implement most of them — so EverythingBox carries that
knowledge as data instead of expecting you to.

Each system's knowledge lives in a **launch recipe**: `native/systems/recipes/<systemId>.json`, shipped inside
the app. You can override any of it without a rebuild by dropping a file of the same name in
`<data>/systems/recipes/` — the same arrangement `<data>/systems/*.json` uses to override the system catalog.
A recipe that fails to parse is logged and ignored; the shipped one stands.

## Where firmware goes

Everything below is relative to the **system folder** next to the app — the same folder the BIOS check uses.
EverythingBox never downloads this firmware: it is copyrighted, it is sold (Amiga Forever, for instance), and
it is yours to supply. What the app does instead is refuse the launch with a message naming the **exact file**
and the **exact folder**, instead of showing you a black screen.

## Per system

| System | Zero-config today | Firmware you must supply | Notes |
|---|---|---|---|
| **MS-DOS** | **Yes, fully.** Drop a game in as a ZIP or as its own folder and press play. | None. | See below. |
| **Commodore Amiga** | **Yes for WHDLoad `.lha`**, once a Kickstart is present. | A Kickstart ROM: `kick40068.A1200` (3.1, what WHDLoad titles want), or `kick34005.A500` (1.3, most floppy games), or any of `kick37175.A500`, `kick40063.A600`, `kick39106.A1200`, `kick40060.CD32`, `amiga-os-310-a1200.rom`, `amiga-os-130.rom`. Encrypted Amiga Forever ROMs also need `rom.key`; IPF floppy images need `capsimg.dll`. | PUAE launches a pre-installed WHDLoad `.lha` directly — it builds the helper image, copies the Kickstart in and writes the config itself. **Prefer `.lha` over `.adf`**: that one choice is most of Amiga zero-config. `.lha` is now a recognised Amiga extension, so a loose one routes on its own. |
| **Atari ST** | Yes, once a TOS image is present. | `tos.img` (also read from `hatari/tos/tos.img`). | The recipe pins `hatari_tosimage` to `default`; hatari's own auto-detection resolves to an invalid path once a TOS is present, and every game is rejected. |
| **Commodore 64** | Yes. VICE autostarts `.d64` / `.prg` / `.crt` / `.tap` — no `LOAD"*",8,1`. | None — VICE compiles the Commodore ROMs in. | The recipe keeps true drive emulation on (accuracy) and turns on load warp for disks, so a real 1541's loading time is not the experience. Optional JiffyDOS ROMs go in `vice/`. |
| **Commodore VIC-20** | Yes. | None. | Memory expansion is set to `auto`, which is what most titles need. |
| **ZX Spectrum** | Yes. Tapes auto-load; snapshots run directly. | None for any stock Sinclair model. Pentagon / Scorpion clones want `fuse/128p-0.rom`, `fuse/256s-0.rom` and friends. | The recipe turns on automatic model selection, so a 128K-only game stops appearing to do nothing on a 48K. |
| **Amstrad CPC** | Yes. cap32 reads the disk's own AMSDOS catalogue and types the `RUN"` command for you. | None — the CPC firmware is compiled into the core. | See [Amstrad CPC in detail](#amstrad-cpc-in-detail). CrocoDS is offered as an alternative core but has no catalogue autorun of its own: it shows a file browser instead. |
| **Apple II** | Yes. `.dsk`, `.woz`, `.po`, `.2mg` and the rest boot from slot 6 drive 1 with nothing set. | None — the Apple II ROMs are compiled into the core (`resource/Apple2e_Enhanced.rom` and friends), and the core declares no firmware at all. | The recipe selects the enhanced //e, which is the machine most software expects, and keeps the slot-7 hard-disk controller in place so a `.hdv` / `.2mg` hard-disk image boots too. |
| **MSX / MSX2** | Yes for the content — carts and disks boot themselves — **once the machine files are in place**. | **blueMSX** (the default) needs the `Machines/` and `Databases/` folders from a blueMSX install, at the root of the system folder: it opens `<system>/Machines` and `<system>/Databases` directly, and its own metadata marks both as required. **fMSX** needs the BIOS ROMs, flat in the system folder and spelled in upper case: `MSX2P.ROM` + `MSX2PEXT.ROM` (it starts as an MSX2+), plus `DISK.ROM` before any disk image will start. | MSX did not exist as a system in EverythingBox before this — blueMSX was only ever a fallback core for SG-1000 and ColecoVision — so there was nothing a recipe could attach to. `.mx1` and `.mx2` route on their own; `.rom`, `.dsk` and `.cas` are claimed by other systems, so put those in the `msx` folder or open them from an MSX shelf. |
| **NEC PC-98** | Not until you supply the ROMs. | `np2kai/bios.rom` — **lower case**, that is the only spelling the core opens — and a font, which it genuinely does try in four spellings (`np2kai/FONT.BMP`, `font.bmp`, `FONT.ROM`, `font.rom`). Several titles also want `np2kai/itf.rom` and `np2kai/sound.rom`. | The launch is refused with the file names rather than left at a black screen. Note the core does carry a stub fallback BIOS and will *start* without `bios.rom`; almost nothing real runs on it, so EverythingBox treats the file as required. Machine model is `PC-9801VX` (the core offers only `PC-286`, `PC-9801VM` and `PC-9801VX`). |
| **Sharp X1** | Not until you supply the IPL ROM. | `xmil/IPLROM.X1` — upper case, 32 KB. The turbo / turbo Z machine types use `xmil/IPLROM.X1T` instead. | The core falls back to a stub boot ROM, which is why an X1 disk usually shows a blank screen. The machine type is `X1` by default; its option keys are upper case (`X1_ROMTYPE`), unlike every other core here. |

### Alternative cores

Every system above lists more than one candidate core in the settings, and a recipe covers each core it
knows about separately — because option keys, firmware and content handling are properties of the *core*,
not of the system. Two that are easy to trip over:

* **Amiga → `puae2021`.** Built from the same source as `puae` (the `2.6.1` branch, whose makefile sets
  `TARGET_NAME := puae2021`), so it uses the same `puae_*` options and reads the same Kickstart filenames
  from the same folder. Switching to it keeps the Kickstart check and the message.
* **MS-DOS → `dosbox_core`.** The opposite of DOSBox-Pure on the one thing that matters here: it has **no
  ZIP support at all**. A game you move onto it is unpacked first, and a folder game is still handed the
  program inside it. Your per-game core choice reaches that decision, so a single overridden game behaves
  correctly while the rest of your DOS library keeps DOSBox-Pure's handling.

## MS-DOS in detail

Put a DOS game in the `dos` folder either way round:

* **as a ZIP** — handed to DOSBox-Pure exactly as it is. The core reads ZIPs natively and keeps everything the
  game writes in a save file beside it, so unpacking it first would both waste the space and throw the saves
  away. If the ZIP holds one program it runs straight away; if it holds several, the core's own start menu
  offers them.
* **as a folder** — one folder per game, and the folder is one entry in your library rather than a tile per
  file inside it. EverythingBox finds the program to run and hands DOSBox-Pure that, which makes the core
  mount the folder as `C:` and run it with no menu at all.

### How the program is chosen

In order, stopping at the first step that decides:

1. only `.bat`, `.exe` and `.com` files count as programs;
2. programs at the top of the game folder beat ones inside a sub-directory;
3. obvious non-games are set aside — `INSTALL`, `SETUP`, `CONFIG`, `SETSOUND`, `UVCONFIG`, `README`,
   `DOS4GW`, `CWSDPMI` and the like — but never all of them: a folder whose only program is `INSTALL.EXE` is
   still launchable;
4. if one program is left, that is the answer;
5. a `.BAT` wins, because in DOS the batch file is what sets things up and calls the real binary;
6. otherwise the program named after the folder wins (`Doom\DOOM.EXE`);
7. if it is still ambiguous, EverythingBox asks — once. Your answer is remembered against that game and the
   next launch goes straight in. (It is stored with the game's other launch overrides, so it follows the game
   rather than the path, and syncs with the rest of your settings.)

A game folder that ships its own `DOSBOX.CONF` is honoured automatically.

### A `dosbox.conf` beside the game

Every GOG release, game pack and "how do I make this run" answer in the DOS world is written as a
`dosbox.conf`. It is how DOS configuration is passed between people, so EverythingBox reads one if you put it
next to the game — `dosbox.conf`, or the `dosbox_<game>.conf` a GOG install ships (the `_single` twin, which
also runs the game and exits, is skipped in favour of the settings one), or the single `.conf` in the folder
if there is exactly one. Two unrelated `.conf` files is an ambiguity the app will not resolve for you: it
uses neither and says nothing.

There are **two** ways that file gets used, and which one you get depends on the engine:

* **DOSBox-Pure (the default) translates it.** A libretro core is configured through core options, not
  through a conf, and the two are not the same set — so the translation is *partial by nature*. What the app
  does about that is tell you: every launch with a conf beside it reports **how many of its settings were
  applied, which ones, and which were ignored**. Nothing is dropped quietly.
* **DOSBox Staging / DOSBox-X take it as it is.** Handed `-conf <your file>`, a real DOSBox reads its own
  format, so nothing is translated and nothing is lost. See [Standalone DOSBox](#standalone-dosbox) below.

A conf with a **syntax error** applies nothing at all and says which line: a half-read conf would leave you
debugging a game configured out of the first few lines of your file.

What the shipped mapping carries onto DOSBox-Pure:

| `dosbox.conf` | Core option | Notes |
|---|---|---|
| `[dosbox] machine` | `dosbox_pure_machine` | every `svga_*` and `vesa_*` value maps to `svga`; `vgaonly` → `vga`; `ega`, `cga`, `tandy`, `hercules`, `pcjr` map across |
| `[dosbox] memsize` | `dosbox_pure_memory_size` | the core offers a fixed list of sizes; one that is not on it is reported as ignored rather than rounded |
| `[cpu] cycles` | `dosbox_pure_cycles` | `auto` → auto, `max` / `max 80%` / `max limit N` → max, `fixed N` and a bare `N` → that count |
| `[cpu] cputype` | `dosbox_pure_cpu_type` | `386`, `386_prefetch`, `486_slow`, `pentium` and `auto`; the core emulates fewer types than a conf can name |
| `[cpu] core` | `dosbox_pure_cpu_core` | `dynamic*` → dynamic, `full` → normal, plus `auto`, `normal`, `simple` |
| `[sblaster] sbtype` | `dosbox_pure_sblaster_type` | `sb1` … `sb16`, `gb`, `none` |
| `[sblaster] oplmode` | `dosbox_pure_sblaster_adlib_mode` | `auto`, `cms`, `opl2`, `dualopl2`, `opl3`, `opl3gold`, `none` |
| `[midi] mididevice` | — | deliberately **not** translated: the MIDI device is a setting, because it depends on which ROMs or soundfont you supplied (below) |
| `[autoexec]` | — | mounting and start-up commands are the emulator's own job |
| everything else | — | reported as ignored, by name |

DOSBox-Pure *also* reads a conf inside the game folder or ZIP itself, which is why `dosbox_pure_conf` is
seeded to `inside`. The translation above is the layer on top: it makes the settings visible to the app, so
they can be reported, and it works for `dosbox_core` too (which has its own, smaller mapping onto its own
option names — a conf mapping is a property of the *core*, never of the system).

The conf sits between your own settings and the system recipe:

    your per-core setting  >  your per-game override  >  this game's dosbox.conf  >  the system recipe

— a conf is more specific than the recipe, because it is about one game; but it is a file that happened to be
in the folder, so anything you chose by hand still wins.

## Standalone DOSBox

Two real DOSBoxes are registered beside the in-process core. Neither is the default — **DOSBox-Pure stays
what MS-DOS launches on** — and both are offered per system or per game through the same picker that offers
ares for N64.

| Entry | What it is for |
|---|---|
| **DOSBox Staging** | The actively developed DOSBox: better defaults, modern scaling, cleaner audio. The "nicer than the core" option. |
| **DOSBox-X** | The accuracy fork: Tandy, PCjr, specific 386/486 variants, obscure hardware, and non-game DOS software (Windows 3.x, development tools) the core does not aim at. |

Both are launched as `-conf <your conf> -exit <the game>`, so the conf hand-off is exact: **nothing is
translated on this path**, because DOSBox is reading its own file. The log says which path a launch took and
which conf, if any, was passed.

Neither is auto-installed. Put the emulator where EverythingBox looks (`emulators/dosbox-staging/` or
`emulators/dosbox-x/` beside the app), or point a `<data>/emulators/*.json` entry at a copy you already have
with `"binary": "C:/path/to/dosbox.exe"`. Until then the launch says which binary is missing and where to get
it.

## MS-DOS MIDI: MT-32 and General MIDI

Many DOS games sound dramatically better through a Roland MT-32 or a General MIDI device than through Adlib
or the PC speaker, and DOSBox-Pure supports both — given the assets. **EverythingBox never downloads or
bundles them**: MT-32 ROMs are copyrighted, and a soundfont is somebody else's licensed content. It does the
same thing it does for firmware — names the exact file and the exact folder.

Settings ▸ General ▸ **MS-DOS MIDI device** (on both layouts):

| Choice | Files, in the **system folder** |
|---|---|
| **Default (the core decides)** | none — this is what happens if you never open the setting |
| **General MIDI (SoundFont)** | `DOSBOX.SF2` — any General MIDI soundfont you like, under that name |
| **Roland MT-32** | `MT32_CONTROL.ROM` **and** `MT32_PCM.ROM` — both, or it is not an MT-32 |

If a file is missing the launch is **not** refused: the game plays through its default audio, and the message
names the file(s) that were wanted and the folder they go in. The device choices themselves come from the
recipe, so a new one is a data file rather than a rebuild.


## Amstrad CPC in detail

A CPC boots to BASIC and waits for you to type `RUN"` and a file name, which is why every CPC user learns
to type `CAT` first. You do not have to: **cap32 reads the disk's AMSDOS catalogue itself** and types the
command, using the convention the platform settled on — a file called `DISC.*` / `DISK.*` if there is one,
otherwise the only program on the disk, otherwise the first `.BAS`, then an extension-less file, then the
first `.BIN`.

EverythingBox does **not** override that with a command of its own. The core's own guess is backed by a
game database EverythingBox cannot see, so replacing it would trade a good answer for a guess. What the
app adds is the thing the core cannot: when the catalogue holds **nothing runnable at all** — the one case
where the core gives up and simply types `CAT` — you get a message saying so and telling you to type
`RUN"` followed by a name from the listing, instead of being left looking at a screen of file names with
no idea why the game did not start. The command the core is about to type is also written to the log for
every CPC disk, so a disk that boots the wrong thing can be diagnosed rather than guessed at.

If you want a *specific* command on a disk cap32 gets wrong, cap32 reads one from an `.m3u` playlist: put
the disk's file name on one line and `#COMMAND:RUN"THEGAME` on another, and open the `.m3u`.

## One entry per game

A retro-computer collection does not hold one file per game. The same title arrives as a WHDLoad `.lha` *and*
as a raw `.adf`; a big game arrives as four floppies, each its own file, each with its own name. Left alone
that is four tiles for one game, and two more for the game you already had. Two rules fix it, both driven by
the system's recipe, both applied while the library is scanned — nothing is moved, renamed or deleted.

### Format preference

A recipe can rank the formats its system's titles come in, best first:

```json
"formats": ["lha", "hdf", "adf", "adz", "dms"]
```

Files that are the **same title** in different listed formats become **one** library entry, on the
best-ranked format. The rest are not lost: they are that entry's **alternates**, reachable from its
"Other versions…" list, and the entry's details name the format it chose (`Format: LHA (also present: ADF)`).
"Same title" is the normalisation the rest of the library already uses — every `(…)`/`[…]` tag is stripped,
then punctuation and case — so `Lemmings (1991) (Psygnosis).lha` and `Lemmings (1991) (Psygnosis).adf` are
one game.

Three deliberate limits:

* a format **not** in the list keeps today's behaviour exactly: it is its own entry, never hidden. That is
  why `.ipf` is absent from the Amiga list — an IPF is a preservation dump whose whole point is to *not* be
  an `.adf`, and it needs `capsimg` to read at all;
* two files of the **same** listed format differ by region or revision, not by format. That is
  [1G1R region collapsing](#per-system)'s decision, which is off by default;
* a system with no `formats` list — which is every console — collapses nothing.

What ships, and why each order is what it is:

| System | Ranking | Why |
|---|---|---|
| **Amiga** | `lha` → `hdf` → `adf` → `adz` → `dms` | A WHDLoad `.lha` is installed-to-hard-disk: no model to choose, no disks to swap, and PUAE builds the image itself. An `.hdf` is the same idea you built by hand. `.adf` is the raw floppy every PUAE reads; `.adz` is that floppy gzipped; `.dms` is DiskMasher, which needs the core's own decompressor and does not survive every copy protection. |
| **Commodore 64** | `d64` → `prg` → `t64` → `tap` | The `.d64` disk image is the whole title — a multi-load game needs it. A `.prg` is one program VICE autostarts instantly. A `.t64` is a tape *archive* of the same program. A `.tap` is the real tape signal and loads in real time. `.crt` (a cartridge is a different product) and `.g64` (a GCR preservation dump) are deliberately unranked. |
| **ZX Spectrum** | `szx` → `z80` → `sna` → `tzx` → `tap` | A snapshot starts *at* the game instead of loading for three minutes: `.szx` is the lossless modern one, `.z80` the classic, `.sna` the 48K-limited lossy one. A `.tzx` carries the loader's own timing, so speedloaders work; a `.tap` is the simplified tape some of those loaders will not survive. The disk formats (+3 `.dsk`, TR-DOS `.trd`/`.scl`) are different *machines*, not different formats of one title, so they are not ranked. |

Every other system ships no ranking. Add one in your own recipe override if your collection wants it.

### Multi-disk sets

A multi-disk set is **one** entry that swaps disks itself, through the same generated `.m3u` playlist a
PlayStation set uses (issue #49). EverythingBox writes the playlist into its own cache — your ROM folder is
never written to — and the entry opens it; the core takes the disk list from there.

Console sets are named `(Disc 1)`, `(Disk 2)`, `(CD 3)`. Computer collections are named the way TOSEC names
them, which is different enough that it used to group nothing:

```
Monkey Island (1990) (Lucasfilm) (Disk 1 of 2).adf
Monkey Island (1990) (Lucasfilm) (Disk 2 of 2).adf
Maniac Mansion (1987) (Lucasfilm) (Side A).d64
Maniac Mansion (1987) (Lucasfilm) (Side B).d64
```

`(Disk N of M)` and `(Side A/B)` are read **only** for a system whose recipe sets `"tosecDiskTags": true`.
That is the gate: a console set that happens to be named `Game (Disk 1 of 2).bin` groups exactly as it did
before, because no console ships a recipe at all. Today the opted-in systems are **Amiga, Commodore 64,
Apple II, Atari ST and Amstrad CPC** — the disk-based ones, whose cores take a disk list and swap between
them. The **ZX Spectrum and MSX are not** opted in: their sets are tapes, and a tape is not a disk a core can
swap. Sides are ordered A then B, and a set with both orders disk 1 side A, disk 1 side B, disk 2 side A…

The two rules compose in that order — format first. A title present both as one `.lha` and as a two-disk
`.adf` set is a single entry on the `.lha`, with both disks as its alternates; with no `.lha` present, the
two disks become the one playlist entry.

## Changing any of this

Copy the shipped recipe out of the source tree (`native/systems/recipes/<id>.json`) into
`<data>/systems/recipes/`, edit it, restart. A user file replaces the shipped one outright. The fields are:

| Field | Meaning |
|---|---|
| `system` | the system id it applies to (must match the file name) |
| `summary` | one line, printed in the ROM folder's `README.txt` next to that system's folder |
| `folderIsGame` | a sub-folder holding a program is ONE library entry, not a tile per file |
| `formats` | the content formats this system's titles come in, **best first** — the same title in several of them is listed once, on the best (see below) |
| `tosecDiskTags` | read TOSEC disk-set names (`(Disk 1 of 2)`, `(Side A)`) as one multi-disk entry |
| `executables` | `extensions` that count as a program, plus `prefer` / `avoid` base names |
| `cores[]` | per core: `options` to seed, `firmware` to check, `content` presentation, `bootCommand` |

Inside a `cores[]` entry:

* `options` — core-option key/value pairs, applied **only where you have chosen nothing**. Your own per-core
  and per-game settings always win.
* `firmware` — a list of `{ purpose, files, md5?, required? }`. `files` is an any-of list: any one of them
  present satisfies the requirement, and names may contain a sub-path (`np2kai/bios.rom`). An entry with
  `"required": false` is documentation only and never refuses a launch.
* `content` — `{ when: file | folder | archive, present: asIs | executable | extract }`. Saying **nothing**
  about a shape means "behave as EverythingBox always has", which for an archive is *extract* — so handing a
  core an unextracted archive is something a recipe has to ask for in as many words.
* `conf` — the `dosbox.conf` translation for this core (issue #191): `unmappedNote` (the phrase used for a
  conf key with no mapping) and `map`, a list of `{ from, to, values?, transform?, note? }`. `from` is
  `section.key` in the conf; `to` is the core option. `values` is BOTH a translation table and a whitelist —
  a conf value it does not list is reported as ignored rather than passed at a core that has no setting for
  it. `transform` is `"cycles"` (the `cycles=` grammar) or `"none"`, which means "we know about this key and
  deliberately do not translate it" and reports the `note` instead.
* `midi` — the MT-32 / General MIDI assets (issue #191): `option` (the core option that selects a device)
  and `devices`, a list of `{ id, label, value, files, note }`. `files` is ALL-OF, unlike `firmware`'s
  any-of: an MT-32 needs both of its ROMs. `note` says why EverythingBox cannot provide the file, which is
  never the same reason twice. Nothing here is ever downloaded.
* `bootCommand` — how this core gets its typed boot command. `"amsdos"` means "read it out of the Amstrad
  disk's own catalogue"; empty means the core needs none. It names a *mechanism*, not a literal command,
  because the command is a property of the disk and a recipe is a property of the system.
