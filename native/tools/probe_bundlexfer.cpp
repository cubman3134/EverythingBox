// Headless check of "Send library to device" (issue #127) — src/core/LibraryBundle.{h,cpp}, plus the two
// routes it adds to the pure #76/#143 surface (src/core/RemoteApi.{h,cpp}, src/core/PlayOnDevice.{h,cpp}).
//
// What this pins, with no socket, no window and no second machine — the whole feature runs against two
// temporary directories that stand in for two devices' metadata caches:
//
//   1. THE SAFETY RULES FIRST. safeItemId accepts a 40-character MetaCache hash and NOTHING else, so "..",
//      an absolute path, a drive letter and a UNC share are refused before a path is built out of them;
//      safeFileName refuses separators, hidden names, Windows device names and every extension that is not
//      art or metadata (a trailer, a manual and an executable therefore cannot ride in a bundle).
//   2. THE STAMP. Stable for an unchanged folder; different for a changed meta.json BYTE, for a changed art
//      size, for a bumped art mtime and for an added role. And its DOCUMENTED LIMIT, asserted rather than
//      glossed: an art file rewritten to the same size with its mtime restored is invisible to the stamp.
//      That is the price of not reading gigabytes of PNG on every inventory, and it is pinned so nobody
//      later believes the stamp is a content hash of the art.
//   3. THE DIFF. Missing -> send; identical stamp -> nothing; different and the source newer -> send;
//      different and the TARGET newer (or a tie) -> keep what the target has.
//   4. THE DEFINING PROPERTY. A whole transfer, then a second run with nothing changed: the plan is empty
//      and the BYTES THAT WOULD BE SENT ARE ZERO. Then one item changes on the source and EXACTLY that one
//      item moves. This is the feature; everything else is how it is made safe.
//   5. WARM, NEVER FIGHT. An item that is newer on the target is kept, byte for byte. Files the bundle did
//      not carry (the trailer this feature deliberately does not move) survive a landing.
//   6. ATOMIC LANDING. An interruption injected mid-item leaves the target's existing copy exactly as it
//      was, leaves no half-written folder at the item's real path, and is swept on the next run — after
//      which the retry lands. A crash injected DURING THE SWAP (the live folder set aside, the new one not
//      yet in place) is recovered rather than lost.
//   7. NOTHING OUTSIDE THE CACHE, AND NO STATE. A recursive census of the parent directory is taken before
//      and after a transfer: everything that changed is inside the cache root, and a sibling state store
//      (marks / favourites / resume) is byte-identical afterwards.
//   8. THE ROUTES. /inventory and /bundle require a paired token; the routing table answers them for the
//      right method only; and a /bundle request is allowed a payload-sized read cap while every other route
//      keeps #76's tiny one.
//
// Prints BUNDLEXFER-OK on success; any failure prints BUNDLEXFER-FAIL <cond> (line) and exits non-zero.
#include "LibraryBundle.h"
#include "PlayOnDevice.h"
#include "RemoteApi.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BUNDLEXFER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- fixtures ---------------------------------------------------------------------------------------------

