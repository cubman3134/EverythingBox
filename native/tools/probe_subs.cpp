// Headless probe for the subtitle accuracy cores: the OpenSubtitles OSDb hash and the download cache.
// Prints SUBS-OK on success; any failure prints SUBS-FAIL <cond> (line) and exits non-zero.
#include "SubtitleHash.h"
#include "SubtitleCache.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QByteArray>
#include <cstdio>
#include <utility>   // std::swap

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SUBS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// An INDEPENDENT reference implementation of the OSDb hash, so the probe proves the real one rather than
// merely agreeing with itself: sum the file size with every little-endian quint64 in the two 64 KiB windows.
static QString refHash(const QByteArray& head, const QByteArray& tail, qint64 size)
{
    quint64 h = quint64(size);
    const auto addAll = [&h](const QByteArray& b) {
        for (int i = 0; i + 8 <= b.size(); i += 8) {
            quint64 w = 0;
            for (int k = 7; k >= 0; --k) w = (w << 8) | quint8(b.at(i + k));   // little-endian
            h += w;
        }
    };
    addAll(head); addAll(tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int W = 65536;

    // --- OSDb hash: pure core matches an independent implementation -------------------------------------
    {
        QByteArray head(W, '\0'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        const qint64 size = 734003200;                       // a plausible 700 MiB rip
        CHECK(SubtitleHash::ofBytes(head, tail, size) == refHash(head, tail, size));
        CHECK(SubtitleHash::ofBytes(head, tail, size).size() == 16);           // 16 hex digits, zero-padded
        CHECK(SubtitleHash::ofBytes(head, tail, size) ==
              SubtitleHash::ofBytes(head, tail, size).toLower());              // lowercase
        // Endianness is load-bearing: a byte-swapped window must NOT produce the same hash.
        QByteArray swapped = head;
        for (int i = 0; i + 8 <= swapped.size(); i += 8)
            for (int k = 0; k < 4; ++k) std::swap(swapped[i + k], swapped[i + 7 - k]);
        CHECK(SubtitleHash::ofBytes(swapped, tail, size) != SubtitleHash::ofBytes(head, tail, size));
        // KNOWN-ANSWER vector — the only assertion here that pins the little-endian CONVENTION rather than
        // mere within-word order sensitivity. refHash above is a structural twin of the implementation, so a
        // shared endianness mistake would satisfy every comparison-based check; this one would not. A single
        // word 01..08 with everything else zero must read back LSB-first.
        {
            QByteArray h8(W, '\0'), t0(W, '\0');
            for (int i = 0; i < 8; ++i) h8[i] = char(i + 1);       // bytes 01 02 03 04 05 06 07 08
            // little-endian => 0x0807060504030201 ; a big-endian impl would yield "0102030405060708".
            CHECK(SubtitleHash::ofBytes(h8, t0, 0) == QStringLiteral("0807060504030201"));
        }
        // Size participates.
        CHECK(SubtitleHash::ofBytes(head, tail, size + 1) != SubtitleHash::ofBytes(head, tail, size));
    }

    // --- OSDb hash: file path wrapper ------------------------------------------------------------------
    {
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString small = tmp.path() + QStringLiteral("/small.mkv");
        { QFile f(small); f.open(QIODevice::WriteOnly); f.write(QByteArray(1000, 'x')); f.close(); }
        CHECK(SubtitleHash::ofFile(small).isEmpty());                  // < 128 KiB ⇒ no valid hash
        CHECK(SubtitleHash::ofFile(tmp.path() + QStringLiteral("/missing.mkv")).isEmpty());

        const QString big = tmp.path() + QStringLiteral("/big.mkv");
        QByteArray head(W, '\0'), mid(4096, 'm'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        { QFile f(big); f.open(QIODevice::WriteOnly); f.write(head); f.write(mid); f.write(tail); f.close(); }
        const qint64 sz = qint64(W) * 2 + mid.size();
        CHECK(SubtitleHash::ofFile(big) == refHash(head, tail, sz));   // reads only the two windows
    }

    // --- SubtitleCache --------------------------------------------------------------------------------
    {
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString cachePath = tmp.path() + QStringLiteral("/subtitles.json");
        const QString srt = tmp.path() + QStringLiteral("/a.srt");
        { QFile f(srt); f.open(QIODevice::WriteOnly); f.write("1\n"); f.close(); }

        CHECK(SubtitleCache::keyFor(QStringLiteral("tt1375666"), QStringLiteral("en"))
              == QStringLiteral("tt1375666|en"));
        {
            SubtitleCache c(cachePath); c.load();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            c.save();
        }
        {
            SubtitleCache c(cachePath); c.load();                       // round-trip
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            // A picker choice OVERWRITES, so the correction sticks on replay.
            const QString srt2 = tmp.path() + QStringLiteral("/b.srt");
            { QFile f(srt2); f.open(QIODevice::WriteOnly); f.write("2\n"); f.close(); }
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt2);
            // A recorded file deleted behind our back reads as a MISS (self-healing ⇒ re-fetch).
            QFile::remove(srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.clear();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
        }
    }

    if (failures == 0) { std::puts("SUBS-OK"); return 0; }
    std::fprintf(stderr, "SUBS: %d check(s) failed\n", failures);
    return 1;
}
