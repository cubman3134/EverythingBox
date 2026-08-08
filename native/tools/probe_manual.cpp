// Headless verification of the on-demand GAME MANUAL wiring (issue #89): the "manual" art role merges by
// precedence exactly like every other role, but the megabyte PDF is NEVER pulled by saveArt's eager prefetch
// (hover / console-entry) — only by an explicit fetchManual on open. Three independent things are proven:
//
//   1. MERGE BY PRECEDENCE (pure). Two providers' MediaArt merged high-first via mergeLowerPriority — the very
//      primitive GameMetaAggregator::finishJob uses. The present provider's manual wins by precedence; when the
//      higher provider has none, the lower one's manual backfills (absent-provider-ignored). Oracle by hand.
//   2. PREFETCH EXCLUSION (behavioural, over a loopback HTTP server). saveArt() on a bundle carrying BOTH a
//      "box" role and a "manual" role fetches the box but must NOT request the manual — while still recording
//      the manual URL in the bundle for a later open. The server counts hits, so a manual accidentally pulled
//      into prefetch is a hit that fails the assertion (this is the download-MB-on-every-hover regression).
//   3. ON-DEMAND FETCH (behavioural). fetchManual() pulls the PDF into the item's MetaCache folder, reports
//      progress, hands back the local path, and — second time — serves the cached file with no network.
//
// The fixture bytes are defined HERE (not derived from the code under test), so the stored file is checked for
// exact equality against an independent oracle. Prints MANUAL-OK; MANUAL-FAIL <what> + non-zero on failure.
#include "AddonModels.h"
#include "MetaCache.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <memory>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "MANUAL-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

// The fixture payloads. Defined independently of anything the app computes, so a stored file can be checked
// byte-for-byte against them.
static const QByteArray kPdfBytes = QByteArrayLiteral("%PDF-1.4\n% fixture game manual payload\n%%EOF\n");
static const QByteArray kBoxBytes = QByteArrayLiteral("\xFF\xD8\xFF\xE0 fixture jpeg-ish box bytes");

// A minimal loopback HTTP server on an OS-chosen port. It records the path of every request it serves, so the
// prefetch-exclusion assertion is simply "the manual path was never requested".
struct Loopback
{
    QTcpServer srv;
    QStringList requested;   // every path served, in order (the whole exclusion assertion reads this)

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
                buf->append(c->readAll());
                const int end = buf->indexOf("\r\n\r\n");
                if (end < 0) return; // head not complete yet
                const QByteArray reqLine = buf->left(end).split('\n').value(0).trimmed();
                const QByteArray path = reqLine.split(' ').value(1);
                requested << QString::fromLatin1(path);
                const QByteArray body = path.endsWith(".pdf") ? kPdfBytes : kBoxBytes;
                const char* ctype = path.endsWith(".pdf") ? "application/pdf" : "image/jpeg";
                QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: ";
                resp += ctype;
                resp += "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
                resp += body;
                c->write(resp);
                c->flush();
                c->disconnectFromHost();
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }
    int count(const QString& path) const { return static_cast<int>(requested.count(path)); }
    QString url(const QString& path) const
    { return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(path); }
};

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

