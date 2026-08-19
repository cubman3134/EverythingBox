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
#include "core/ps3/Ps3Pkg.h"

#include <QByteArray>
#include <QDateTime>
#include <QFileDevice>
#include <QFileInfo>
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

// Sony-shaped package bytes: payload + a 0x20-byte footer whose first 20 bytes are the SHA-1 of the
// payload — the real retail pkg layout (verified against a live BCUS98148 01.02 download, 2026-08-19).
// The ver.xml `sha1sum` covers ONLY the payload, so the digest the installer must compute is
// SHA1(file minus 0x20) — and a whole-file hash of a genuine pkg can NEVER equal its ver.xml digest
// (the digest is part of what a whole-file hash consumes). The hardware failure this pins: hashing the
// whole file failed every genuine Sony download and aborted the chain before rpcs3 ever ran.
static QByteArray sonyPkg(const QByteArray& payload)
{
    QByteArray b = payload;
    b += QCryptographicHash::hash(payload, QCryptographicHash::Sha1); // 20 bytes: the digest itself
    b += QByteArray(12, '\0');                                        // pad the footer to 0x20
    return b;
}

static void testInstaller()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QByteArray bodyA = sonyPkg("PKG-A-PAYLOAD-BYTES"), bodyB = sonyPkg("PKG-B-PAYLOAD-BYTES");
    // The digest the feed advertises is the payload's, never the whole file's.
    const QString shaA = sha1Hex(bodyA.left(bodyA.size() - 0x20));
    const QString shaB = sha1Hex(bodyB.left(bodyB.size() - 0x20));
    CHECK(shaA != sha1Hex(bodyA)); // the never-equal property the footer guarantees
    CHECK(shaB != sha1Hex(bodyB));

    // A stub downloader that writes canned bytes keyed by URL, and records order.
    QStringList installed, fetched;
    auto downloader = [&](const QString& url, const QString& dest) -> bool {
        fetched << url;
        const QByteArray body = url.endsWith(QStringLiteral("a.pkg")) ? bodyA : bodyB;
        QFile f(dest); if (!f.open(QIODevice::WriteOnly)) return false; f.write(body); return true;
    };
    QVector<QPair<QString, QString>> ctx; // (titleId, version) threaded through to each install
    auto runner = [&](const QString&, const QString& pkg, const QString& titleId, const QString& version) -> int {
        installed << pkg; ctx.append({ titleId, version }); return 0; };

    QVector<Ps3UpdatePackage> pkgs = {
        { QStringLiteral("01.05"), 0, shaA, QStringLiteral("http://h/a.pkg"), {} },
        { QStringLiteral("01.11"), 0, shaB, QStringLiteral("http://h/b.pkg"), {} },
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

    // Already-applied packages are skipped BEFORE their download — the whole point is not paying
    // hundreds of megabytes to rediscover a version that is already on disk.
    installed.clear(); ctx.clear(); fetched.clear();
    auto applied = [](const QString& titleId, const QString& version) {
        return titleId == QStringLiteral("BLUS31156") && version == QStringLiteral("01.05"); };
    Ps3UpdateInstaller skipper(QStringLiteral("rpcs3.exe"), dir.path(), downloader, runner, applied);
    CHECK(skipper.installAll(QStringLiteral("BLUS31156"), pkgs));
    CHECK(fetched.size() == 1);                                                   // a.pkg never fetched
    if (fetched.size() == 1) CHECK(fetched[0].endsWith(QStringLiteral("b.pkg")));
    CHECK(ctx.size() == 1);                                                       // nor installed
    if (ctx.size() == 1) CHECK(ctx[0] == qMakePair(QStringLiteral("BLUS31156"), QStringLiteral("01.11")));
    CHECK(d.entryList(QStringList() << QStringLiteral("*.pkg"), QDir::Files).isEmpty());
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

    // Numeric, not lexical: installed 01.10 IS past target 01.9 (minor 10 > 9), while a string compare
    // would rank "01.10" < "01.9" and re-run an update that is already on disk.
    const QString numeric = tmp.path() + QStringLiteral("/numeric");
    writeSfo(numeric, makeSfo({ { "APP_VER", "01.10" }, { "TITLE_ID", "BLUS31156" } }));
    CHECK(Ps3InstalledVersion::reachedTarget(numeric, QStringLiteral("01.9")));

    // An update SFO that carries ONLY VERSION (no APP_VER at all) still reports its version, and
    // reachedTarget accepts it at >= target — success must be recognized wherever the SFO reports it,
    // or the bounded wait expires on a perfectly good install and the rollback destroys it.
    const QString fallback = tmp.path() + QStringLiteral("/fallback");
    writeSfo(fallback, makeSfo({ { "CATEGORY", "GD" }, { "TITLE", "Game" },
                                 { "TITLE_ID", "BCUS98148" }, { "VERSION", "01.02" } }));
    CHECK(Ps3InstalledVersion::installedVersion(fallback).value_or(QString()) == QStringLiteral("01.02"));
    CHECK(Ps3InstalledVersion::reachedTarget(fallback, QStringLiteral("01.02")));
    CHECK(Ps3InstalledVersion::reachedTarget(fallback, QStringLiteral("01.01"))); // past target counts
    CHECK(!Ps3InstalledVersion::reachedTarget(fallback, QStringLiteral("01.03")));

    // The real Sony patch shape (hardware ground truth, LBP BCUS98148 01.02): APP_VER carries the patch
    // level while VERSION stays at the base 01.00 forever. APP_VER must win, or every patch reads as 01.00.
    const QString sony = tmp.path() + QStringLiteral("/sony");
    writeSfo(sony, makeSfo({ { "APP_VER", "01.02" }, { "CATEGORY", "GD" },
                             { "TITLE_ID", "BCUS98148" }, { "VERSION", "01.00" } }));
    CHECK(Ps3InstalledVersion::installedVersion(sony).value_or(QString()) == QStringLiteral("01.02"));
    CHECK(Ps3InstalledVersion::reachedTarget(sony, QStringLiteral("01.02")));

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

    // Quiescence fingerprint. An absent dir is not "busy", it is "nothing there yet": a valid,
    // stable value (nullopt is reserved for a file we could not open, i.e. the writer holds it).
    const auto emptyPrint = Ps3InstalledVersion::dirFingerprint(missing);
    CHECK(emptyPrint.has_value());
    CHECK(Ps3InstalledVersion::dirFingerprint(bare) == emptyPrint);

    const QString quiet = tmp.path() + QStringLiteral("/quiet");
    QDir().mkpath(quiet + QStringLiteral("/USRDIR/deep"));
    const QString deep = quiet + QStringLiteral("/USRDIR/deep/data.bin");
    { QFile f(deep); CHECK(f.open(QIODevice::WriteOnly)); f.write("payload"); }

    const auto p1 = Ps3InstalledVersion::dirFingerprint(quiet);
    CHECK(p1.has_value());
    CHECK(p1 != emptyPrint); // a tree with files is distinguishable from an empty one
    // Two scans of an unchanged tree must be byte-identical, or the quiet window never closes.
    CHECK(Ps3InstalledVersion::dirFingerprint(quiet) == p1);

    // A file GROWING moves the fingerprint even though its directory-entry mtime may not have been
    // flushed yet — the size comes from the open handle, which is what makes this NTFS-safe.
    { QFile f(deep); CHECK(f.open(QIODevice::Append)); f.write("more"); }
    const auto p2 = Ps3InstalledVersion::dirFingerprint(quiet);
    CHECK(p2.has_value());
    CHECK(p2 != p1);

    // A brand-new file nested two levels down counts: a pkg's payload lands under USRDIR, not at
    // the game root, so a root-only scan would call a mid-flight extraction quiet.
    QDir().mkpath(quiet + QStringLiteral("/USRDIR/deep/deeper"));
    { QFile f(quiet + QStringLiteral("/USRDIR/deep/deeper/extra.bin"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
    const auto p3 = Ps3InstalledVersion::dirFingerprint(quiet);
    CHECK(p3.has_value());
    CHECK(p3 != p2);

    // The cases above all CLOSED the file before rescanning. This one keeps the writer's handle OPEN
    // across the scan — the real shape of a pkg extraction — and pins two things the others cannot:
    // our own ReadOnly open does not lose to the in-flight writer (so a live install is not
    // permanently misread as "busy"), and the growth is visible mid-write rather than at close.
    // has_value() is asserted first: a nullopt would compare unequal too and pass vacuously.
    {
        QFile writer(deep);
        CHECK(writer.open(QIODevice::Append));
        writer.write("still-writing");
        writer.flush();
        const auto midWrite = Ps3InstalledVersion::dirFingerprint(quiet);
        CHECK(midWrite.has_value()); // not vacuous: a busy nullopt would also compare unequal
        CHECK(midWrite != p3);
        writer.close(); // only now
    }

    // The whole point of the fix, pinned: a file that GREW but whose mtime did not move must still
    // change the fingerprint. This is the mutation that a "newest mtime" quiescence check fails and
    // the one that mattered on NTFS, where the directory entry's timestamp lags a long write — restore
    // the old mtime by hand to reproduce that lag deterministically instead of racing the filesystem.
    const auto beforeGrow = Ps3InstalledVersion::dirFingerprint(quiet);
    CHECK(beforeGrow.has_value());
    const QDateTime was = QFileInfo(deep).lastModified();
    {
        QFile f(deep);
        CHECK(f.open(QIODevice::Append));
        f.write("grew-without-touching-mtime");
        f.close();
        QFile t(deep);
        if (t.open(QIODevice::ReadWrite)) t.setFileTime(was, QFileDevice::FileModificationTime);
    }
    // Verified AFTER the handle closed — a close that re-stamped the mtime would make the check below
    // pass for the wrong reason, so the skip is decided on what is actually on disk now.
    if (QFileInfo(deep).lastModified() == was) // else: filesystem refused the mtime write, skip
        CHECK(Ps3InstalledVersion::dirFingerprint(quiet) != beforeGrow);

    // An abort callback ends the walk as "busy" — the safe direction, and what keeps the app-quit
    // join bounded when the tree is huge.
    CHECK(!Ps3InstalledVersion::dirFingerprint(quiet, [] { return true; }).has_value());
    CHECK(Ps3InstalledVersion::dirFingerprint(quiet, [] { return false; }).has_value());
}

// Rollback of the entry-state PARAM.SFO around a KILLED --installpkg run. Without it, the early-extracted
// PARAM.SFO claiming the target version outlives the killed process, and the next launch skips the chain
// before downloading it and records a truncated update as applied.
static void testSfoRollback()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QByteArray oldSfo = makeSfo({ { "APP_VER", "01.05" }, { "TITLE_ID", "BLUS31156" } });
    const QByteArray newSfo = makeSfo({ { "APP_VER", "01.11" }, { "TITLE_ID", "BLUS31156" } });

    // Nothing there: null, NOT empty — the two mean different things to restoreSfo.
    const QString absent = tmp.path() + QStringLiteral("/absent");
    CHECK(Ps3InstalledVersion::snapshotSfo(absent).isNull());
    const QString bare = tmp.path() + QStringLiteral("/bare"); // dir exists, no PARAM.SFO
    QDir().mkpath(bare);
    CHECK(Ps3InstalledVersion::snapshotSfo(bare).isNull());

    const QString g = tmp.path() + QStringLiteral("/game");
    QDir().mkpath(g);
    const QString sfoPath = g + QStringLiteral("/PARAM.SFO");
    { QFile f(sfoPath); CHECK(f.open(QIODevice::WriteOnly)); f.write(oldSfo); }

    const QByteArray snap = Ps3InstalledVersion::snapshotSfo(g);
    CHECK(!snap.isNull());
    CHECK(snap == oldSfo);

    // The scenario: a killed run left the TARGET version on disk over a truncated tree. Restoring the
    // entry bytes must put the older, true version back, so the next launch re-runs the chain.
    { QFile f(sfoPath); CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(newSfo); }
    CHECK(Ps3InstalledVersion::installedVersion(g).value_or(QString()) == QStringLiteral("01.11"));
    CHECK(Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.11"))); // the lie
    Ps3InstalledVersion::restoreSfo(g, snap);
    CHECK(Ps3InstalledVersion::snapshotSfo(g) == oldSfo); // byte-identical
    CHECK(Ps3InstalledVersion::installedVersion(g).value_or(QString()) == QStringLiteral("01.05"));
    CHECK(!Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.11"))); // lie undone -> chain re-runs

    // A truncated/torn write from the killed run is replaced too, not appended to.
    { QFile f(sfoPath); CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(newSfo.left(newSfo.size() / 2)); }
    Ps3InstalledVersion::restoreSfo(g, snap);
    CHECK(Ps3InstalledVersion::snapshotSfo(g) == oldSfo);

    // Entry state was "no file at all" -> restore removes whatever the killed run left.
    const QString fresh = tmp.path() + QStringLiteral("/fresh");
    QDir().mkpath(fresh);
    const QByteArray none = Ps3InstalledVersion::snapshotSfo(fresh);
    CHECK(none.isNull());
    { QFile f(fresh + QStringLiteral("/PARAM.SFO")); CHECK(f.open(QIODevice::WriteOnly)); f.write(newSfo); }
    Ps3InstalledVersion::restoreSfo(fresh, none);
    CHECK(!QFile::exists(fresh + QStringLiteral("/PARAM.SFO")));
    CHECK(Ps3InstalledVersion::snapshotSfo(fresh).isNull());

    // The game dir may not exist yet when the kill lands (RPCS3 creates it): restoring bytes into a
    // missing dir creates it rather than silently dropping the rollback.
    const QString gone = tmp.path() + QStringLiteral("/gone/deeper");
    Ps3InstalledVersion::restoreSfo(gone, oldSfo);
    CHECK(Ps3InstalledVersion::snapshotSfo(gone) == oldSfo);
    // ...and removing from a missing dir is a no-op, not a crash.
    Ps3InstalledVersion::restoreSfo(tmp.path() + QStringLiteral("/never/existed"), QByteArray());
}

// The final verification a kill branch owes the tree before it restores the entry-state PARAM.SFO.
// The hardware failure class this exists for: a false-negative in the bounded success wait must never
// let the rollback clobber an install that actually completed — success is (version at target-or-newer,
// wherever the SFO reports it) AND (tree byte-identical to the last mid-run fingerprint). Anything less
// keeps the restore, which is the safe, self-healing direction for a genuinely truncated tree.
static void testCompletedDespiteKill()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    auto writeSfo = [&](const QString& dir, const QByteArray& bytes) {
        QDir().mkpath(dir);
        QFile f(dir + QStringLiteral("/PARAM.SFO"));
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
    };

    const QString g = tmp.path() + QStringLiteral("/game");
    QDir().mkpath(g + QStringLiteral("/USRDIR"));
    const QString payload = g + QStringLiteral("/USRDIR/data.bin");
    { QFile f(payload); CHECK(f.open(QIODevice::WriteOnly)); f.write("payload"); }
    // A VERSION-only update SFO (no APP_VER key at all) — the success must be recognized through the
    // fallback here too, with NO restore.
    writeSfo(g, makeSfo({ { "CATEGORY", "GD" }, { "TITLE", "LBP" },
                          { "TITLE_ID", "BCUS98148" }, { "VERSION", "01.02" } }));
    const auto printDone = Ps3InstalledVersion::dirFingerprint(g);
    CHECK(printDone.has_value());

    // Complete: version at target + tree unchanged since the last mid-run scan → SUCCESS, no restore.
    CHECK(Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printDone));
    CHECK(Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.01"), printDone)); // newer counts

    // Version short of target: the kill caught a real failure → restore path.
    CHECK(!Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.03"), printDone));
    // No mid-run fingerprint was ever taken (killed before the version first read as reached): nothing
    // to verify against → restore path.
    CHECK(!Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), std::nullopt));
    // An aborted verification scan verifies nothing → restore path (the safe direction).
    CHECK(!Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printDone,
                                                     [] { return true; }));

    // The tree moved since the last scan — a writer was still going when the kill landed, so the
    // PARAM.SFO claiming the target sits over an untrustworthy tree → restore path.
    { QFile f(payload); CHECK(f.open(QIODevice::Append)); f.write("more"); }
    CHECK(!Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printDone));
    const auto printNow = Ps3InstalledVersion::dirFingerprint(g);
    CHECK(Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printNow)); // agreement restored

    // The real Sony patch shape: APP_VER at target beside a base VERSION=01.00 — recognized too.
    writeSfo(g, makeSfo({ { "APP_VER", "01.02" }, { "CATEGORY", "GD" },
                          { "TITLE_ID", "BCUS98148" }, { "VERSION", "01.00" } }));
    const auto printSony = Ps3InstalledVersion::dirFingerprint(g);
    CHECK(Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printSony));
    CHECK(!Ps3InstalledVersion::completedDespiteKill(g, QStringLiteral("01.02"), printNow)); // stale print
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

