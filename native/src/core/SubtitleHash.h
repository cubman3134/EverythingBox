// The OpenSubtitles "OSDb" movie hash — the highest-accuracy way to match a subtitle to an EXACT release
// (so the timings actually line up with this rip). It is NOT a cryptographic digest: it is filesize plus the
// sum of every little-endian 64-bit word in the first and last 64 KiB, truncated to 64 bits, printed as 16
// lowercase hex digits. Files under 128 KiB (two windows) have no valid hash.
#pragma once
#include <QByteArray>
#include <QString>

namespace SubtitleHash
{
    // NOT THREAD-SAFE: ofFile memoises its last result (the same file is hashed twice per open) in
    // unsynchronised file-statics. GUI-thread use only — moving this to a worker thread needs a lock first.
    QString ofFile(const QString& path);
    // Pure core (probe-tested): head and tail must each be exactly 65536 bytes (any other length returns an
    // empty QString rather than a hash no server could match); size is the true file size.
    QString ofBytes(const QByteArray& head, const QByteArray& tail, qint64 size);
}
