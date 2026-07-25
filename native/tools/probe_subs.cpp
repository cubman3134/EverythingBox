// Headless probe for the subtitle accuracy cores: the OpenSubtitles OSDb hash, the match chain (tier ORDER
// and which tiers are emitted at all), the cache-identifier precedence, and the download cache.
// Prints SUBS-OK on success; any failure prints SUBS-FAIL <cond> (line) and exits non-zero.
#include "SubtitleHash.h"
#include "SubtitleCache.h"
#include "SubtitleFetcher.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QByteArray>
#include <QStringList>
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
        // Precondition guard: a window that is not exactly 64 KiB has NO valid hash (a short read must not
        // produce a plausible-looking digest that the server can never match).
        CHECK(SubtitleHash::ofBytes(head.left(W - 8), tail, size).isEmpty());
        CHECK(SubtitleHash::ofBytes(head, tail.left(W - 8), size).isEmpty());
        CHECK(SubtitleHash::ofBytes(QByteArray(), QByteArray(), size).isEmpty());
        CHECK(SubtitleHash::ofBytes(head + QByteArray(8, '\0'), tail, size).isEmpty());   // too LONG, too
    }

    // --- OSDb hash: file path wrapper ------------------------------------------------------------------
    // The fixture dir outlives this block: the match-chain and cacheIdentifier sections below need a hashable
    // and an unhashable file on disk to prove tier gating. Hermetic — everything lives under QTemporaryDir.
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QString smallPath = tmp.path() + QStringLiteral("/small.mkv");
    const QString bigPath   = tmp.path() + QStringLiteral("/big.mkv");
    {
        { QFile f(smallPath); f.open(QIODevice::WriteOnly); f.write(QByteArray(1000, 'x')); f.close(); }
        CHECK(SubtitleHash::ofFile(smallPath).isEmpty());              // < 128 KiB ⇒ no valid hash
        CHECK(SubtitleHash::ofFile(tmp.path() + QStringLiteral("/missing.mkv")).isEmpty());

        QByteArray head(W, '\0'), mid(4096, 'm'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        { QFile f(bigPath); f.open(QIODevice::WriteOnly); f.write(head); f.write(mid); f.write(tail); f.close(); }
        const qint64 sz = qint64(W) * 2 + mid.size();
        CHECK(SubtitleHash::ofFile(bigPath) == refHash(head, tail, sz));   // reads only the two windows

        // ofFile memoizes its last result (the same file is hashed twice per open). Two things must hold:
        // a repeat call agrees with the first, AND a file rewritten underneath us re-hashes rather than
        // serving a stale digest — the memo key carries size + mtime, so a different size invalidates it.
        CHECK(SubtitleHash::ofFile(bigPath) == SubtitleHash::ofFile(bigPath));      // stable
        const QString before = SubtitleHash::ofFile(bigPath);
        {
            QByteArray h2(W, '\0'), m2(8192, 'z'), t2(W, '\0');
            for (int i = 0; i < W; ++i) { h2[i] = char((i * 3) % 253); t2[i] = char((i * 11) % 239); }
            QFile f(bigPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate);   // different bytes AND size
            f.write(h2); f.write(m2); f.write(t2); f.close();
            CHECK(SubtitleHash::ofFile(bigPath) == refHash(h2, t2, qint64(W) * 2 + m2.size()));
        }
        CHECK(SubtitleHash::ofFile(bigPath) != before);                // the memo did NOT serve a stale hash
    }

    // --- the match chain: order + gating -----------------------------------------------------------
    // buildQueries IS the accuracy feature: which tiers are emitted, and in what order. Pure (strings in,
    // strings out; the only I/O is hashing the path it's handed), so it's asserted directly here.
    {
        // A stream (no local path): imdb then title, never a hash tier.
        const QStringList s = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), QString());
        CHECK(s.size() == 2);
        CHECK(s.at(0).contains(QStringLiteral("imdb_id=1375666")));
        CHECK(s.at(1).contains(QStringLiteral("query=Inception")));
        for (const QString& q : s) CHECK(q.contains(QStringLiteral("languages=en")));
        // An episode id expands to parent+season+episode.
        const QStringList e = SubtitleFetcher::buildQueries(QStringLiteral("tt0903747:2:5"),
                                  QString(), QStringLiteral("en"), QString());
        CHECK(e.size() == 1);
        CHECK(e.at(0).contains(QStringLiteral("parent_imdb_id=903747")));
        CHECK(e.at(0).contains(QStringLiteral("season_number=2")));
        CHECK(e.at(0).contains(QStringLiteral("episode_number=5")));
        // The API's imdb ids are NUMERIC: the "tt" prefix goes AND the zero padding goes with it. Most
        // pre-2000 titles are zero-padded, so leaving the padding on would blunt the imdb tier for them.
        CHECK(!e.at(0).contains(QStringLiteral("parent_imdb_id=0")));
        // The language filter rides EVERY tier, episodes included — a tier that dropped it would answer with
        // subtitles in some other language and the auto-pick would load them.
        for (const QString& q : e) CHECK(q.contains(QStringLiteral("languages=en")));
        CHECK(SubtitleFetcher::buildQueries(QStringLiteral("tt0110912"), QString(), QStringLiteral("en"),
                  QString()).at(0).contains(QStringLiteral("imdb_id=110912")));
        // Nothing to search on ⇒ no queries at all (no blind, unmatchable request).
        CHECK(SubtitleFetcher::buildQueries(QString(), QString(), QStringLiteral("en"), QString()).isEmpty());
        // A hashable local file puts the moviehash tier FIRST, ahead of imdb — the exact-rip match wins.
        const QStringList h = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), bigPath);
        CHECK(h.size() == 3);
        CHECK(h.at(0).contains(QStringLiteral("moviehash=")));
        CHECK(h.at(1).contains(QStringLiteral("imdb_id=")));
        CHECK(h.at(2).contains(QStringLiteral("query=Inception")));
        // …and on all three tiers of the hashable case, the moviehash tier especially (it is the one built
        // from a different branch, so it is the one that could silently lose the filter).
        for (const QString& q : h) CHECK(q.contains(QStringLiteral("languages=en")));
        // An UNHASHABLE local file (< 128 KiB) must NOT emit a hash tier — a bogus hash matches nothing.
        const QStringList u = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), smallPath);
        CHECK(u.size() == 2);
        CHECK(!u.at(0).contains(QStringLiteral("moviehash=")));
        CHECK(u.at(0).contains(QStringLiteral("imdb_id=")));
    }

    // --- cacheIdentifier precedence ----------------------------------------------------------------
    // The download cache must key on whichever tier will ACTUALLY match, or a hit would replay a subtitle
    // matched by a coarser tier than the one this open would use.
    {
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), bigPath)
                  .startsWith(QStringLiteral("hash:")));                        // hashable ⇒ hash wins
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), QString())
                  == QStringLiteral("tt1"));                                    // no path ⇒ imdb
        CHECK(SubtitleFetcher::cacheIdentifier(QString(), QStringLiteral("T"), QString())
                  == QStringLiteral("title:T"));                                // neither ⇒ title
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), smallPath)
                  == QStringLiteral("tt1"));                                    // unhashable ⇒ falls to imdb
        // The last rung: an unhashable path AND no imdb id ⇒ all the way down to the title. (A "hash:" here
        // would key on a digest no search ever used; an empty key would collide across every such video.)
        CHECK(SubtitleFetcher::cacheIdentifier(QString(), QStringLiteral("T"), smallPath)
                  == QStringLiteral("title:T"));
    }

    // --- SubtitleCache --------------------------------------------------------------------------------
    {
        QTemporaryDir ctmp; CHECK(ctmp.isValid());   // its own dir (the outer `tmp` holds the hash fixtures)
        const QString cachePath = ctmp.path() + QStringLiteral("/subtitles.json");
        const QString srt = ctmp.path() + QStringLiteral("/a.srt");
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
            const QString srt2 = ctmp.path() + QStringLiteral("/b.srt");
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