// AES-128-CTR with the retail GPKG key, pinned against an INDEPENDENT implementation (.NET
// AES-128-ECB keystream, generated 2026-08-19) — the fixture tests below encrypt with the same
// function the parser decrypts with, so only known-answer vectors can catch a broken AES/key/counter.
static void testPkgCrypt()
{
    const QByteArray z16(16, '\0'), z32(32, '\0');
    CHECK(Ps3Pkg::gpkgCrypt(z32, z16).toHex()
          == "06b0681ba85d3e959861d07991838548c54505a018180ec6c1bc89151d6392d1");
    // riv=ff*16 + blockOffset 1 wraps the 128-bit counter to zero — the carry math, pinned: the
    // wrapped keystream must equal the riv=0 vector's first block.
    CHECK(Ps3Pkg::gpkgCrypt(z16, QByteArray(16, char(0xFF)), 1).toHex()
          == "06b0681ba85d3e959861d07991838548");
    // The real A0130.pkg riv at block 3 (the live parse that validated key+format, 2026-08-19).
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    CHECK(Ps3Pkg::gpkgCrypt(z16, riv, 3).toHex() == "a4a93c5f9890b4c6d0663ebc25cf1efc");
    // CTR is its own inverse at any block offset, and actually transforms.
    const QByteArray msg("USRDIR/patch.sdat sized 910064");
    CHECK(Ps3Pkg::gpkgCrypt(Ps3Pkg::gpkgCrypt(msg, riv, 7), riv, 7) == msg);
    CHECK(Ps3Pkg::gpkgCrypt(msg, riv, 7) != msg);
    // Non-block-multiple lengths keep the tail (partial last block).
    CHECK(Ps3Pkg::gpkgCrypt(QByteArray(17, '\0'), z16).toHex()
          == "06b0681ba85d3e959861d07991838548c5");
    CHECK(Ps3Pkg::gpkgCrypt(msg, QByteArray(15, '\0')).isEmpty()); // riv must be 16 bytes
    // A negative block offset is a corrupt header, not a counter position: reject it like a bad riv
    // rather than wrapping the 128-bit counter around and emitting plausible garbage.
    CHECK(Ps3Pkg::gpkgCrypt(QByteArray(16, '\0'), QByteArray(16, '\0'), -1).isEmpty());
}

