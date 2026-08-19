// Headless pure-logic probe for the PS3 firmware auto-install unit. Prints PS3FIRMWARE-OK on success.
// No display, no network, no process spawns — every external effect is an injected seam.
#include "core/ps3/Ps3Firmware.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdio>
#include <optional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// Realistic two-record feed: a compatibility line (no CDN) followed by the image line. The parser must
// skip the first and return the CDN url + SystemSoftwareVersion of the second.
static const char* kRealFeed =
    "Dest=84;CompatibleSystemSoftwareVersion=4.9200-;\n"
    "Dest=84;ImageVersion=04.9200;SystemSoftwareVersion=4.9200;"
    "CDN=http://dus01.ps3.update.playstation.net/update/ps3/image/us/"
    "2024_0227_09799a2ba7bdcb84302b3ba09b5be4f8/PS3UPDAT.PUP;CDN_Timeout=30;\n";

static void testParse()
{
    auto info = Ps3Firmware::parseUpdateList(QByteArray(kRealFeed));
    CHECK(info.has_value());
    if (info)
    {
        CHECK(info->version == QStringLiteral("4.9200"));
        CHECK(info->url.startsWith(QStringLiteral("http://dus01.ps3.update.playstation.net/")));
        CHECK(info->url.endsWith(QStringLiteral("/PS3UPDAT.PUP")));
    }

    CHECK(!Ps3Firmware::parseUpdateList(QByteArray()).has_value());                     // empty body
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("Dest=84;ImageVersion=1;\n")).has_value()); // no CDN field
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("random junk not a feed")).has_value());
    CHECK(!Ps3Firmware::parseUpdateList(QByteArray("CDN=ftp://evil/PS3UPDAT.PUP;\n")).has_value()); // non-http url
}

// Create <root>/dev_flash/vsh/etc/version.txt with the given bytes.
static void seedVersionTxt(const QString& root, const QByteArray& bytes)
{
    QDir().mkpath(root + QStringLiteral("/dev_flash/vsh/etc"));
    QFile f(root + QStringLiteral("/dev_flash/vsh/etc/version.txt"));
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

// The failure marker maybeInstall drops in tmpDir to back off the next attempt.
static QString markerPath(const QString& tmpDir)
{
    return QDir(tmpDir).filePath(QStringLiteral("fw-install-failed"));
}

// Write the marker, dated `agoSecs` seconds in the past (0 = now).
static void seedMarker(const QString& tmpDir, int agoSecs)
{
    QDir().mkpath(tmpDir);
    QFile f(markerPath(tmpDir));
    CHECK(f.open(QIODevice::ReadWrite | QIODevice::Truncate));
    f.write("earlier failure\n");
    if (agoSecs > 0)
        CHECK(f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-agoSecs),
                            QFileDevice::FileModificationTime));
}

static void testInstalled()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    CHECK(!Ps3Firmware::installed(dir.path()));       // no dev_flash at all
    seedVersionTxt(dir.path(), QByteArray());
    CHECK(!Ps3Firmware::installed(dir.path()));       // version.txt present but EMPTY = incomplete
    seedVersionTxt(dir.path(), QByteArray("04.9200"));
    CHECK(Ps3Firmware::installed(dir.path()));        // real content = installed
}

