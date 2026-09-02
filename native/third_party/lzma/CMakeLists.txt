# Vendored, read-only .7z decoder from the public-domain LZMA SDK (7-Zip), version 26.02.
# Pure portable C — builds on every platform the app targets (Windows/macOS/Linux/Android/iOS) with no
# external dependency. This is exactly the file set the SDK's own "Util/7z" sample compiles for read-only
# extraction (LZMA/LZMA2/BCJ/BCJ2/Delta/PPMd filters + CRC), nothing encoder- or encryption-related.
#
# ONE FILE BEYOND THAT SAMPLE: Ppmd7aDec.c, the ORIGINAL PPMd var.H range decoder (Ppmd7a_*), which 7-Zip
# itself does not need because .7z stores PPMd with the 7z range coder (Ppmd7z_*, in Ppmd7Dec.c). RAR's
# PPMd method uses the original coder, so the vendored unarr RAR reader (issue #144, third_party/unarr)
# calls into it. It is the SAME 26.02 source as its siblings and shares their CPpmd7 struct, which is why
# it belongs HERE rather than as a second copy inside unarr: two Ppmd7.c objects in one binary would be a
# duplicate-symbol link error, and a second copy that merely compiled would be a struct-layout landmine.
add_library(lzma7z STATIC
    7zAlloc.c
    7zArcIn.c
    7zBuf.c
    7zCrc.c
    7zCrcOpt.c
    7zDec.c
    7zFile.c
    7zStream.c
    Bcj2.c
    Bra.c
    Bra86.c
    BraIA64.c
    CpuArch.c
    Delta.c
    Lzma2Dec.c
    LzmaDec.c
    Ppmd7.c
    Ppmd7Dec.c
    Ppmd7aDec.c)

# Consumers (SevenZip.cpp) include "7z.h" etc. from here.
target_include_directories(lzma7z PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Third-party C: silence its warnings so a strict (-Werror) app build isn't tripped by SDK code.
if(MSVC)
    target_compile_options(lzma7z PRIVATE /w)
else()
    target_compile_options(lzma7z PRIVATE -w)
endif()