// Assemble a retail-shaped pkg: header + AES-CTR-encrypted data area (entry table, then the name
// blob, then payload bytes). Field layout is the one the live A0130.pkg parse validated 2026-08-19.
// dataOffset is deliberately NOT 0x80 (real pkgs use 0x190) so a parser that assumes the data area
// abuts the header goes red.
struct PkgFixtureEntry { QByteArray name; QByteArray data; quint32 type; };
static QByteArray makePkg(const QVector<PkgFixtureEntry>& items, const QByteArray& riv,
                          quint32 itemCountOverride = 0xFFFFFFFF)
{
    auto be32 = [](quint32 v) { char b[4]; qToBigEndian(v, b); return QByteArray(b, 4); };
    auto be64 = [](quint64 v) { char b[8]; qToBigEndian(v, b); return QByteArray(b, 8); };

    const qint64 tableSize = qint64(items.size()) * 32;
    QByteArray names, blobs;
    QVector<quint32> nameOffs;
    QVector<quint64> dataOffs;
    for (const auto& it : items) { nameOffs << quint32(tableSize + names.size()); names += it.name; }
    const qint64 blobBase = tableSize + names.size();
    for (const auto& it : items) { dataOffs << quint64(blobBase + blobs.size()); blobs += it.data; }

    QByteArray table;
    for (int i = 0; i < items.size(); ++i)
    {
        table += be32(nameOffs[i]);
        table += be32(quint32(items[i].name.size()));
        table += be64(dataOffs[i]);
        table += be64(quint64(items[i].data.size()));
        table += be32(items[i].type);
        table += be32(0);
    }
    const QByteArray data = table + names + blobs;

    const quint64 dataOffset = 0x90; // header 0x80 + 0x10 slack: the parser must READ the field
    QByteArray hdr(int(dataOffset), '\xAA');
    hdr[0] = 0x7F; hdr[1] = 'P'; hdr[2] = 'K'; hdr[3] = 'G';
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 0; hdr[7] = 1; // pkg type 0x0001 = PS3
    const quint32 itemCount =
        itemCountOverride != 0xFFFFFFFF ? itemCountOverride : quint32(items.size());
    hdr.replace(0x14, 4, be32(itemCount));
    hdr.replace(0x20, 8, be64(dataOffset));
    hdr.replace(0x28, 8, be64(quint64(data.size())));
    const QByteArray cid = QByteArray("UP9000-BCUS98148_00-GLBPPATCH0000001").leftJustified(0x30, '\0');
    hdr.replace(0x30, 0x30, cid);
    hdr.replace(0x70, 16, riv);
    return hdr + Ps3Pkg::gpkgCrypt(data, riv, 0);
}