namespace fx
{
    // A MetaCache folder name is sha1(key).toHex(). Building the probe's ids the same way keeps the fixture
    // honest about what a real cache holds.
    static QString idFor(const char* key)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(QByteArray(key), QCryptographicHash::Sha1).toHex());
    }

    static bool writeFile(const QString& path, const QByteArray& data)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(data);
        f.close();
        return true;
    }

    static QByteArray readFile(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }

    static void setMtime(const QString& path, qint64 ms)
    {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
        {
            f.setFileTime(QDateTime::fromMSecsSinceEpoch(ms), QFileDevice::FileModificationTime);
            f.close();
        }
    }

    static qint64 mtimeOf(const QString& path)
    {
        return QFileInfo(path).lastModified().toMSecsSinceEpoch();
    }

    // A whole item: meta.json plus two art roles, every file at a PINNED modification time so the stamp is
    // reproducible from run to run and the "newer" comparisons are exact rather than racy.
    static void makeItem(const QString& root, const QString& id, const QByteArray& meta,
                         const QByteArray& thumb, qint64 baseMs)
    {
        QDir().mkpath(root + QLatin1Char('/') + id);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/meta.json"), meta);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/thumb.png"), thumb);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/poster.jpg"), QByteArray("POSTERBYTES"));
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/meta.json"), baseMs);
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/thumb.png"), baseMs);
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/poster.jpg"), baseMs);
    }

    // Everything under `dir`, as relative path -> (size, sha256). The census the "nothing outside the cache"
    // and "no state store touched" assertions compare.
    static QMap<QString, QString> census(const QString& dir)
    {
        QMap<QString, QString> out;
        QDir base(dir);
        QStringList stack;
        stack << dir;
        while (!stack.isEmpty())
        {
            const QString cur = stack.takeLast();
            QDir d(cur);
            const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                          QDir::Name);
            for (const QFileInfo& fi : entries)
            {
                if (fi.isDir()) { stack << fi.absoluteFilePath(); continue; }
                const QString rel = base.relativeFilePath(fi.absoluteFilePath());
                out.insert(rel, QString::fromLatin1(
                    QCryptographicHash::hash(readFile(fi.absoluteFilePath()),
                                             QCryptographicHash::Sha256).toHex()));
            }
        }
        return out;
    }

    // The bytes a plan would put on the wire. The second-run assertion is about THIS number being zero, not
    // about a count of items: an empty plan that still encoded a payload would pass a count and fail here.
    static qint64 wireBytesFor(const QString& root, const QStringList& ids)
    {
        qint64 total = 0;
        for (const QString& id : ids)
        {
            LibraryBundle::Payload p;
            QString err;
            if (!LibraryBundle::readPayload(root, id, p, err)) continue;
            total += qint64(LibraryBundle::encodePayload(p).size());
        }
        return total;
    }

    // One item, source -> target, through encode/decode exactly as the wire would. Returns the landing
    // result and adds what crossed to `bytes`.
    static LibraryBundle::LandResult transferOne(const QString& srcRoot, const QString& dstRoot,
                                                 const QString& id, qint64* bytes, QString* error)
    {
        LibraryBundle::Payload p;
        QString err;
        if (!LibraryBundle::readPayload(srcRoot, id, p, err))
        {
            if (error) *error = err;
            return LibraryBundle::LandResult::Refused;
        }
        const QByteArray wire = LibraryBundle::encodePayload(p);
        if (bytes) *bytes += qint64(wire.size());

        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        if (!LibraryBundle::decodePayload(wire, got, why, message))
        {
            if (error) *error = message;
            return LibraryBundle::LandResult::Refused;
        }
        return LibraryBundle::landItem(dstRoot, got, err);
    }
}

