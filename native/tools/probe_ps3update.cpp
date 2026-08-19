// Headless pure-logic probe for the PS3 auto-update units. Prints PS3UPDATE-OK on success.
// No display, no network, no process spawns — every external effect is an injected seam.
#include "core/ps3/Ps3Sfo.h"
#include "core/ps3/Ps3UpdateFeed.h"
#include "core/ps3/Ps3Version.h"
#include "core/ps3/Ps3UpdateState.h"
#include "core/ps3/Ps3UpdateInstaller.h"
#include "core/ps3/Ps3TitleId.h"
#include "core/ps3/Ps3UpdateCoordinator.h"
#include "core/ps3/Ps3InstalledVersion.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileDevice>
#include <QString>
#include <QVector>
#include <QPair>
#include <QtEndian>
#include <QTemporaryDir>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QStringList>
#include <cstdio>
#include <optional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// Build a minimal valid PARAM.SFO holding the given UTF-8 string keys, so the parser is exercised
// against real bytes rather than a magic blob. Layout: 20-byte header, N index entries (16 bytes each),
// key table (null-terminated names), data table (null-terminated values padded to max len).
static QByteArray makeSfo(const QVector<QPair<QString, QString>>& kv)
{
    auto u16 = [](quint16 v) { char b[2]; qToLittleEndian(v, b); return QByteArray(b, 2); };
    auto u32 = [](quint32 v) { char b[4]; qToLittleEndian(v, b); return QByteArray(b, 4); };

    QByteArray keyTable, dataTable, index;
    QVector<quint32> keyOffs, dataOffs, dataLens, dataMax;
    for (const auto& p : kv)
    {
        QByteArray k = p.first.toUtf8();  k.append('\0');
        QByteArray d = p.second.toUtf8(); d.append('\0');
        const quint32 maxLen = static_cast<quint32>((d.size() + 15) & ~15); // pad to 16
        keyOffs.append(static_cast<quint32>(keyTable.size()));
        dataOffs.append(static_cast<quint32>(dataTable.size()));
        dataLens.append(static_cast<quint32>(d.size()));
        dataMax.append(maxLen);
        keyTable.append(k);
        dataTable.append(d);
        dataTable.append(QByteArray(static_cast<int>(maxLen) - d.size(), '\0'));
    }
    const quint32 entries = static_cast<quint32>(kv.size());
    const quint32 keyStart = 20 + entries * 16;
    const quint32 dataStart = keyStart + static_cast<quint32>(keyTable.size());
    for (quint32 i = 0; i < entries; ++i)
    {
        index += u16(static_cast<quint16>(keyOffs[i]));
        index += u16(0x0204); // utf8 null-terminated
        index += u32(dataLens[i]);
        index += u32(dataMax[i]);
        index += u32(dataOffs[i]);
    }
    QByteArray out;
    out.append('\0'); out.append("PSF", 3);       // magic \0PSF
    out += u32(0x00000101);                        // version 1.1
    out += u32(keyStart);
    out += u32(dataStart);
    out += u32(entries);
    out += index; out += keyTable; out += dataTable;
    return out;
}

static void testSfo()
{
    const QByteArray sfo = makeSfo({ { "APP_VER", "01.00" }, { "TITLE_ID", "BLUS31156" }, { "TITLE", "GTA V" } });
    auto id = Ps3Sfo::titleIdFromSfo(sfo);
    CHECK(id.has_value());
    CHECK(id.value_or(QString()) == QStringLiteral("BLUS31156"));

    CHECK(!Ps3Sfo::titleIdFromSfo(makeSfo({ { "TITLE", "No id here" } })).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray("not an sfo")).has_value());
    CHECK(!Ps3Sfo::titleIdFromSfo(QByteArray()).has_value());

    // Any key, not just TITLE_ID: the installed-version check reads APP_VER out of the same blob.
    CHECK(Ps3Sfo::stringValue(sfo, "APP_VER").value_or(QString()) == QStringLiteral("01.00"));
    CHECK(Ps3Sfo::stringValue(sfo, "TITLE").value_or(QString()) == QStringLiteral("GTA V"));
    CHECK(!Ps3Sfo::stringValue(sfo, "VERSION").has_value()); // absent key
    CHECK(!Ps3Sfo::stringValue(QByteArray("not an sfo"), "APP_VER").has_value());
}

