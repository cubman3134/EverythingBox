# Vendored (lib)unarr — RAR reader only

## What this is

The RAR extraction half of [(lib)unarr](https://github.com/selmf/unarr), vendored so `.cbr` comic archives
open in the reader (issue #144). unarr began as a port of The Unarchiver's RAR extraction written for exactly
this purpose — reading page images out of comic-book archives — which is why it is the choice here rather
than libarchive.

* **Upstream:** https://github.com/selmf/unarr
* **Version:** 1.1.1, released 2023-10-23 (tag `v1.1.1`)
* **Source archive:** `https://github.com/selmf/unarr/archive/refs/tags/v1.1.1.tar.gz`
  SHA-256 `fa0ebf6d9b420d34171b1b6100949edce708c7933e7cfd2cedd03eae998d1c53`
* **Licence:** LGPL v3 — the full text is in `COPYING`, the authors in `AUTHORS`, and the `NOTICE` at the
  repository root records it alongside the other vendored components.

## What was taken, and what was left

Taken: the public header (`unarr.h`), the common layer (`common/`) and the RAR reader (`rar/`).

Left behind: `zip/`, `tar/`, `_7z/`, the whole bundled `lzmasdk/` (see below), the tests, the fuzzer, the
corpus and upstream's build system. This app already reads ZIP with miniz, TAR with `src/comic/Tar.h` and 7z
with the vendored LZMA SDK, and a second implementation of any of them would be a second page order and a
second set of bugs. `common/` is decoupled from the formats — it references no reader — so the RAR subset
builds on its own.

`common/custalloc.c` is also left out: upstream's own `CMakeLists.txt` has it commented out, and it exists
only for callers that want to install a custom allocator through `ar_set_custom_allocator`.

## Modifications to upstream source

**None.** Every `.c` and `.h` under `common/` and `rar/`, plus `unarr.h`, `COPYING` and `AUTHORS`, is
byte-for-byte as released.

One file is *added* rather than modified: `lzmasdk/Ppmd7.h`, a forwarding header that answers `rar/rar.h`'s
`#include "../lzmasdk/Ppmd7.h"` by pointing at this project's existing LZMA SDK copy (`../../lzma/Ppmd7.h`,
version 26.02). Upstream's bundled Ppmd7 header is byte-identical to it apart from line endings; sharing the
one copy avoids a second `Ppmd7.c` in the binary, which would be a duplicate-symbol link failure. The RAR
PPMd method also needs the original PPMd var.H range decoder (`Ppmd7a_*`), which 7-Zip's read-only sample does
not compile — `Ppmd7aDec.c` was added to `native/third_party/lzma` from the same 26.02 source for that.

## Build

`CMakeLists.txt` here is this project's, not upstream's: one static library, no shared-library export macros,
no system bzip2/zlib/liblzma detection (none of them is on the RAR path), warnings silenced the way the other
vendored C targets are. Nothing here is compiled into the app except through the `unarr` target.

## Known limitation: RAR5

**unarr 1.1.1 does not read RAR5 archives** (`Rar!\x1A\x07\x01\x00`), only RAR 2.9/3.x/4.x
(`Rar!\x1A\x07\x00`). Upstream's README says so and upstream `master` still has no RAR5 reader. `src/comic/
RarComic.cpp` therefore sniffs the signature itself, ahead of unarr, and a RAR5 `.cbr` is refused with a
message that names the format rather than reading as a generic "unreadable archive".
