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

## Changing any of this

Copy the shipped recipe out of the source tree (`native/systems/recipes/<id>.json`) into
`<data>/systems/recipes/`, edit it, restart. A user file replaces the shipped one outright. The fields are:

| Field | Meaning |
|---|---|
| `system` | the system id it applies to (must match the file name) |
| `summary` | one line, printed in the ROM folder's `README.txt` next to that system's folder |
| `folderIsGame` | a sub-folder holding a program is ONE library entry, not a tile per file |
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
* `bootCommand` — how this core gets its typed boot command. `"amsdos"` means "read it out of the Amstrad
  disk's own catalogue"; empty means the core needs none. It names a *mechanism*, not a literal command,
  because the command is a property of the disk and a recipe is a property of the system.
