// The OpenSubtitles "OSDb" movie hash — the highest-accuracy way to match a subtitle to an EXACT release
// (so the timings actually line up with this rip). It is NOT a cryptographic digest: it is filesize plus the
// sum of every little-endian 64-bit word in the first and last 64 KiB, truncated to 64 bits, printed as 16
// lowercase hex digits. Files under 128 KiB (two windows) have no valid hash.
#pragma once
#include <QByteArray>
#include <QString>

namespace SubtitleHash
{
    QString ofFile(const QString& path);
    // Pure core (probe-tested): head and tail are each exactly 65536 bytes; size is the true file size.
    QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size);
}