static QByteArray be32Bytes(quint32 v) { char b[4]; qToBigEndian(v, b); return QByteArray(b, 4); }
static QByteArray be64Bytes(quint64 v) { char b[8]; qToBigEndian(v, b); return QByteArray(b, 8); }

// Overwrite a big-endian field at `off` inside the ENCRYPTED entry table, re-encrypting in place.
// Lets a fixture lie about ONE field while every other field stays perfectly well-formed — the only
// shape that reaches a per-entry guard, since a wholesale-garbage table trips the name checks first.
static void patchTableField(QByteArray& pkg, quint64 dataOffset, const QByteArray& riv,
                            int off, const QByteArray& beBytes)
{
    const int blockFirst = off / 16;
    const int base = int(dataOffset) + blockFirst * 16;
    const int span = (off + beBytes.size() + 15) / 16 * 16 - blockFirst * 16;
    QByteArray pt = Ps3Pkg::gpkgCrypt(pkg.mid(base, span), riv, blockFirst);
    pt.replace(off - blockFirst * 16, beBytes.size(), beBytes);
    pkg.replace(base, span, Ps3Pkg::gpkgCrypt(pt, riv, blockFirst));
}

static QVector<PkgFixtureEntry> lbpShapedItems()
{
    // The A0130.pkg shape in miniature: overwrite files, a non-overwrite icon, dirs, NPDRM EBOOT,
    // SDAT — including the very entry that was 0 bytes on hardware.
    return {
        { "PARAM.SFO",         QByteArray(12, 'S'),  0x80000003u },
        { "ICON0.PNG",         QByteArray(5, 'I'),   0x00000003u }, // no overwrite bit
        { "USRDIR",            {},                    0x80000004u }, // dir
        { "USRDIR/output",     {},                    0x80000004u }, // dir, no file inside
        { "USRDIR/EBOOT.BIN",  QByteArray(100, 'E'), 0x80000101u }, // NPDRM (type & 0xFF == 1)
        { "USRDIR/patch.sdat", QByteArray(33, 'D'),  0x80000009u }, // SDAT
        { "USRDIR/empty.bin",  {},                    0x80000003u }, // legit 0-byte payload
    };
}

