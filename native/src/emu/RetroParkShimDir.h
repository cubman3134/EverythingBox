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

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>

namespace rpshim {

// Byte-for-byte equality. False if either file cannot be opened (a missing mirror is "not the same", which
// is the answer that makes the caller re-copy). Chunked, so it does not assume the file is small.
inline bool sameFileContents(const QString& a, const QString& b)
{
    QFile fa(a), fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly)) return false;
    if (fa.size() != fb.size()) return false;
    for (;;) {
        const QByteArray ca = fa.read(64 * 1024);
        const QByteArray cb = fb.read(64 * 1024);
        if (ca != cb) return false;
        if (ca.isEmpty()) return true;   // both reached EOF together (sizes already matched)
    }
}

// Should the mirrored copy at dst be (re)written from src? Yes whenever it is missing or differs.
inline bool mirrorIsStale(const QString& src, const QString& dst)
{
    return !sameFileContents(src, dst);
}

} // namespace rpshim
