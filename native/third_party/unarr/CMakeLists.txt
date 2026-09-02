# Vendored, extraction-only RAR reader from (lib)unarr 1.1.1 (LGPL v3) — see README.EverythingBox.md for the
# upstream pin, the file subset and the one added forwarding header. Pure portable C99, no external
# dependency: the ZIP/TAR/7z readers upstream also ships are NOT built (this app already has one of each),
# and neither is upstream's optional bzip2/zlib/liblzma detection, none of which is on the RAR path.
add_library(unarr STATIC
    common/conv.c
    common/crc32.c
    common/stream.c
    common/unarr.c
    rar/filter-rar.c
    rar/huffman-rar.c
    rar/parse-rar.c
    rar/rar.c
    rar/rarvm.c
    rar/uncompress-rar.c)

# Consumers include <unarr.h> from here. rar/rar.h reaches "../lzmasdk/Ppmd7.h", which is the forwarding
# header in lzmasdk/ — it resolves relative to itself, so no extra include path is needed for the SDK.
target_include_directories(unarr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# RAR's PPMd method decodes through the LZMA SDK's original-var.H coder (Ppmd7a_*), which lives in the lzma7z
# target beside this one (Ppmd7aDec.c). Linking rather than re-compiling keeps ONE CPpmd7 in the binary.
target_link_libraries(unarr PUBLIC lzma7z)

# UNARR_IS_SHARED_LIBRARY is deliberately NOT defined: this is a static library, so unarr.h's UNARR_EXPORT
# expands to nothing and no dllimport/dllexport decoration is emitted.

# Third-party C: silence its warnings so a strict app build isn't tripped by library code.
if(MSVC)
    target_compile_options(unarr PRIVATE /w)
else()
    target_compile_options(unarr PRIVATE -w)
endif()
