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
| **Amstrad CPC** | Yes. cap32 auto-runs disk and tape content. | None — the CPC firmware is compiled into the core. | |
| **Apple II** | Yes. | None — the Apple II ROMs are compiled into the core. | The recipe selects the enhanced //e, which is the machine most software expects. |
| **NEC PC-98** | Not until you supply the ROMs. | `np2kai/bios.rom` and a font ROM (`np2kai/font.rom` or `np2kai/font.bmp`); several titles also want `np2kai/itf.rom` and `np2kai/sound.rom`. | The core cannot boot at all without these, so the launch is refused with the file names rather than left at a black screen. |
| **Sharp X1** | Not until you supply the IPL ROM. | `xmil/IPLROM.X1` (the turbo machine types also use `xmil/IPLROM.X1T`). | The core falls back to a stub boot ROM, which is why an X1 disk usually shows a blank screen. |

MSX is **not** in this table: EverythingBox's system catalog has no `msx` system id yet (blueMSX appears only
as a fallback core for SG-1000 and ColecoVision), so a recipe for it would be data nothing could reach. Adding
the system is a prerequisite, and blueMSX additionally requires `Machines/` and `Databases/` folders in the
system folder.

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
* `bootCommand` — reserved for the cores that take one; unused today.
