# Installing a Wii file-replacement mod by composing a patched disc

**Date:** 2026-08-28
**Status:** approved, not yet implemented
**Repo boundary:** this is the PUBLIC client repo. Nothing here names a content source. The mod and
the game it patches are named because they are the measured subject; where the distribution came
from is deliberately absent and must stay absent.

## The problem

The romhack install path assumes a hack is a *patch*: apply it to a base ROM, write the result into
the ROMs folder, and from that moment the hack is an ordinary local game. `RomhackInstall`'s own
header states it plainly — the alternative, a virtual entry patched at launch, "would have meant
threading a new id through every per-item store".

A large class of Wii mods does not fit. They ship no patch at all. They ship a **replacement file
tree** plus a **Riivolution XML** that says where each folder belongs on the disc. Riivolution is a
loader that performs that substitution at runtime on real hardware. There is nothing for `RomPatch`
to apply, so these mods can be listed on the romhack shelf and cannot be installed — which is the
state Super Mario Gravity, a Super Mario Galaxy 2 mod, is in today.

## What was measured

All of the following was established by inspection on 2026-08-28, not assumed.

**The mod's own shape.** Its distribution carries three things: a `Riivolution/` XML, a
`SuperMarioGravity_Demo/` replacement tree, and a `Patcher/` directory holding `wit.exe` (Wiimms ISO
Tools), Cygwin runtime DLLs and a `.bat`. The batch script does exactly three things:

```
wit.exe extract SMG2.iso ISOfiles/
XCOPY SuperMarioGravity_Demo  Patcher\ISOfiles\DATA\files  /Y /E /S /Q
wit.exe copy Patcher\ISOfiles\ "Super Mario Gravity - DEMO [KB4P01].wbfs" --id K
```

Extract the disc, overlay the tree, rebuild the image. **The author already treats a composed disc
as the supported route for Dolphin**; Riivolution is the route for real hardware. So this design is
not inventing an approach — it is automating the one the mod ships.

The XML declares the same overlay declaratively: `root="/SuperMarioGravity_Demo"` and a set of
`<folder disc="/StageData" external="StageData" create="true"/>` mappings, plus aliases where seven
locale directories all resolve to `EuEnglish`, and one `<savegame>` element.

**Dolphin cannot be driven into Riivolution from outside.** The shipped build contains the whole
feature — `RiivolutionBootWidget`, `RiivolutionPatcher`, `AddRiivolutionPatches` — but its command
line offers only `--batch`, `--config`, `--exec`, `--movie`, `--save_state`. There is no
`--riivolution`. The only persisted key is `GuiRiivolutionPatchIndex`, the dialog's last selection.
A launch-time route would require a human to click a dialog on every start, which a
controller-driven UI with no dialogs cannot do.

**`DolphinTool` extracts but will not compose from a directory.**

| operation | result |
|---|---|
| `DolphinTool extract -i <disc> -o <dir> -g` | works; 2.6 s on a GameCube disc, yielding `sys/` + `files/` |
| `DolphinTool convert -i <dir> -o <out> -f rvz` | **`Error: The input file could not be opened.`** |
| `DolphinTool verify` / `header` on a directory | same refusal |
| `Dolphin.exe -b -e <dir>` | **claim RETRACTED — see below** |

**A retraction.** An earlier draft of this spec said `Dolphin.exe -b -e <dir>` boots a bare directory,
on the evidence that the process was still alive eight seconds after launch. That evidence does not
support the claim: Dolphin is a GUI application, and one showing a modal error dialog is equally
still alive. The implementation pass then established the mechanism, which contradicts it outright —
`DirectoryBlobReader`'s own `IsValidDirectoryBlob` accepts only a path ending in `/sys/main.dol`, so a
directory was never going to be accepted by any code path. What is true is narrower and is what the
design actually rests on: `DirectoryBlobReader` can read an extracted disc when handed that file.

**And the reason for that refusal is a single early-out.** `DiscIO::CreateBlobReader` already handles
extracted discs — `DirectoryBlobReader::Create(filename)` sits in its `default:` branch — but the
function opens the path as a file and reads a 4-byte magic *first*, returning `nullptr` for a
directory before the switch is ever reached. The conversion pipeline downstream is blob- and
volume-based and format-agnostic.

This is the finding the whole design rests on: **Dolphin can already compose a Wii disc from a
directory, including FST, hash tree and encryption. Only its input helper says no.** Measured in
implementation: the entry point is `DirectoryBlobReader::Create(<dir>/sys/main.dol)`, not the
directory itself, and the fix must resolve one to the other.