template <typename Pred>
static void pumpUntil(Pred done, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms && !done()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---------------------------------------------------------------- 1. merge by precedence (pure)
    // The manual role is not special to the merge: it rides in MediaArt::images and mergeLowerPriority keeps
    // the earliest (highest-priority) provider's candidate at the front, which image() returns.
    {
        MediaArt high; high.images.insert(QStringLiteral("manual"), { QStringLiteral("hi://manual.pdf") });
        MediaArt low;
        low.images.insert(QStringLiteral("manual"), { QStringLiteral("lo://manual.pdf") });
        low.images.insert(QStringLiteral("box"),    { QStringLiteral("lo://box.jpg") });

        MediaArt merged = high;             // finishJob seeds with the highest-priority result…
        merged.mergeLowerPriority(low);     // …then folds each lower provider in, in order.
        CHECK(merged.image(QStringLiteral("manual")) == QStringLiteral("hi://manual.pdf"),
              "present higher-priority provider's manual wins");
        CHECK(merged.image(QStringLiteral("box")) == QStringLiteral("lo://box.jpg"),
              "a role only the lower provider has still backfills");

        // Absent-provider-ignored: the higher provider offers NO manual, so the lower provider's manual is the
        // one that survives — the merge does not manufacture a gap where a provider simply didn't answer.
        MediaArt highNoManual; highNoManual.images.insert(QStringLiteral("box"), { QStringLiteral("hi://box.jpg") });
        MediaArt lowHasManual; lowHasManual.images.insert(QStringLiteral("manual"), { QStringLiteral("lo://manual.pdf") });
        MediaArt merged2 = highNoManual;
        merged2.mergeLowerPriority(lowHasManual);
        CHECK(merged2.image(QStringLiteral("manual")) == QStringLiteral("lo://manual.pdf"),
              "manual backfills from the lower provider when the higher has none");
    }

    // ---------------------------------------------------------------- classifier (pure)
    // The single fact the prefetch exclusion turns on: a manual is on-demand, every artwork role is not.
    CHECK(MetaCache::isOnDemandRole(QStringLiteral("manual")), "manual is an on-demand role");
    CHECK(!MetaCache::isOnDemandRole(QStringLiteral("box")),   "box is not on-demand");
    CHECK(!MetaCache::isOnDemandRole(QStringLiteral("poster")),"poster is not on-demand");
    CHECK(!MetaCache::isOnDemandRole(QStringLiteral("logo")),  "logo is not on-demand");
    CHECK(!MetaCache::isOnDemandRole(QStringLiteral("thumb")), "thumb is not on-demand");

    Loopback http;
    CHECK(http.start(), "loopback http server listens");
    if (!http.srv.isListening()) { std::fprintf(stderr, "MANUAL-FAIL no server\n"); return 1; }

    // ---------------------------------------------------------------- 2. prefetch exclusion (behavioural)
    // saveArt() is the terminus of the hover / console-entry prefetch path (finishJob calls it). Handed a
    // bundle with a box AND a manual role, it must eagerly fetch the box but leave the manual alone — the URL
    // recorded, the megabyte file untouched.
    {
        const QString key = QStringLiteral("manual-probe:exclusion");
        MediaArt art;
        art.images.insert(QStringLiteral("box"),    { http.url(QStringLiteral("/box.jpg")) });
        art.images.insert(QStringLiteral("manual"), { http.url(QStringLiteral("/manual.pdf")) });
        MetaCache::saveArt(key, art);

        // Box downloads asynchronously; wait for it, then drain a little so any (wrongly-issued) manual request
        // has certainly reached the server before we assert it never came.
        pumpUntil([&] { return !MetaCache::imagePath(key, QStringLiteral("box")).isEmpty(); }, 8000);
        pump(300);

        CHECK(!MetaCache::imagePath(key, QStringLiteral("box")).isEmpty(), "box role was eagerly prefetched");
        CHECK(http.count(QStringLiteral("/box.jpg")) >= 1, "the box was actually requested");
        // The exclusion itself: the manual was never fetched.
        CHECK(http.count(QStringLiteral("/manual.pdf")) == 0, "the manual was NOT requested on prefetch");
        CHECK(MetaCache::manualPath(key).isEmpty(), "no manual file was written on prefetch");
        CHECK(MetaCache::imagePath(key, QStringLiteral("manual")).isEmpty(), "manual not stored as an image");
        // …but the URL is recorded, so a later open can find it.
        CHECK(MetaCache::loadArt(key).image(QStringLiteral("manual")) == http.url(QStringLiteral("/manual.pdf")),
              "the manual URL is recorded in the bundle for on-demand open");
    }

    // ---------------------------------------------------------------- 3. on-demand fetch (behavioural)
    {
        const QString key = QStringLiteral("manual-probe:ondemand");
        CHECK(MetaCache::manualPath(key).isEmpty(), "no manual cached before the first open");

        bool done = false; QString gotPath; qint64 maxReceived = -1;
        MetaCache::fetchManual(key, http.url(QStringLiteral("/manual.pdf")),
            [&](qint64 received, qint64) { maxReceived = qMax(maxReceived, received); },
            [&](const QString& path) { gotPath = path; done = true; });
        pumpUntil([&] { return done; }, 8000);

        CHECK(done, "fetchManual completed");
        CHECK(!gotPath.isEmpty(), "fetchManual handed back a local path");
        CHECK(MetaCache::manualPath(key) == gotPath, "manualPath resolves the fetched file");
        CHECK(maxReceived > 0, "download progress fired with bytes received");
        CHECK(http.count(QStringLiteral("/manual.pdf")) == 1, "on-demand fetch made exactly one request");

        // The stored bytes are exactly the fixture payload — an independent oracle, not anything the code
        // derived — and the container is .pdf (the reader we open it with).
        QFile f(gotPath);
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == kPdfBytes, "the fetched file is the served PDF, byte-for-byte");
        CHECK(gotPath.endsWith(QStringLiteral(".pdf")), "the manual is stored as a .pdf");

        // Second open: served from the cache with NO extra network hit.
        bool done2 = false; QString gotPath2;
        MetaCache::fetchManual(key, http.url(QStringLiteral("/manual.pdf")),
            nullptr, [&](const QString& path) { gotPath2 = path; done2 = true; });
        pumpUntil([&] { return done2; }, 4000);
        CHECK(done2 && gotPath2 == gotPath, "second open returns the cached path");
        CHECK(http.count(QStringLiteral("/manual.pdf")) == 1, "second open hit the cache, not the network");
    }

    if (failures) { std::fprintf(stderr, "MANUAL-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("MANUAL-OK\n");
    return 0;
}