static void testFeed()
{
    // Verified real single-package feed (The Last of Us, BCUS98174).
    const QByteArray single =
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<titlepatch status=\"alive\" titleid=\"BCUS98174\">"
        "<tag name=\"BCUS98174_T11\" popup=\"true\" signoff=\"true\">"
        "<package version=\"01.11\" size=\"284414928\" "
        "sha1sum=\"5f978c88721962b54f5b12053ee06f896ef3b4a1\" "
        "url=\"http://b0.ww.np.dl.playstation.net/tppkg/np/BCUS98174/BCUS98174_T11/x/patch.pkg\" "
        "ps3_system_ver=\"04.4000\"><paramsfo><TITLE>The Last of Us 1.11</TITLE></paramsfo></package>"
        "</tag></titlepatch>";
    auto one = Ps3UpdateFeed::parseVerXml(single);
    CHECK(one.size() == 1);
    if (one.size() == 1)
    {
        CHECK(one[0].version == QStringLiteral("01.11"));
        CHECK(one[0].size == 284414928LL);
        CHECK(one[0].sha1 == QStringLiteral("5f978c88721962b54f5b12053ee06f896ef3b4a1"));
        CHECK(one[0].url.startsWith(QStringLiteral("http://")));
    }

    // A multi-package chain, listed OUT of version order — must come back sorted ascending.
    const QByteArray chain =
        "<titlepatch titleid=\"BLUS31156\">"
        "<package version=\"01.11\" size=\"20\" sha1sum=\"bb\" url=\"http://h/b.pkg\"></package>"
        "<package version=\"01.05\" size=\"10\" sha1sum=\"aa\" url=\"http://h/a.pkg\"></package>"
        "</titlepatch>";
    auto many = Ps3UpdateFeed::parseVerXml(chain);
    CHECK(many.size() == 2);
    if (many.size() == 2)
    {
        CHECK(many[0].version == QStringLiteral("01.05")); // sorted ascending
        CHECK(many[1].version == QStringLiteral("01.11"));
    }

    CHECK(Ps3UpdateFeed::parseVerXml(QByteArray()).isEmpty());          // no updates = empty body
    CHECK(Ps3UpdateFeed::parseVerXml(QByteArray("<broken")).isEmpty()); // malformed = empty, not fatal
}