static void testPkgEntries()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    auto writePkg = [&](const QString& name, const QByteArray& bytes) {
        const QString p = tmp.path() + '/' + name;
        QFile f(p); CHECK(f.open(QIODevice::WriteOnly)); f.write(bytes); return p;
    };

    const QString good = writePkg("good.pkg", makePkg(lbpShapedItems(), riv));
    const auto got = Ps3Pkg::entries(good);
    CHECK(got.has_value());
    if (got)
    {
        CHECK(got->size() == 7);
        CHECK((*got)[0].path == QStringLiteral("PARAM.SFO"));
        CHECK((*got)[0].size == 12);
        CHECK((*got)[0].overwrite); CHECK(!(*got)[0].isDir);
        CHECK(!(*got)[1].overwrite);                       // ICON0.PNG carries no overwrite bit
        CHECK((*got)[2].isDir); CHECK((*got)[3].isDir);
        CHECK((*got)[4].path == QStringLiteral("USRDIR/EBOOT.BIN"));
        CHECK((*got)[4].size == 100); CHECK(!(*got)[4].isDir); // NPDRM low byte 0x01 is a FILE
        CHECK((*got)[5].path == QStringLiteral("USRDIR/patch.sdat"));
        CHECK((*got)[5].size == 33);                        // SDAT low byte 0x09 is a FILE
        CHECK((*got)[6].size == 0);
    }

    // Not a pkg / torn pkg → nullopt (fallback path), never a crash. Each case below reaches a
    // DIFFERENT header guard; a short file would short-circuit at the first one and leave the rest
    // untested, so the magic and size guards get full-length files of their own.
    CHECK(!Ps3Pkg::entries(writePkg("nomagic.pkg", QByteArray("garbage bytes"))).has_value()); // short read
    CHECK(!Ps3Pkg::entries(tmp.path() + QStringLiteral("/absent.pkg")).has_value());           // no file
    QByteArray truncated = makePkg(lbpShapedItems(), riv);
    truncated.truncate(0x60);                                                    // < 0x80: header short read
    CHECK(!Ps3Pkg::entries(writePkg("trunc.pkg", truncated)).has_value());

    // Full-length file, wrong magic — reaches the magic COMPARE rather than the short-read gate.
    QByteArray badMagic = makePkg(lbpShapedItems(), riv);
    badMagic[0] = 'X';
    CHECK(!Ps3Pkg::entries(writePkg("badmagic.pkg", badMagic)).has_value());

    // Header intact but the data area is gone — reaches the dataOffset/dataSize-vs-fileSize guard.
    QByteArray shortData = makePkg(lbpShapedItems(), riv);
    shortData.truncate(0x100); // > 0x80 header, < dataOffset + dataSize
    CHECK(!Ps3Pkg::entries(writePkg("shortdata.pkg", shortData)).has_value());

    // A non-PS3 pkg type must not be decrypted with the PS3 key.
    QByteArray psp = makePkg(lbpShapedItems(), riv);
    psp[7] = 2; // pkg type 0x0002 (PSP)
    CHECK(!Ps3Pkg::entries(writePkg("psp.pkg", psp)).has_value());

    // Wrong riv in the header = the table decrypts to garbage = names fail sanity → nullopt.
    // This is the self-guard that routes debug/foreign pkgs into the fallback instead of
    // "verifying" against noise.
    QByteArray wrongRiv = makePkg(lbpShapedItems(), riv);
    wrongRiv.replace(0x70, 16, QByteArray(16, '\x42'));
    CHECK(!Ps3Pkg::entries(writePkg("wrongriv.pkg", wrongRiv)).has_value());

    // A wild item count is capped, not looped over: 0x00FFFFFF trips the absolute cap...
    CHECK(!Ps3Pkg::entries(writePkg("count.pkg",
        makePkg(lbpShapedItems(), riv, 0x00FFFFFF))).has_value());
    // ...while 99999 sits UNDER the cap and must still be rejected, by the guard that says a table
    // of itemCount*32 bytes cannot be larger than the data area that is supposed to hold it. Without
    // this second case the cap alone would answer for both and the containment guard goes untested.
    CHECK(!Ps3Pkg::entries(writePkg("count2.pkg",
        makePkg(lbpShapedItems(), riv, 99999))).has_value());

    // Trailing padding is what a real pkg looks like (Sony's 0x20 sha1 footer sits after the data
    // area), and it is what lets an overstated header field be READ back successfully and still be a
    // lie. Without it a short read rejects both of these on its own and the guards go untested.
    const QByteArray padded = makePkg(lbpShapedItems(), riv) + QByteArray(4096, '\xEE');

    // Overstated item count. The 13 rows past the real 7 decrypt to padding, so the table dies on
    // those rows' names — this pins the BEHAVIOUR (a readable-but-wrong count never yields entries)
    // rather than the itemCount*32 > dataSize guard itself, whose job is to bound the read.
    QByteArray bigCount = padded;
    bigCount.replace(0x14, 4, be32Bytes(20)); // 20*32 = 640 bytes of "table" vs a 460-byte data area
    CHECK(!Ps3Pkg::entries(writePkg("bigcount.pkg", bigCount)).has_value());

    // Overstated data size. The 7-row table is entirely readable, so nothing downstream objects and
    // the dataSize-vs-fileSize guard is the only thing standing between this and a parse.
    QByteArray bigSize = padded;
    bigSize.replace(0x28, 8, be64Bytes(0x7FFFFFFFull));
    CHECK(!Ps3Pkg::entries(writePkg("bigsize.pkg", bigSize)).has_value());

    // One entry claims a payload running off the end of the data area while its name — and every
    // other entry — stays clean. Nothing but the per-entry data-window check stands between this and
    // a verifier told to expect a 2GB patch.sdat.
    QByteArray bigFile = makePkg(lbpShapedItems(), riv);
    patchTableField(bigFile, 0x90, riv, 5 * 32 + 16, be64Bytes(0x7FFFFFFFull)); // entry 5's fileSize
    CHECK(!Ps3Pkg::entries(writePkg("bigfile.pkg", bigFile)).has_value());

    // Positive control for patchTableField itself. Every other use of it asserts a REJECTION, and a
    // helper that mangled the surrounding block (wrong counter position, wrong span) would satisfy
    // those vacuously. Rewriting entry 5's fileSize with its TRUE value must leave the pkg parsing
    // exactly as the untouched one did. Together with bigfile.pkg above — which a no-op helper would
    // fail — this pins the helper in both directions.
    QByteArray repatched = makePkg(lbpShapedItems(), riv);
    patchTableField(repatched, 0x90, riv, 5 * 32 + 16, be64Bytes(33));
    const auto same = Ps3Pkg::entries(writePkg("repatched.pkg", repatched));
    CHECK(same.has_value());
    if (same)
    {
        CHECK(same->size() == 7);
        CHECK((*same)[5].path == QStringLiteral("USRDIR/patch.sdat"));
        CHECK((*same)[5].size == 33);
    }

    // Escaping or malformed names must poison the WHOLE table: a verifier must never stat outside
    // gameDir, and a table this key demonstrably did not decrypt must never look parseable.
    const QVector<QByteArray> evilNames = {
        QByteArray("../evil.bin"),        // parent walk
        QByteArray("/abs.bin"),           // absolute, POSIX form
        QByteArray("C:/evil.bin"),        // absolute, WINDOWS drive form: no '\\', no leading '/',
                                          // no ".." — and QDir::filePath() returns it UNCHANGED.
                                          // Must reject on EVERY platform, so this row also guards
                                          // against leaning on QDir's Q_OS_WIN-only drive branch.
        QByteArray("C:evil.bin"),         // drive-RELATIVE: absolute to Qt on Windows ONLY — the explicit drive clause must reject it everywhere
        QByteArray("USR\\DIR.bin"),       // backslash
        QByteArray("USRDIR/.."),          // trailing parent walk (no "../" substring)
        QByteArray(".."),                 // bare parent
        QByteArray("USR\x01" "DIR.bin"),  // interior control byte (split literal: not \x1D)
        QByteArray("\xC3("),              // invalid UTF-8 -> U+FFFD
        QByteArray(4, '\0'),              // nothing left after the trailing-NUL strip
        QByteArray(5000, 'a'),            // past the 4096 nameSize cap
    };
    for (const QByteArray& evil : evilNames)
    {
        auto items = lbpShapedItems();
        items[5].name = evil;
        CHECK(!Ps3Pkg::entries(writePkg(QStringLiteral("evil.pkg"), makePkg(items, riv))).has_value());
    }
}

