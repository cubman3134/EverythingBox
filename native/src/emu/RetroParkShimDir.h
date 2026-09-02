#pragma once
// Pure helper for RetroParkView's per-system shim directories (probe_retropark_content pins it).
//
// RetroPark's libretro shim is staged ONCE, into <coresDir>/libretro_shim. Every other system that needs a
// different libretro core gets its own sibling directory (N64 -> libretro_shim_n64) holding a COPY of
// LibretroShim.dll, so the runtime can load a directory whose core.json names that system's core. Those
// sibling directories are created on first use and then live across every later upgrade, which makes the
// question "is this copy still the shim we ship?" load-bearing rather than cosmetic.
//
// It has to be answered by CONTENT. The two things that look like cheaper answers are both wrong here:
//   * "copy only if absent" froze the N64 directory at whatever shim existed the day it was created. A fix
//     inside the shim then never reached N64 at all -- the app kept dying in the graphics driver with the
//     fixed DLL sitting one directory away, which reads as "the fix did not work" rather than "the fix was
//     never loaded".
//   * comparing SIZE would not have caught it either: the shim before and after that fix are both exactly
//     98816 bytes and differ only in their bytes.
//
// Deliberately Qt-free (std::ifstream, wide paths). probe_retropark_content is built by the retropark-windows
// CI job, which configures with the app gate OFF and therefore has no Qt at all; a QFile implementation here
// would link fine locally and fail that job.

#include <cstring>
#include <fstream>
#include <string>

namespace rpshim {

// Byte-for-byte equality. False if either file cannot be opened (a missing mirror is "not the same", which
// is the answer that makes the caller re-copy). Chunked, so it does not assume the file is small.
inline bool sameFileContents(const std::wstring& a, const std::wstring& b)
{
    std::ifstream fa(a.c_str(), std::ios::binary);
    std::ifstream fb(b.c_str(), std::ios::binary);
    if (!fa || !fb) return false;

    constexpr std::streamsize kChunk = 64 * 1024;
    std::string bufA(static_cast<size_t>(kChunk), '\0');
    std::string bufB(static_cast<size_t>(kChunk), '\0');
    for (;;) {
        fa.read(&bufA[0], kChunk);
        fb.read(&bufB[0], kChunk);
        const std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;                                   // one ran out first -> different lengths
        if (na == 0) return true;                                     // both hit EOF together
        if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(na)) != 0) return false;
    }
}

// Should the mirrored copy at dst be (re)written from src? Yes whenever it is missing or differs.
inline bool mirrorIsStale(const std::wstring& src, const std::wstring& dst)
{
    return !sameFileContents(src, dst);
}

} // namespace rpshim