static void testState()
{
    CHECK(Ps3Version::less(QStringLiteral("01.05"), QStringLiteral("01.11")));
    CHECK(!Ps3Version::less(QStringLiteral("01.11"), QStringLiteral("01.11")));
    CHECK(!Ps3Version::less(QStringLiteral("02.00"), QStringLiteral("01.99")));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/ps3-updates.json");
    {
        Ps3UpdateState s(path);
        CHECK(s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11"))); // unknown -> needs it
        s.markInstalled(QStringLiteral("BLUS31156"), QStringLiteral("01.11"));
        CHECK(!s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11"))); // equal -> no
        CHECK(s.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.12")));  // newer -> yes
    }
    {
        Ps3UpdateState reopened(path); // persisted across instances
        CHECK(!reopened.needsUpdate(QStringLiteral("BLUS31156"), QStringLiteral("01.11")));
    }
}

static QString sha1Hex(const QByteArray& b)
{ return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex()); }

static void testInstaller()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QByteArray bodyA("PKG-A-BYTES"), bodyB("PKG-B-BYTES");

    // A stub downloader that writes canned bytes keyed by URL, and records order.
    QStringList installed;
    auto downloader = [&](const QString& url, const QString& dest) -> bool {
        const QByteArray body = url.endsWith(QStringLiteral("a.pkg")) ? bodyA : bodyB;
        QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write(body); return true;
    };
    QVector<QPair<QString, QString>> ctx; // (titleId, version) threaded through to each install
    auto runner = [&](const QString&, const QString& pkg, const QString& titleId, const QString& version) -> int {
        installed << pkg; ctx.append({ titleId, version }); return 0; };

    QVector<Ps3UpdatePackage> pkgs = {
        { QStringLiteral("01.05"), 0, sha1Hex(bodyA), QStringLiteral("http://h/a.pkg"), {} },
        { QStringLiteral("01.11"), 0, sha1Hex(bodyB), QStringLiteral("http://h/b.pkg"), {} },
    };

    Ps3UpdateInstaller good(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    CHECK(good.installAll(QStringLiteral("BLUS31156"), pkgs));
    CHECK(installed.size() == 2);                       // both installed, in order
    if (installed.size() == 2) CHECK(installed[0].endsWith(QStringLiteral("a.pkg")) || installed[0].contains(QStringLiteral("01.05")));
    CHECK(ctx.size() == 2);                             // each install knows the title + version it must reach
    if (ctx.size() == 2)
    {
        CHECK(ctx[0] == qMakePair(QStringLiteral("BLUS31156"), QStringLiteral("01.05")));
        CHECK(ctx[1] == qMakePair(QStringLiteral("BLUS31156"), QStringLiteral("01.11")));
    }

    // Temp pkgs cleaned up afterwards.
    QDir d(dir.path());
    CHECK(d.entryList(QStringList() << QStringLiteral("*.pkg"), QDir::Files).isEmpty());

    // SHA mismatch on the second package aborts the whole update, nothing extra installed.
    installed.clear();
    ctx.clear();
    QVector<Ps3UpdatePackage> bad = pkgs;
    bad[1].sha1 = QStringLiteral("deadbeef");
    Ps3UpdateInstaller mm(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    CHECK(!mm.installAll(QStringLiteral("BLUS31156"), bad));
    CHECK(installed.size() == 1); // only the first (good) package's install ran before the abort
    CHECK(d.entryList(QStringList() << QStringLiteral("*.pkg"), QDir::Files).isEmpty()); // still cleaned up
}

static void testInstalledVersion()
{
    CHECK(Ps3InstalledVersion::gameDir(QStringLiteral("root"), QStringLiteral("BLUS31156"))
          == QStringLiteral("root/dev_hdd0/game/BLUS31156"));

    QTemporaryDir tmp; CHECK(tmp.isValid());
    auto writeSfo = [&](const QString& dir, const QByteArray& bytes) {
        QDir().mkpath(dir);
        QFile f(dir + QStringLiteral("/PARAM.SFO"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    };

    const QString g = tmp.path() + QStringLiteral("/game");
    writeSfo(g, makeSfo({ { "APP_VER", "01.05" }, { "TITLE_ID", "BLUS31156" } }));
    CHECK(Ps3InstalledVersion::installedVersion(g).value_or(QString()) == QStringLiteral("01.05"));
    CHECK(Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.05")));
    CHECK(!Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.11")));
    CHECK(Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.04"))); // past the target still counts

    const QString fallback = tmp.path() + QStringLiteral("/fallback");
    writeSfo(fallback, makeSfo({ { "VERSION", "01.02" } }));
    CHECK(Ps3InstalledVersion::installedVersion(fallback).value_or(QString()) == QStringLiteral("01.02"));

    const QString missing = tmp.path() + QStringLiteral("/nothing-here");
    CHECK(!Ps3InstalledVersion::installedVersion(missing).has_value());
    CHECK(!Ps3InstalledVersion::reachedTarget(missing, QStringLiteral("01.00")));

    const QString bare = tmp.path() + QStringLiteral("/bare"); // dir exists, no PARAM.SFO
    QDir().mkpath(bare);
    CHECK(!Ps3InstalledVersion::installedVersion(bare).has_value());
    CHECK(!Ps3InstalledVersion::reachedTarget(bare, QStringLiteral("01.00")));

    const QString junk = tmp.path() + QStringLiteral("/junk");
    writeSfo(junk, QByteArray("this is not an sfo"));
    CHECK(!Ps3InstalledVersion::installedVersion(junk).has_value());
    CHECK(!Ps3InstalledVersion::reachedTarget(junk, QStringLiteral("01.00")));

    // Quiescence: no files at all (or no dir) is not "quiet", it is "unknown" -> -1.
    CHECK(Ps3InstalledVersion::secsSinceNewestWrite(missing, QDateTime::currentDateTimeUtc()) == -1);
    CHECK(Ps3InstalledVersion::secsSinceNewestWrite(bare, QDateTime::currentDateTimeUtc()) == -1);
    const qint64 fresh = Ps3InstalledVersion::secsSinceNewestWrite(g, QDateTime::currentDateTimeUtc());
    CHECK(fresh >= 0 && fresh <= 5);

    // A file nested two levels down counts: a pkg's payload lands under USRDIR, not at the game root.
    const QString quiet = tmp.path() + QStringLiteral("/quiet");
    QDir().mkpath(quiet + QStringLiteral("/USRDIR/deep"));
    const QString deep = quiet + QStringLiteral("/USRDIR/deep/data.bin");
    { QFile f(deep); CHECK(f.open(QIODevice::WriteOnly)); f.write("payload"); }
    const qint64 nested = Ps3InstalledVersion::secsSinceNewestWrite(quiet, QDateTime::currentDateTimeUtc());
    CHECK(nested >= 0 && nested <= 5);

    bool aged = false;
    { QFile f(deep);
      if (f.open(QIODevice::ReadWrite))
          aged = f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-60), QFileDevice::FileModificationTime); }
    if (aged) // skipped where the filesystem refuses an mtime write
        CHECK(Ps3InstalledVersion::secsSinceNewestWrite(quiet, QDateTime::currentDateTimeUtc()) >= 55);
}

static void testTitleId()
{
    // --- PKG header: magic "\x7FPKG", 36-byte content_id at 0x30 = "UP0001-BLUS31156_00-GTAVGTAVGTAVGTA"
    QByteArray pkg(0x60, '\0');
    pkg[0] = 0x7F; pkg[1] = 'P'; pkg[2] = 'K'; pkg[3] = 'G';
    const QByteArray cid = "UP0001-BLUS31156_00-GTAVGTAVGTAVGTA"; // 35 chars + implicit slack
    for (int i = 0; i < cid.size(); ++i) pkg[0x30 + i] = cid[i];
    auto fromHdr = Ps3TitleId::titleIdFromPkgHeader(pkg);
    CHECK(fromHdr.value_or(QString()) == QStringLiteral("BLUS31156"));
    CHECK(!Ps3TitleId::titleIdFromPkgHeader(QByteArray("not a pkg")).has_value());

    // --- folder game: <root>/PS3_GAME/PARAM.SFO
    QTemporaryDir dir; CHECK(dir.isValid());
    const QString root = dir.path() + QStringLiteral("/game");
    QDir().mkpath(root + QStringLiteral("/PS3_GAME"));
    {
        QFile f(root + QStringLiteral("/PS3_GAME/PARAM.SFO"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(makeSfo({ { "TITLE_ID", "BLUS31156" } }));
    }
    CHECK(Ps3TitleId::read(root).value_or(QString()) == QStringLiteral("BLUS31156"));
    // reading from the EBOOT path walks up to the game root
    QDir().mkpath(root + QStringLiteral("/PS3_GAME/USRDIR"));
    { QFile e(root + QStringLiteral("/PS3_GAME/USRDIR/EBOOT.BIN")); CHECK(e.open(QIODevice::WriteOnly)); e.write("x"); }
    CHECK(Ps3TitleId::read(root + QStringLiteral("/PS3_GAME/USRDIR/EBOOT.BIN")).value_or(QString()) == QStringLiteral("BLUS31156"));

    // --- .pkg file on disk
    const QString pkgPath = dir.path() + QStringLiteral("/game.pkg");
    { QFile f(pkgPath); CHECK(f.open(QIODevice::WriteOnly)); f.write(pkg); }
    CHECK(Ps3TitleId::read(pkgPath).value_or(QString()) == QStringLiteral("BLUS31156"));

    // --- unknown format -> nullopt (safe fallthrough)
    const QString isoPath = dir.path() + QStringLiteral("/game.iso");
    { QFile f(isoPath); CHECK(f.open(QIODevice::WriteOnly)); f.write("random iso bytes"); }
    CHECK(!Ps3TitleId::read(isoPath).has_value());
}

static void testCoordinator()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QByteArray body("PKGDATA");
    const QByteArray feed =
        QByteArray("<titlepatch titleid=\"BLUS31156\"><package version=\"01.11\" size=\"7\" sha1sum=\"")
        + sha1Hex(body).toLatin1() + "\" url=\"http://h/a.pkg\"></package></titlepatch>";

    auto downloader = [&](const QString&, const QString& dest) {
        QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write(body); return true; };
    int installs = 0;
    auto runner = [&](const QString&, const QString&, const QString&, const QString&) { ++installs; return 0; };
    Ps3UpdateInstaller installer(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner);
    Ps3UpdateState state(dir.path() + QStringLiteral("/state.json"));

    QStringList notes;
    auto progress = [&](const QString& m) { notes << m; };

    // Happy path: reads id, fetches feed, installs, marks state.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(c.maybeUpdate(QStringLiteral("/any/rom")));
    }
    CHECK(installs == 1);
    CHECK(!notes.isEmpty()); // showed an "Updating…" note

    // Second run: state says current -> no work, no extra install.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS31156")); },
            [&](const QString&) { return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
    }
    CHECK(installs == 1); // unchanged

    // No Title ID -> falls through, no fetch/install.
    {
        int fetches = 0;
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(); },
            [&](const QString&) { ++fetches; return std::optional<QByteArray>(feed); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
        CHECK(fetches == 0);
    }

    // Empty feed (Sony "no updates") -> falls through.
    {
        Ps3UpdateCoordinator c(
            [](const QString&) { return std::optional<QString>(QStringLiteral("BLUS40000")); },
            [&](const QString&) { return std::optional<QByteArray>(QByteArray()); },
            &state, &installer, progress);
        CHECK(!c.maybeUpdate(QStringLiteral("/any/rom")));
    }
}

int main()
{
    testSfo();
    testFeed();
    testState();
    testInstaller();
    testInstalledVersion();
    testTitleId();
    testCoordinator();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3UPDATE-OK\n");
    return 0;
}