// Lay the fixture's items down on disk exactly as a COMPLETE install would: dirs as dirs, files at
// their table sizes. Every poisoned shape below is then a deliberate edit away from this baseline.
static void materialize(const QString& gameDir, const QVector<PkgFixtureEntry>& items)
{
    for (const auto& it : items)
    {
        const QString p = gameDir + '/' + QString::fromUtf8(it.name);
        if ((it.type & 0xFF) == 0x04) { QDir().mkpath(p); continue; }
        QDir().mkpath(QFileInfo(p).path());
        QFile f(p); f.open(QIODevice::WriteOnly); f.write(it.data);
    }
}

// The verification a success verdict now owes the tree: every table entry present at its expected
// size. The hardware failure this pins (2026-08-19, BCUS98148): PARAM.SFO claimed APP_VER=01.30
// while USRDIR/patch.sdat sat at 0 bytes and 6 files were missing entirely — version+quiescence
// passed and the game crashed ~14s after boot when the EBOOT loaded the empty sdat.
static void testVerifyInstalled()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const auto items = lbpShapedItems();
    const QByteArray riv = QByteArray::fromHex("4309f292bd44abd6788293457fd4a8a4");
    const QString pkgPath = tmp.path() + QStringLiteral("/u.pkg");
    { QFile f(pkgPath); CHECK(f.open(QIODevice::WriteOnly)); f.write(makePkg(items, riv)); }
    const auto table = Ps3Pkg::entries(pkgPath);
    CHECK(table.has_value());
    if (!table) return;

    const QString g = tmp.path() + QStringLiteral("/game");
    materialize(g, items);
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // complete install verifies

    // Extra files RPCS3 didn't write (runtime game data, older update leftovers) are none of the
    // table's business.
    { QFile f(g + QStringLiteral("/USRDIR/leftover.bin"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // THE poisoned-install case: the expected-910064-byte sdat truncated to 0 bytes while
    // PARAM.SFO still claims the target — must FAIL even though every path exists.
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(33, 'D')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // healed

    // A missing file — the other half of the hardware poison.
    CHECK(QFile::remove(g + QStringLiteral("/USRDIR/EBOOT.BIN")));
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(100, 'E')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // An overwrite entry at the WRONG size (torn mid-write, then killed) fails too.
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::Append)); f.write("tail"); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/USRDIR/EBOOT.BIN"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(QByteArray(100, 'E')); }

    // A NON-overwrite entry may keep a pre-existing file of a different size — RPCS3's
    // "Didn't overwrite" path (unpkg.cpp) skips it, so a size mismatch there is legitimate…
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(QByteArray(999, 'O')); }
    CHECK(Ps3Pkg::verifyInstalled(g, *table));
    // …but 0 bytes where the table expects content is never legitimate, overwrite bit or not.
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    { QFile f(g + QStringLiteral("/ICON0.PNG"));
      CHECK(f.open(QIODevice::WriteOnly)); f.write(QByteArray(5, 'I')); }

    // A 0-byte file the table EXPECTS at 0 bytes is fine (some updates ship empty markers).
    CHECK(Ps3Pkg::verifyInstalled(g, *table)); // USRDIR/empty.bin is 0 bytes by design

    // A directory entry the install never produced.
    CHECK(QDir(g + QStringLiteral("/USRDIR/output")).removeRecursively());
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));
    QDir().mkpath(g + QStringLiteral("/USRDIR/output"));
    CHECK(Ps3Pkg::verifyInstalled(g, *table));

    // The wiring rule the installer lambda applies (Task 6), composed here where a probe can reach
    // it: version-at-target alone said "done", the table says otherwise, and the restore then
    // un-tells the lie so the next launch re-runs the chain.
    const QByteArray priorSfo = makeSfo({ { "APP_VER", "01.02" }, { "TITLE_ID", "BCUS98148" } });
    { QFile f(g + QStringLiteral("/PARAM.SFO"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(priorSfo); }
    const QByteArray entrySnap = Ps3InstalledVersion::snapshotSfo(g);
    { QFile f(g + QStringLiteral("/PARAM.SFO")); CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(makeSfo({ { "APP_VER", "01.30" }, { "TITLE_ID", "BCUS98148" } })); }
    { QFile f(g + QStringLiteral("/USRDIR/patch.sdat"));
      CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); } // the 0-byte poison
    CHECK(Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.30"))); // the old check passes…
    CHECK(!Ps3Pkg::verifyInstalled(g, *table));                            // …the table does not
    Ps3InstalledVersion::restoreSfo(g, entrySnap);
    CHECK(!Ps3InstalledVersion::reachedTarget(g, QStringLiteral("01.30"))); // lie undone
}

static void testHasZeroByteFile()
{
    QTemporaryDir tmp; CHECK(tmp.isValid());
    // Missing or empty dirs hold no poison — this must never make a fresh install look poisoned.
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(tmp.path() + QStringLiteral("/absent")));
    const QString d = tmp.path() + QStringLiteral("/usr");
    QDir().mkpath(d + QStringLiteral("/deep"));
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(d));
    { QFile f(d + QStringLiteral("/deep/ok.bin")); CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
    CHECK(!Ps3InstalledVersion::hasZeroByteFile(d));
    { QFile f(d + QStringLiteral("/deep/poison.sdat")); CHECK(f.open(QIODevice::WriteOnly)); }
    CHECK(Ps3InstalledVersion::hasZeroByteFile(d));
}

int main()
{
    testSfo();
    testFeed();
    testState();
    testInstaller();
    testInstalledVersion();
    testCompletedDespiteKill();
    testSfoRollback();
    testTitleId();
    testCoordinator();
    testPkgCrypt();
    testPkgEntries();
    testVerifyInstalled();
    testHasZeroByteFile();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("PS3UPDATE-OK\n");
    return 0;
}