static void testMaybeInstall()
{
    const QString feed = QString::fromLatin1(kRealFeed);

    // Already installed -> no work at all: the feed is never even fetched.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        seedVersionTxt(dir.path(), QByteArray("04.9200"));
        int fetches = 0;
        auto fetch = [&]() -> std::optional<QByteArray> { ++fetches; return feed.toUtf8(); };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), dir.path(),
                                         fetch, nullptr, nullptr, nullptr));
        CHECK(fetches == 0);
    }

    // A FRESH failure marker suppresses the whole pipeline: no feed fetch, no ~230MB re-download.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        seedMarker(tmp, 0);
        int fetches = 0;
        auto fetch = [&]() -> std::optional<QByteArray> { ++fetches; return feed.toUtf8(); };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, nullptr, nullptr, nullptr));
        CHECK(fetches == 0);
    }

    // A STALE marker (older than the backoff) must not suppress anything: the pipeline runs again.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        seedMarker(tmp, 7200); // ~2h ago
        int fetches = 0;
        auto fetch = [&]() -> std::optional<QByteArray> { ++fetches; return feed.toUtf8(); };
        auto download = [&](const QString&, const QString&) { return false; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, nullptr, nullptr));
        CHECK(fetches == 1);
    }

    // Happy path: missing firmware -> fetch -> download -> --installfw (which produces dev_flash) -> true.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        seedMarker(tmp, 7200); // a stale marker from an old failure, so its REMOVAL is observable
        QString downloadedUrl, installedPup;
        QStringList notes;
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString& url, const QString& dest) {
            downloadedUrl = url;
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString& pup) {
            installedPup = pup;
            CHECK(QFile::exists(pup)); // the PUP must still be on disk when the installer runs
            seedVersionTxt(dir.path(), QByteArray("04.9200")); // simulate --installfw writing dev_flash
            return 0;
        };
        auto progress = [&](const QString& m) { notes << m; };
        CHECK(Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                        fetch, download, install, progress));
        CHECK(downloadedUrl.endsWith(QStringLiteral("/PS3UPDAT.PUP")));
        CHECK(installedPup.endsWith(QStringLiteral("PS3UPDAT.PUP")));
        CHECK(installedPup.startsWith(tmp));     // downloaded into the temp dir we were handed
        CHECK(!notes.isEmpty());                 // told the user what's happening
        CHECK(notes.first().contains(QStringLiteral("4.9200"))); // ...and which version it's installing
        CHECK(!QFile::exists(installedPup));     // temp PUP cleaned up afterwards
        CHECK(!QFile::exists(markerPath(tmp)));  // success clears the old failure marker
    }

    // Fetch fails -> false, nothing downloaded or installed.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        int downloads = 0;
        auto fetch    = [&]() -> std::optional<QByteArray> { return std::nullopt; };
        auto download = [&](const QString&, const QString&) { ++downloads; return true; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), dir.path(),
                                         fetch, download, nullptr, nullptr));
        CHECK(downloads == 0);
    }

    // Download fails -> false, installer never runs, no stray PUP left behind, and the failure is
    // recorded so the next launch backs off instead of re-downloading.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        int installs = 0;
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString&) { return false; };
        auto install  = [&](const QString&, const QString&) { ++installs; return 0; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
        CHECK(installs == 0);
        CHECK(!QFile::exists(QDir(tmp).filePath(QStringLiteral("PS3UPDAT.PUP"))));
        CHECK(QFile::exists(markerPath(tmp))); // failure marker written -> next launch backs off
    }

    // Installer exits non-zero -> false, temp PUP still cleaned up. The installer here DOES leave a
    // dev_flash behind (a half-written one, or one left over from an earlier attempt), so the exit code
    // is the only thing that can fail this case — that is what makes it a test of the exit-code check
    // rather than a second test of the dev_flash check below.
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString& dest) {
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString&) {
            seedVersionTxt(dir.path(), QByteArray("04.9200"));
            return 1;
        };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
        CHECK(!QFile::exists(QDir(tmp).filePath(QStringLiteral("PS3UPDAT.PUP"))));
    }

    // Installer exits 0 but produced NO dev_flash -> false (an exit code alone is not proof).
    {
        QTemporaryDir dir; CHECK(dir.isValid());
        const QString tmp = dir.path() + QStringLiteral("/tmp");
        auto fetch    = [&]() -> std::optional<QByteArray> { return feed.toUtf8(); };
        auto download = [&](const QString&, const QString& dest) {
            QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write("PUPBYTES"); return true;
        };
        auto install  = [&](const QString&, const QString&) { return 0; };
        CHECK(!Ps3Firmware::maybeInstall(dir.path(), QStringLiteral("rpcs3.exe"), tmp,
                                         fetch, download, install, nullptr));
    }
}

int main()
{
    testParse();
    testInstalled();
    testMaybeInstall();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3FIRMWARE-OK\n");
    return 0;
}