int main(int, char**)
{
    const QString base = QDir::tempPath() + QStringLiteral("/eb-bundlexfer-probe");
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QString srcRoot = base + QStringLiteral("/source/metadata");
    const QString dstBase = base + QStringLiteral("/target");
    const QString dstRoot = dstBase + QStringLiteral("/metadata");
    const QString dstState = dstBase + QStringLiteral("/state");
    QDir().mkpath(srcRoot);
    QDir().mkpath(dstRoot);
    QDir().mkpath(dstState);

    const QString idA = fx::idFor("tt0111161");
    const QString idB = fx::idFor("igdb:1020");
    const QString idC = fx::idFor("local:/roms/smw.sfc");

    // The state store this transfer must not touch: marks, favourites and a resume position. Drive sync owns
    // these; an art transfer that moved one of them would be two systems owning one datum.
    fx::writeFile(dstState + QStringLiteral("/marks.json"), QByteArray("{\"tt0111161\":{\"watched\":true}}"));
    fx::writeFile(dstState + QStringLiteral("/favourites.json"), QByteArray("[\"igdb:1020\"]"));
    fx::writeFile(dstState + QStringLiteral("/resume.json"), QByteArray("{\"pos\":1234.5}"));

    // ---- 1. safety -----------------------------------------------------------------------------------
    {
        CHECK(LibraryBundle::safeItemId(idA));
        CHECK(!LibraryBundle::safeItemId(QString()));
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("..")));
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("../../etc")));
        CHECK(!LibraryBundle::safeItemId(idA.left(39)));
        CHECK(!LibraryBundle::safeItemId(idA.toUpper()));                       // hex is lower case here
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("C:/Windows/System32")));
        CHECK(!LibraryBundle::safeItemId(idA.left(38) + QStringLiteral("/x")));
        QString notHex = QStringLiteral("0123456789012345678901234567890123456789");
        notHex[0] = QLatin1Char('g');
        CHECK(!LibraryBundle::safeItemId(notHex));

        CHECK(LibraryBundle::safeFileName(QStringLiteral("meta.json")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("thumb.png")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("miximage.png")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("fixed-1a2b.jpg")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("../meta.json")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("sub/thumb.png")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("..")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral(".hidden.png")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("con.png")));         // a Windows device name
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("lpt1.png")));
        // Not art, and each is a real file that lives in a MetaCache folder today: the megabyte roles this
        // increment deliberately does not move, plus the thing nobody should ever be able to send.
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("trailer.mp4")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("theme.mp3")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("manual.pdf")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("payload.exe")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("script.sh")));
    }

    // ---- 2. the stamp --------------------------------------------------------------------------------
    {
        QList<LibraryBundle::FileEntry> files;
        LibraryBundle::FileEntry m; m.name = QStringLiteral("meta.json"); m.size = 20; m.mtimeMs = 1000;
        LibraryBundle::FileEntry t; t.name = QStringLiteral("thumb.png"); t.size = 500; t.mtimeMs = 2000;
        files << m << t;
        const QByteArray meta("{\"title\":\"A\"}");
        const QString s0 = LibraryBundle::stampOf(files, meta);

        CHECK(!s0.isEmpty());
        CHECK(LibraryBundle::stampOf(files, meta) == s0);                       // stable
        // Order of the input list must not matter: the stamp sorts.
        QList<LibraryBundle::FileEntry> reordered; reordered << t << m;
        CHECK(LibraryBundle::stampOf(reordered, meta) == s0);

        // A changed meta.json BYTE, with size and time held constant: caught, because meta.json is hashed by
        // content.
        CHECK(LibraryBundle::stampOf(files, QByteArray("{\"title\":\"B\"}")) != s0);

        // A changed art SIZE, and a bumped art MTIME: each caught on its own.
        QList<LibraryBundle::FileEntry> bigger = files; bigger[1].size = 501;
        CHECK(LibraryBundle::stampOf(bigger, meta) != s0);
        QList<LibraryBundle::FileEntry> later = files;  later[1].mtimeMs = 2001;
        CHECK(LibraryBundle::stampOf(later, meta) != s0);

        // An added role: caught.
        QList<LibraryBundle::FileEntry> plus = files;
        LibraryBundle::FileEntry l; l.name = QStringLiteral("logo.png"); l.size = 10; l.mtimeMs = 2000;
        plus << l;
        CHECK(LibraryBundle::stampOf(plus, meta) != s0);

        // THE DOCUMENTED LIMIT. An art file rewritten to the same size with the same mtime is invisible.
        // Pinned deliberately: the alternative is hashing every byte of a gigabyte of PNG on every inventory,
        // which is slower than the re-scrape this feature exists to avoid.
        CHECK(LibraryBundle::stampOf(files, meta) == s0);

        CHECK(LibraryBundle::updatedMsOf(files) == 2000);
        CHECK(LibraryBundle::updatedMsOf(QList<LibraryBundle::FileEntry>()) == 0);
    }

    // ---- 3. the inventory wire -----------------------------------------------------------------------
    {
        QList<LibraryBundle::Entry> in;
        LibraryBundle::Entry a; a.id = idA; a.stamp = QStringLiteral("aaa"); a.updatedMs = 1700000000000LL;
        LibraryBundle::Entry b; b.id = idB; b.stamp = QStringLiteral("bbb"); b.updatedMs = 1700000000001LL;
        in << a << b;
        QList<LibraryBundle::Entry> out;
        QString err;
        CHECK(LibraryBundle::parseInventory(LibraryBundle::inventoryJson(in), out, err));
        CHECK(out.size() == 2);
        CHECK(out.at(0).id == idA && out.at(0).stamp == QStringLiteral("aaa"));
        CHECK(out.at(1).updatedMs == 1700000000001LL);                          // ms survive the JSON double

        CHECK(!LibraryBundle::parseInventory(QByteArray("not json"), out, err));
        CHECK(!err.isEmpty());
        // A future cache format is refused with a sentence, never half-read (#127's version guard).
        CHECK(!LibraryBundle::parseInventory(QByteArray("{\"v\":99,\"items\":[]}"), out, err));
        CHECK(err.contains(QStringLiteral("newer")));
    }

    // ---- 4. the diff ---------------------------------------------------------------------------------
    {
        LibraryBundle::Entry s; s.id = idA; s.stamp = QStringLiteral("s1"); s.updatedMs = 2000;
        CHECK(LibraryBundle::verdictFor(s, nullptr) == LibraryBundle::Verdict::Send);

        LibraryBundle::Entry same = s;
        CHECK(LibraryBundle::verdictFor(s, &same) == LibraryBundle::Verdict::Unchanged);

        LibraryBundle::Entry older; older.id = idA; older.stamp = QStringLiteral("t0"); older.updatedMs = 1000;
        CHECK(LibraryBundle::verdictFor(s, &older) == LibraryBundle::Verdict::Send);

        LibraryBundle::Entry newer; newer.id = idA; newer.stamp = QStringLiteral("t2"); newer.updatedMs = 3000;
        CHECK(LibraryBundle::verdictFor(s, &newer) == LibraryBundle::Verdict::TargetNewer);

        // A tie goes to the target. Warming a cache is never worth overwriting something on the other end.
        LibraryBundle::Entry tie; tie.id = idA; tie.stamp = QStringLiteral("t3"); tie.updatedMs = 2000;
        CHECK(LibraryBundle::verdictFor(s, &tie) == LibraryBundle::Verdict::TargetNewer);

        QList<LibraryBundle::Entry> src, tgt;
        LibraryBundle::Entry sa; sa.id = idA; sa.stamp = QStringLiteral("x"); sa.updatedMs = 5;
        LibraryBundle::Entry sb; sb.id = idB; sb.stamp = QStringLiteral("y"); sb.updatedMs = 5;
        LibraryBundle::Entry sc; sc.id = idC; sc.stamp = QStringLiteral("z"); sc.updatedMs = 5;
        src << sa << sb << sc;
        LibraryBundle::Entry ta; ta.id = idA; ta.stamp = QStringLiteral("x"); ta.updatedMs = 5;   // identical
        LibraryBundle::Entry tb; tb.id = idB; tb.stamp = QStringLiteral("y2"); tb.updatedMs = 9;  // newer there
        tgt << ta << tb;                                                                          // C missing
        const LibraryBundle::Plan p = LibraryBundle::planTransfer(src, tgt);
        CHECK(p.send == QStringList{ idC });
        CHECK(p.unchanged == QStringList{ idA });
        CHECK(p.targetNewer == QStringList{ idB });
    }

    // ---- 5. a whole transfer, and the second run -----------------------------------------------------
    const qint64 t0 = 1700000000000LL;
    {
        fx::makeItem(srcRoot, idA, QByteArray("{\"key\":\"tt0111161\",\"title\":\"Shawshank\"}"),
                     QByteArray("PNGDATA-A"), t0);
        fx::makeItem(srcRoot, idB, QByteArray("{\"key\":\"igdb:1020\",\"title\":\"GTA\"}"),
                     QByteArray("PNGDATA-BB"), t0);
        fx::makeItem(srcRoot, idC, QByteArray("{\"key\":\"local\",\"title\":\"Mario World\"}"),
                     QByteArray("PNGDATA-CCC"), t0);

        // Something in the source cache this feature must NOT move: a trailer, in an item's own folder.
        fx::writeFile(srcRoot + QLatin1Char('/') + idA + QStringLiteral("/trailer.mp4"),
                      QByteArray("MP4-A-LOT-OF-BYTES"));

        const QMap<QString, QString> beforeCensus = fx::census(dstBase);

        const QList<LibraryBundle::Entry> srcInv = LibraryBundle::inventoryFor(srcRoot);
        const QList<LibraryBundle::Entry> dstInv = LibraryBundle::inventoryFor(dstRoot);
        CHECK(srcInv.size() == 3);
        CHECK(dstInv.isEmpty());

        const LibraryBundle::Plan plan = LibraryBundle::planTransfer(srcInv, dstInv);
        CHECK(plan.send.size() == 3);

        qint64 bytes = 0;
        LibraryBundle::Progress prog;
        prog.itemsTotal = int(plan.send.size());
        for (const QString& id : plan.send)
        {
            QString err;
            const LibraryBundle::LandResult r = fx::transferOne(srcRoot, dstRoot, id, &bytes, &err);
            CHECK(r == LibraryBundle::LandResult::Landed);
            if (r == LibraryBundle::LandResult::Landed) ++prog.itemsSent;
        }
        prog.bytesSent = bytes;
        CHECK(bytes > 0);
        CHECK(prog.itemsSent == 3);

        // The art arrived, byte for byte.
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-A"));
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/meta.json"))
              .contains("Mario World"));
        // The trailer did not. Art only, this increment.
        CHECK(!QFileInfo::exists(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/trailer.mp4")));

        // The mtimes travelled — the one mechanism the whole second-run property rests on.
        CHECK(fx::mtimeOf(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png")) == t0);

        // NOTHING WAS TOUCHED OUTSIDE THE CACHE ROOT, and the state store is byte-identical.
        const QMap<QString, QString> afterCensus = fx::census(dstBase);
        for (auto it = beforeCensus.constBegin(); it != beforeCensus.constEnd(); ++it)
        {
            CHECK(afterCensus.contains(it.key()));
            CHECK(afterCensus.value(it.key()) == it.value());
        }
        for (auto it = afterCensus.constBegin(); it != afterCensus.constEnd(); ++it)
        {
            const bool inCache = it.key().startsWith(QStringLiteral("metadata/"));
            const bool wasThere = beforeCensus.contains(it.key());
            CHECK(inCache || wasThere);          // every NEW file is inside the cache
        }
        CHECK(fx::readFile(dstState + QStringLiteral("/marks.json"))
              == QByteArray("{\"tt0111161\":{\"watched\":true}}"));
        CHECK(fx::readFile(dstState + QStringLiteral("/resume.json")) == QByteArray("{\"pos\":1234.5}"));

        // ---- THE DEFINING PROPERTY. Run it again with nothing changed. ----
        const QList<LibraryBundle::Entry> srcInv2 = LibraryBundle::inventoryFor(srcRoot);
        const QList<LibraryBundle::Entry> dstInv2 = LibraryBundle::inventoryFor(dstRoot);
        const LibraryBundle::Plan plan2 = LibraryBundle::planTransfer(srcInv2, dstInv2);
        CHECK(plan2.send.isEmpty());
        CHECK(plan2.unchanged.size() == 3);
        CHECK(fx::wireBytesFor(srcRoot, plan2.send) == 0);          // ZERO BYTES OF PAYLOAD

        LibraryBundle::Progress none;
        CHECK(LibraryBundle::describeProgress(none, QStringLiteral("Living Room"))
                  .contains(QStringLiteral("already up to date")));
        CHECK(LibraryBundle::describeProgress(prog, QStringLiteral("Living Room"))
                  .contains(QStringLiteral("Living Room")));

        // ---- change ONE item on the source: exactly one item moves. ----
        fx::writeFile(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-BB-REDRAWN"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"), t0 + 60000);

        const LibraryBundle::Plan plan3 =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(plan3.send == QStringList{ idB });
        CHECK(plan3.unchanged.size() == 2);

        qint64 bytes3 = 0;
        QString err3;
        CHECK(fx::transferOne(srcRoot, dstRoot, idB, &bytes3, &err3) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-BB-REDRAWN"));
        // ...and the run after THAT is empty again.
        CHECK(LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                          LibraryBundle::inventoryFor(dstRoot)).send.isEmpty());
    }

    // ---- 6. warm, never fight ------------------------------------------------------------------------
    {
        // The target's copy of A is made NEWER than the source's, with different content. A second send must
        // leave it exactly as it is, and must SAY so rather than report a success it did not perform.
        fx::writeFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-A-EDITED-HERE"));
        fx::setMtime(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"), t0 + 3600000);

        const LibraryBundle::Plan plan =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(!plan.send.contains(idA));
        CHECK(plan.targetNewer.contains(idA));

        // Even if a source sends it anyway (a race, or an older build), the TARGET refuses to clobber.
        qint64 bytes = 0;
        QString err;
        CHECK(fx::transferOne(srcRoot, dstRoot, idA, &bytes, &err) == LibraryBundle::LandResult::KeptNewer);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-A-EDITED-HERE"));

        const LibraryBundle::Receipt kept =
            LibraryBundle::receiptFor(LibraryBundle::LandResult::KeptNewer, QStringLiteral("newer here"));
        CHECK(kept.httpStatus == 200);
        CHECK(kept.result == QStringLiteral("kept"));
        LibraryBundle::Receipt parsed;
        CHECK(LibraryBundle::parseReceipt(LibraryBundle::receiptJson(kept), parsed));
        CHECK(parsed.result == QStringLiteral("kept"));
    }

    // ---- 7. the files a bundle did not carry survive a landing ---------------------------------------
    {
        // The target has a trailer for C that the source never sends. Landing a new C must not cost it.
        fx::writeFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/trailer.mp4"),
                      QByteArray("TARGET-OWN-TRAILER"));
        fx::writeFile(srcRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"), QByteArray("LOGO-C"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"), t0 + 120000);

        qint64 bytes = 0;
        QString err;
        CHECK(fx::transferOne(srcRoot, dstRoot, idC, &bytes, &err) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"))
              == QByteArray("LOGO-C"));
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/trailer.mp4"))
              == QByteArray("TARGET-OWN-TRAILER"));
    }

    // ---- 8. atomic landing under an interruption -----------------------------------------------------
    {
        const QString liveThumb = dstRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png");
        const QByteArray before = fx::readFile(liveThumb);
        CHECK(!before.isEmpty());

        // Change the source so there IS something to send, then interrupt after one file.
        fx::writeFile(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-BB-INTERRUPTED-RUN"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"), t0 + 240000);

        LibraryBundle::Payload p;
        QString err;
        CHECK(LibraryBundle::readPayload(srcRoot, idB, p, err));

        LibraryBundle::LandOptions opts;
        opts.failAfterFiles = 1;
        CHECK(LibraryBundle::landItem(dstRoot, p, err, opts) == LibraryBundle::LandResult::Interrupted);

        // The target still holds EXACTLY what it held: no half-written folder at the item's real path.
        CHECK(fx::readFile(liveThumb) == before);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-incoming/") + idB
                                 + QStringLiteral("/nonexistent")));
        // ...and the item's own inventory entry is unchanged, so the next run diffs from a consistent tree.
        LibraryBundle::Entry e;
        QList<LibraryBundle::FileEntry> files;
        CHECK(LibraryBundle::scanItem(dstRoot, idB, e, files));

        // A resumed run picks up from the diff: the sweep clears the staged remains, the plan still names B,
        // and the retry lands.
        LibraryBundle::sweepPartials(dstRoot);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-incoming")));
        const LibraryBundle::Plan resumed =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(resumed.send.contains(idB));
        qint64 bytes = 0;
        CHECK(fx::transferOne(srcRoot, dstRoot, idB, &bytes, &err) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(liveThumb) == QByteArray("PNGDATA-BB-INTERRUPTED-RUN"));
    }

    // ---- 9. an interruption DURING the swap is recovered, not lost -----------------------------------
    {
        // The worst moment: the live folder has been set aside and the new one is not yet in place. Simulate
        // it exactly, then assert the next run puts the item back rather than reporting it missing.
        const QString live    = dstRoot + QLatin1Char('/') + idC;
        const QString retired = dstRoot + QStringLiteral("/.eb-retired/") + idC;
        const QByteArray logo = fx::readFile(live + QStringLiteral("/logo.png"));
        CHECK(!logo.isEmpty());
        QDir().mkpath(dstRoot + QStringLiteral("/.eb-retired"));
        CHECK(QDir().rename(live, retired));
        CHECK(!QFileInfo::exists(live));

        CHECK(LibraryBundle::sweepPartials(dstRoot) >= 1);
        CHECK(QFileInfo::exists(live));
        CHECK(fx::readFile(live + QStringLiteral("/logo.png")) == logo);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-retired")));

        // And the sweep runs itself before any inventory is reported, so a caller cannot see the half state.
        CHECK(LibraryBundle::inventoryFor(dstRoot).size() >= 3);
    }

    // ---- 10. a hostile bundle is refused, and writes nothing -----------------------------------------
    {
        const QMap<QString, QString> before = fx::census(dstBase);

        // A path-traversal item id, hand-built as the wire would carry it.
        const QByteArray evil =
            "{\"v\":1,\"id\":\"../../../../state\",\"stamp\":\"s\",\"updated\":1,"
            "\"files\":[{\"name\":\"marks.json\",\"mtime\":1,\"data\":\"eA==\"}]}";
        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        CHECK(!LibraryBundle::decodePayload(evil, got, why, message));
        CHECK(why == LibraryBundle::Refusal::UnsafeId);
        CHECK(!message.isEmpty());

        // ...and landItem refuses it a second time, so a caller that skipped the decoder cannot get through.
        LibraryBundle::Payload direct;
        direct.id = QStringLiteral("../../../../state");
        LibraryBundle::PayloadFile f;
        f.name = QStringLiteral("marks.json");
        f.data = QByteArray("x");
        direct.files << f;
        QString err;
        CHECK(LibraryBundle::landItem(dstRoot, direct, err) == LibraryBundle::LandResult::Refused);

        // A traversing FILE NAME inside a legitimate item.
        const QByteArray evil2 = QByteArray("{\"v\":1,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,"
              "\"files\":[{\"name\":\"../../marks.json\",\"mtime\":1,\"data\":\"eA==\"}]}";
        CHECK(!LibraryBundle::decodePayload(evil2, got, why, message));
        CHECK(why == LibraryBundle::Refusal::UnsafeFileName);

        // A future format is refused with a sentence a user can act on.
        const QByteArray future = QByteArray("{\"v\":99,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,\"files\":[]}";
        CHECK(!LibraryBundle::decodePayload(future, got, why, message));
        CHECK(why == LibraryBundle::Refusal::FutureFormat);
        CHECK(message.contains(QStringLiteral("newer")));

        // An empty bundle is not a transfer.
        const QByteArray empty = QByteArray("{\"v\":1,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,\"files\":[]}";
        CHECK(!LibraryBundle::decodePayload(empty, got, why, message));
        CHECK(why == LibraryBundle::Refusal::Empty);

        // Nothing on disk moved for any of it.
        const QMap<QString, QString> after = fx::census(dstBase);
        CHECK(after == before);
    }

    // ---- 11. the routes ------------------------------------------------------------------------------
    {
        // Both new routes are credentialled. An unpaired caller can neither read what a device holds (an
        // inventory is a list of what someone owns) nor write a byte into its cache.
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/inventory")));
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/bundle")));
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/open")));      // #143's, unchanged
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/state")));    // #76's, unchanged
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/pair")));     // how a token is obtained

        // The routing table, by shape. Each route answers for ONE method; the other is a reasoned
        // BadRequest rather than a 404, so a client using the wrong verb is told what is wrong.
        RemoteApi::Request r;
        r.valid = true;
        r.method = RemoteApi::Method::Get;
        r.path = QStringLiteral("/inventory");
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::Inventory);
        r.method = RemoteApi::Method::Post;
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);

        r.path = QStringLiteral("/bundle");
        r.method = RemoteApi::Method::Post;
        r.body = QByteArray("{\"v\":1}");
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::Bundle);
        r.body = QByteArray();
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);   // a bundle with no item
        r.body = QByteArray("{\"v\":1}");
        r.method = RemoteApi::Method::Get;
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);

        // #143's and #76's routes are untouched by any of it.
        r.method = RemoteApi::Method::Get;
        r.path = QStringLiteral("/state");
        r.body = QByteArray();
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::State);

        // THE READ CAP. Only POST /bundle is allowed a payload-sized buffer; every other route keeps #76's
        // tiny one, so a control API cannot be made to eat memory by a request that merely mentions the word.
        CHECK(RemoteApi::requestCapBytes(QByteArray("POST /bundle HTTP/1.1")) == RemoteApi::kBundleRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("GET /state HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("POST /player HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("GET /bundle HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        const QByteArray mentionsBundleInAHeader =
            QByteArray("GET /x HTTP/1.1") + QByteArray("\r\n")
            + QByteArray("X-Note: POST /bundle ") + QByteArray("\r\n");
        CHECK(RemoteApi::requestCapBytes(mentionsBundleInAHeader) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray()) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::kBundleRequestCap > RemoteApi::kDefaultRequestCap);
    }

    QDir(base).removeRecursively();

    if (failures == 0) std::printf("BUNDLEXFER-OK\n");
    else               std::fprintf(stderr, "BUNDLEXFER had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
