// Pure, header-only, in-memory reader for POSIX/ustar tar archives — enough of the format to open a CBT
// comic (a .tar of page images). No disk, no Qt beyond QByteArray/QString, no throw: a malformed archive
// yields whatever parsed cleanly up to the damage and stops. This is deliberately the *reading* half only;
// nothing here writes a tar, so a fixture built by an independent writer (the probe hand-lays the 512-byte
// blocks) exercises this code without sharing any of it.
//
// Format, only the parts we need (a tar is a flat run of 512-byte blocks):
//   * each member is a 512-byte header block, then its data padded up to the next 512-byte boundary;
//   * name    = header[0..100)   NUL-terminated;
//   * size    = header[124..136) ASCII octal (space/NUL terminated);
//   * typeflag= header[156]      '0' or '\0' = regular file, '5' = directory, others = link/special;
//   * magic   = header[257..263) "ustar\0" (POSIX) or "ustar " (old GNU); absent in the pre-ustar format;
//   * prefix  = header[345..500) NUL-terminated — when set (ustar only), the full name is prefix + '/' + name;
//   * the archive ends at the first all-zero header block (the canonical terminator is two zero blocks; the
//     first is enough to stop the walk).
// Directory and non-regular entries are ignored. Sizes larger than octal can express (GNU base-256) are not
// needed for comic pages and are not decoded.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstring>

namespace Tar
{
    struct TarEntry
    {
        QString name;            // full member name (prefix + '/' + name when ustar prefix is set)
        qint64  size = 0;        // declared uncompressed size, in bytes
        qint64  dataOffset = 0;  // byte offset of this member's data within the tar buffer
    };

    namespace detail
    {
        // A NUL-terminated (or maxLen-bounded) field, decoded as UTF-8.
        inline QString field(const char* p, int maxLen)
        {
            int len = 0;
            while (len < maxLen && p[len] != '\0') ++len;
            return QString::fromUtf8(p, len);
        }

        // An ASCII-octal numeric field: skip leading spaces, then read octal digits until a space, a NUL, or
        // the field end. A field with no digits reads as 0.
        inline qint64 octal(const char* p, int maxLen)
        {
            int i = 0;
            while (i < maxLen && p[i] == ' ') ++i;
            qint64 v = 0;
            for (; i < maxLen; ++i)
            {
                const char c = p[i];
                if (c < '0' || c > '7') break;
                v = v * 8 + (c - '0');
            }
            return v;
        }

        inline bool blockIsZero(const char* p)
        {
            for (int i = 0; i < 512; ++i)
                if (p[i] != 0) return false;
            return true;
        }
    }

    // Walk the header blocks and return every regular-file member. Stops at the zero-block terminator or the
    // end of the buffer, whichever comes first; a truncated tail yields whatever was parsed before it. Never
    // throws.
    inline QVector<TarEntry> listEntries(const QByteArray& tar)
    {
        QVector<TarEntry> entries;
        const char* d = tar.constData();
        const qint64 n = tar.size();
        qint64 pos = 0;

        while (pos + 512 <= n)
        {
            const char* h = d + pos;
            if (detail::blockIsZero(h)) break; // end-of-archive marker

            QString name = detail::field(h, 100);
            const bool ustar = std::memcmp(h + 257, "ustar", 5) == 0;
            if (ustar)
            {
                const QString prefix = detail::field(h + 345, 155);
                if (!prefix.isEmpty()) name = prefix + QLatin1Char('/') + name;
            }
            const qint64 size = detail::octal(h + 124, 12);
            const char typeflag = h[156];
            const qint64 dataOffset = pos + 512;

            // '0' and the historical '\0' both mean an ordinary file; '5' is a directory, others are links or
            // devices. Only ordinary files carry page images, so they are all we surface.
            const bool regular = (typeflag == '0' || typeflag == '\0');
            if (regular && !name.isEmpty() && size >= 0)
                entries.append(TarEntry{ name, size, dataOffset });

            if (size < 0) break; // malformed length — stop rather than seek backwards
            const qint64 next = dataOffset + ((size + 511) / 512) * 512; // data padded to 512
            if (next <= pos) break; // no forward progress — malformed; do not loop
            pos = next;
        }
        return entries;
    }

    // The member's raw bytes. QByteArray::mid() clamps the length to the bytes actually present, so a
    // truncated final entry returns the bytes that survived and never reads past the buffer. The leading
    // guard is a defensive validity check on a malformed entry (bad offset / non-positive size) — it returns
    // empty rather than indexing out of range.
    inline QByteArray extractEntry(const QByteArray& tar, const TarEntry& e)
    {
        if (e.dataOffset < 0 || e.dataOffset >= tar.size() || e.size <= 0)
            return QByteArray();
        return tar.mid(e.dataOffset, e.size);
    }
}
