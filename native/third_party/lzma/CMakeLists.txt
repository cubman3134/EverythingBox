# Vendored, read-only .7z decoder from the public-domain LZMA SDK (7-Zip), version 26.02.
# Pure portable C — builds on every platform the app targets (Windows/macOS/Linux/Android/iOS) with no
# external dependency. This is exactly the file set the SDK's own "Util/7z" sample compiles for read-only
# extraction (LZMA/LZMA2/BCJ/BCJ2/Delta/PPMd filters + CRC), nothing encoder- or encryption-related.
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
    Ppmd7Dec.c)

# Consumers (SevenZip.cpp) include "7z.h" etc. from here.
target_include_directories(lzma7z PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Third-party C: silence its warnings so a strict (-Werror) app build isn't tripped by SDK code.
if(MSVC)
    target_compile_options(lzma7z PRIVATE /w)
else()
    target_compile_options(lzma7z PRIVATE -w)
endif()