**Composed images are not bit-identical to their source, by construction.** A disc rebuilt from an
extracted tree differs from the original even before any overlay is applied — measured at ~10 MB
larger with a different SHA-1. That is inherent to composing rather than a defect, but it means a
composed disc will not be recognised by anything that identifies games by whole-file hash.

## Approach

Four offline stages producing one file:

```
base ROM ──extract──> staging tree ──overlay──> patched tree ──compose──> .rvz in the ROMs folder
```

The output is an ordinary disc image. The library scan finds it, and tiles, saves, save states,
marks, tags, favourites and scraping work with no new plumbing — the property `RomhackInstall`
exists to preserve.

Two alternatives were rejected on measured grounds. **A launch-time Riivolution overlay** is what
Riivolution is actually for and would cost no disk, but the shipped Dolphin cannot be told to do it
without a dialog. **An extracted directory kept as the installed game** would work — Dolphin boots
one — but `RomLibrary::scan` enumerates files only, recursing into subdirectories, so a disc tree in
the ROMs folder would never be an entry and its thousands of files would be walked on every scan.

### The disc tool

One new binary, built from the Dolphin source already vendored in RetroPark, invoked as a subprocess
the way the app already invokes emulators. Its delta from stock `DolphinTool` is a directory check in
`CreateBlobReader` ahead of the magic read, so an extracted tree reaches the `DirectoryBlobReader`
branch that is already written. Nothing about encryption, hashing, FST layout or container format is
implemented here; that is Dolphin's code, exercised by every disc it boots.

It is a subprocess and not a linked library because the app already shells out to emulator binaries,
and because DiscIO drags in Common, mbedtls and zstd.

### Components

- **`RiivolutionPatch`** — XML to a list of overlay operations. Pure; no I/O. Table-tested against
  the real `SuperMarioGravity_Demo.xml`, including the seven-way locale aliasing.
- **`DiscOverlay`** — applies operations to an extracted tree. File operations only, with path
  containment enforced so no `external` or `disc` attribute can write outside the staging root.
- **`DiscCompose`** — the subprocess wrapper: extract, then compose. Owns staging, the free-space
  check and cleanup.
- **`RomhackInstall`** — unchanged. It already names and places the finished file.

### What is honoured, and what is refused

`<folder>` and `<file>` are honoured. `<option>`/`<choice>` selection is resolved to decide which
`<patch>` blocks apply.

**`<savegame>` is ignored**, and the ignoring is recorded rather than silent: it redirects saves at
runtime and has no meaning in a composed disc.

**`<memory>` patches are a refusal.** They are runtime RAM writes and *cannot* be represented in a
disc image. A mod using them would compose cleanly, boot, and be subtly wrong — the worst available
outcome. The mod at hand uses none, so refusing costs nothing today and forecloses a class of silent
breakage. A refusal names the element and says the mod needs a loader this install path is not.

### Failure modes

**Disk is the sharp edge.** The measuring machine had **6.7 GB free** against a base game stored as a
1.4 GB archive whose decompressed disc, extracted tree and output image must coexist. The tool
estimates the requirement and refuses *before* starting rather than dying mid-compose. Staging lives
outside the ROMs folder, so a half-made disc is never scanned as a game, and every exit path cleans
up.

**The base ROM is never modified.** It is read to extract and not written, and the composed disc is a
new file, matching the guarantee the existing install path already makes.

A refusal at any stage leaves nothing behind that looks playable.

## Testing

`RiivolutionPatch` and `DiscOverlay` are unit-tested with no disc and no network: the real XML's
folder mappings and locale aliases, a `<memory>` document refused, a `<savegame>` document ignored
with the fact recorded, and a hostile `external`/`disc` pair that tries to escape the staging root.

**The end-to-end gate is not "it boots".** A stock disc also boots, so booting proves only that
composition produced a valid image, not that the overlay landed. The gate is: compose Super Mario
Gravity against Super Mario Galaxy 2; confirm the base ROM is byte-identical afterwards; confirm the
composed disc boots; and confirm the mod's **own title screen** appears — the overlay replaces
`LayoutData/TitleLogo.arc`, so the title screen is the cheapest place the substitution becomes
visible.

## Out of scope

- Riivolution at runtime, on hardware or through an in-process core.
- Dolphin texture packs, a third install shape found in the same census.
- Any change to how the romhack capability stages or serves files.
- Any change to `RomLibrary` scanning. This design exists partly to avoid needing one.
