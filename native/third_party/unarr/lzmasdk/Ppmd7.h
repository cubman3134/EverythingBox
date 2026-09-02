/* NOT unarr's own file, and NOT a copy of the LZMA SDK's Ppmd7.h. It is a one-line forwarding header, and it
   is the whole of the modification this project makes to the vendored unarr sources.

   unarr's rar/rar.h includes "../lzmasdk/Ppmd7.h" for RAR's PPMd method. This project ALREADY vendors the
   LZMA SDK, one directory over, for the .7z reader (native/third_party/lzma, version 26.02) — and unarr's own
   bundled copy is byte-identical in that header apart from line endings. Shipping unarr's copy as well would
   put a second Ppmd7.c/Ppmd7Dec.c in the same binary: a duplicate-symbol link failure at best, and at worst
   two CPpmd7 layouts if the two SDK versions ever drifted.

   So the include is answered HERE, by pointing at the one copy. unarr's sources are otherwise byte-for-byte
   as released (see README.EverythingBox.md). The RAR-only decoder Ppmd7aDec.c — which 7-Zip's own read-only
   sample does not compile, because .7z uses the other range coder — was added to the lzma target beside it. */
#ifndef EVERYTHINGBOX_UNARR_PPMD7_FORWARD_H
#define EVERYTHINGBOX_UNARR_PPMD7_FORWARD_H

#include "../../lzma/Ppmd7.h"

#endif
