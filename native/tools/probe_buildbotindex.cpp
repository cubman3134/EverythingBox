// Headless check of the "All cores" browser (issue #98, increments 2-3): the libretro buildbot index read,
// classified and installed from, and a buildbot-installed custom core updated — without ever touching the real
// buildbot. QtCore + QtNetwork, offscreen-safe; the data dir is this process's own scratch one
// (EB_ISOLATED_DATA_DIR), so the registry and the core files it writes cannot reach a real install.
//
// It pins:
//
//   * INDEX PARSING. A fixture `.index-extended` (date, crc, file, and the optional size) into entries; blank lines
//     ignored; malformed lines (field count, date, size) skipped AND counted, never fatal; the 4 MiB body cap
//     at its exact boundary; the 2,000-entry cap with the excess counted.
//   * NAME REFUSALS. A name is a file name on disk and a URL path segment, so traversal ("../"), a path segment
//     ("sub/", "..\\"), a drive ("C:"), a "custom:" ref, a leading dot and another platform's file shape are all
//     refused (and counted), and isValidCoreName is pinned case by case.
//   * CLASSIFICATION. catalogue / installed-custom / available, catalogue winning; a hand-loaded core matched by
//     its file; the search filter.
//   * THE HOST RULE, as a pure function: https + the exact buildbot host + default port + no user info, and
//     nothing else — and the EB_UITEST-gated override, which only ever admits a loopback base and only when
//     EB_UITEST is set. CoreManager's catalogue URL is unchanged and still empty for a "custom:" ref.
//   * THE BYTE PATH, against a loopback stub. The stub is loopback, which the host rule (correctly) refuses, so
//     the byte path is driven with the policy INJECTED: policyFor(uitest=true, <stub base>) — the same function
//     the app's test-only override goes through. The production policy is then pointed at the same stub and
//     shown to refuse it with ZERO requests reaching the stub. A redirect off the allowed origin is refused and
//     never followed; the size cap aborts.
//   * INSTALL + INSPECTION. The zip holds the fixture stub core (built from source by probe_customcore's target)
//     under a traversal path plus decoys: only the expected file is written, under our name, and it registers
//     with its source. A zip whose core file is not a core is refused and the file DELETED.
//   * UPDATES. "Update available" follows the index date (newer yes; same or older no); a hand-loaded core never
//     shows one; four failed updates (not a core, not a zip, no core file inside, HTTP 500) each keep the old
//     core's exact bytes and its registry record; a good update swaps in the new bytes and records the new date.
//
// Prints BUILDBOTINDEX-OK on success; any failure prints BUILDBOTINDEX-FAIL <cond> (line) and exits non-zero.
// Every expected value is a literal written from the fixture, never read back from the function under test.
#include "BuildbotIndex.h"
#include "BuildbotInstall.h"
#include "CustomCores.h"
#include "CustomCoreInstall.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <cstring>
#include <memory>

#include "miniz.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BUILDBOTINDEX-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- the platform's buildbot naming, written out independently of BuildbotIndex (the oracle) ---------------
#if defined(Q_OS_WIN)
static const char* kSubpath = "windows/x86_64/latest/";
static const char* kTail    = "_libretro.dll";
#elif defined(Q_OS_MACOS)
  #if defined(Q_PROCESSOR_ARM)
static const char* kSubpath = "apple/osx/arm64/latest/";
  #else
static const char* kSubpath = "apple/osx/x86_64/latest/";
  #endif
static const char* kTail    = "_libretro.dylib";
#elif defined(Q_OS_ANDROID)
static const char* kSubpath = "android/latest/arm64-v8a/";
static const char* kTail    = "_libretro_android.so";
#else
static const char* kSubpath = "linux/x86_64/latest/";
static const char* kTail    = "_libretro.so";
#endif

static QString tail() { return QString::fromLatin1(kTail); }

namespace {

// ---- a loopback HTTP stub: path -> (status, body, extra headers), recording every path requested ----------
struct Reply { int status = 200; QByteArray body; QByteArray headers; };

struct Stub
{
    QTcpServer srv;
    QHash<QString, Reply> routes;
    QStringList requested;

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            while (QTcpSocket* c = srv.nextPendingConnection())
            {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
                    buf->append(c->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray reqLine = buf->left(end).split('\n').value(0).trimmed();
                    const QString path = QString::fromLatin1(reqLine.split(' ').value(1));
                    requested << path;
                    const Reply r = routes.value(path, Reply{ 404, QByteArray("not here"), QByteArray() });
                    QByteArray resp = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\n";
                    resp += r.headers;
                    resp += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\nConnection: close\r\n\r\n";
                    resp += r.body;
                    c->write(resp);
                    c->flush();
                    c->disconnectFromHost();
                });
                QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
            }
        });
        return true;
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1/nightly/").arg(srv.serverPort()); }
    QString dirPath() const { return QStringLiteral("/nightly/") + QString::fromLatin1(kSubpath); }
};

template <typename Pred>
void pumpUntil(Pred done, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms && !done()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

QByteArray makeZip(const QList<QPair<QString, QByteArray>>& members)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return QByteArray();
    bool added = true;
    for (const auto& m : members)
        added = added && mz_zip_writer_add_mem(&zip, m.first.toUtf8().constData(), m.second.constData(),
                                               static_cast<size_t>(m.second.size()), MZ_BEST_SPEED);
    void* p = nullptr;
    size_t n = 0;
    QByteArray out;
    if (mz_zip_writer_finalize_heap_archive(&zip, &p, &n) && added)   // a member that would not add = no zip
        out = QByteArray(static_cast<const char*>(p), int(n));
    mz_zip_writer_end(&zip);
    if (p) mz_free(p);
    return out;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray sha(const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256); }

const CustomCore* byId(const QString& id)
{
    for (const CustomCore& c : CustomCores::all())
        if (c.id == id) return &c;
    return nullptr;
}

BuildbotIndex::Entry entryNamed(const QList<BuildbotIndex::Entry>& es, const QString& name)
{
    for (const BuildbotIndex::Entry& e : es)
        if (e.name == name) return e;
    return BuildbotIndex::Entry();
}

// install() through the injected policy, synchronously from the probe's point of view.
struct InstallResult { bool done = false; bool ok = false; CustomCore rec; QString error; };
InstallResult runInstall(const BuildbotInstall::FetchPolicy& pol, const BuildbotIndex::Entry& e, QObject* ctx)
{
    auto r = std::make_shared<InstallResult>();
    BuildbotInstall::install(pol, e, ctx, {}, [r](bool ok, const CustomCore& rec, const QString& err) {
        r->done = true; r->ok = ok; r->rec = rec; r->error = err;
    });
    pumpUntil([r] { return r->done; }, 15000);
    return *r;
}

struct FetchResult { bool done = false; QByteArray bytes; QString error; };
FetchResult runFetch(const BuildbotInstall::FetchPolicy& pol, const QUrl& url, qint64 cap, QObject* ctx)
{
    auto r = std::make_shared<FetchResult>();
    BuildbotInstall::fetch(pol, url, cap, ctx, {}, [r](const QByteArray& b, const QString& err) {
        r->done = true; r->bytes = b; r->error = err;
    });
    pumpUntil([r] { return r->done; }, 15000);
    return *r;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qunsetenv("EB_UITEST");
    qunsetenv("EB_UITEST_BUILDBOT_BASE");
    qunsetenv("EB_FIXTURE_CORE_MODE");   // the fixture core is its default "content" self: "EB Fixture Content"

    const QDir here(QCoreApplication::applicationDirPath());
    auto fixturePath = [&here](const char* base) {
        QStringList suffixes{ CustomCoreInstall::librarySuffix() };
        suffixes << QStringLiteral(".dll") << QStringLiteral(".so") << QStringLiteral(".dylib");
        for (const QString& sfx : suffixes)
        {
            const QString p = here.absoluteFilePath(QString::fromLatin1(base) + sfx);
            if (QFileInfo::exists(p)) return p;
        }
        return QString();
    };
    const QString fixture  = fixturePath("probe_customcore_fixture");
    const QString notACore = fixturePath("probe_customcore_notacore");
    if (fixture.isEmpty() || notACore.isEmpty())
    {
        std::fprintf(stderr, "BUILDBOTINDEX-FAIL fixture cores not built beside the probe\n");
        return 1;
    }
    const QByteArray coreBytes  = readAll(fixture);
    const QByteArray notBytes   = readAll(notACore);
    CHECK(!coreBytes.isEmpty() && !notBytes.isEmpty());

    QFile::remove(CustomCores::registryPath());
    CustomCores::reload();

    // ---- 1. Parsing ---------------------------------------------------------------------------------------
    {
        const QByteArray body =
            "2024-03-01 6bf0c4c8 2048_libretro.dll.zip\n"
            "2023-12-25 0A0B0C0D fceumm_libretro.dll.zip\r\n"
            "2024-01-10 deadbeef mrboom_libretro.dll.zip 1234567\n"
            "\n"
            "   \r\n"
            "garbage\n"                                                   // malformed: 1 field
            "2024-13-45 00000000 baddate_libretro.dll.zip\n"              // malformed: no such date
            "2024-01-01 00000000\n"                                       // malformed: 2 fields
            "2024-01-01 00000000 sized_libretro.dll.zip lots\n"           // malformed: size not a number
            "2024-01-01 00000000 sized_libretro.dll.zip -5\n"             // malformed: negative size
            "2024-01-01 00000000 a b c d\n"                               // malformed: 6 fields
            "01/02/2024 00000000 slashdate_libretro.dll.zip\n"            // malformed: not yyyy-MM-dd
            "2024-01-01 00000000 ../evil_libretro.dll.zip\n"              // refused: traversal
            "2024-01-01 00000000 sub/dir_libretro.dll.zip\n"              // refused: path segment
            "2024-01-01 00000000 ..\\evil_libretro.dll.zip\n"             // refused: backslash segment
            "2024-01-01 00000000 C:evil_libretro.dll.zip\n"               // refused: drive
            "2024-01-01 00000000 custom:x_libretro.dll.zip\n"             // refused: a custom ref
            "2024-01-01 00000000 .hidden_libretro.dll.zip\n"              // refused: leading dot
            "2024-01-01 00000000 _libretro.dll.zip\n"                     // refused: empty name
            "2024-01-01 00000000 mgba_libretro.so.zip\n"                  // refused: another platform's file
            "2024-01-01 00000000 mgba_libretro.dll\n"                     // refused: not a zip
            "2024-01-01 00000000 a.b_libretro.dll.zip\n"                  // refused: dot in the name
            "2024-02-02 - vitaquake2-rogue_libretro.dll.zip\n";           // accepted, crc "-"
        const BuildbotIndex::Parsed p = BuildbotIndex::parseWithTail(body, QStringLiteral("_libretro.dll"));
        CHECK(!p.tooLarge);
        CHECK(p.entries.size() == 4);
        CHECK(p.malformed == 7);
        CHECK(p.refused == 10);
        CHECK(p.dropped == 0);
        if (p.entries.size() == 4)
        {
            CHECK(p.entries[0].name == QStringLiteral("2048"));
            CHECK(p.entries[0].file == QStringLiteral("2048_libretro.dll.zip"));
            CHECK(p.entries[0].coreFile == QStringLiteral("2048_libretro.dll"));
            CHECK(p.entries[0].date == QDate(2024, 3, 1));
            CHECK(p.entries[0].crc == QStringLiteral("6bf0c4c8"));
            CHECK(p.entries[0].size == -1);
            CHECK(p.entries[1].name == QStringLiteral("fceumm"));
            CHECK(p.entries[1].crc == QStringLiteral("0a0b0c0d"));        // lowercased
            CHECK(p.entries[1].date == QDate(2023, 12, 25));
            CHECK(p.entries[2].name == QStringLiteral("mrboom"));
            CHECK(p.entries[2].size == 1234567);                          // the optional fourth field
            CHECK(p.entries[3].name == QStringLiteral("vitaquake2-rogue"));
            CHECK(p.entries[3].crc.isEmpty());
            CHECK(p.entries[3].date == QDate(2024, 2, 2));
        }

        // Empty / all-garbage bodies are empty models, not failures.
        CHECK(BuildbotIndex::parseWithTail(QByteArray(), QStringLiteral("_libretro.dll")).entries.isEmpty());
        const BuildbotIndex::Parsed junk = BuildbotIndex::parseWithTail("\x01\x02\xff\n<html>", QStringLiteral("_libretro.dll"));
        CHECK(junk.entries.isEmpty() && junk.malformed == 2 && !junk.tooLarge);

        // This platform's own naming through parse(): the platform tail is accepted, another platform's is not.
        const QByteArray mine = QByteArray("2024-04-04 11111111 tic80") + kTail + ".zip\n"
                                "2024-04-04 11111111 tic80_libretro_other.zip\n";
        const BuildbotIndex::Parsed pm = BuildbotIndex::parse(mine);
        CHECK(pm.entries.size() == 1 && pm.refused == 1);
        if (pm.entries.size() == 1)
            CHECK(pm.entries[0].coreFile == QStringLiteral("tic80") + tail());
    }

    // ---- 1b. Caps -----------------------------------------------------------------------------------------
    {
        // Body cap: exactly 4 MiB is read; one byte more is refused whole.
        QByteArray atCap("2024-03-01 6bf0c4c8 2048_libretro.dll.zip\n");
        atCap.append(QByteArray(4194304 - atCap.size(), ' '));
        CHECK(atCap.size() == 4194304);
        const BuildbotIndex::Parsed a = BuildbotIndex::parseWithTail(atCap, QStringLiteral("_libretro.dll"));
        CHECK(!a.tooLarge && a.entries.size() == 1);
        QByteArray overCap = atCap;
        overCap.append(' ');
        const BuildbotIndex::Parsed o = BuildbotIndex::parseWithTail(overCap, QStringLiteral("_libretro.dll"));
        CHECK(o.tooLarge && o.entries.isEmpty());

        // Entry cap: 2,005 well-formed lines keep 2,000 and count 5 dropped.
        QByteArray many;
        for (int i = 0; i < 2005; ++i)
            many += "2024-03-01 00000000 core" + QByteArray::number(i) + "_libretro.dll.zip\n";
        const BuildbotIndex::Parsed m = BuildbotIndex::parseWithTail(many, QStringLiteral("_libretro.dll"));
        CHECK(m.entries.size() == 2000);
        CHECK(m.dropped == 5);
        CHECK(!m.entries.isEmpty() && m.entries.last().name == QStringLiteral("core1999"));
    }

    // ---- 2. The name rule ---------------------------------------------------------------------------------
    {
        CHECK(BuildbotIndex::isValidCoreName(QStringLiteral("2048")));
        CHECK(BuildbotIndex::isValidCoreName(QStringLiteral("mame2003_plus")));
        CHECK(BuildbotIndex::isValidCoreName(QStringLiteral("vitaquake2-rogue")));
        CHECK(BuildbotIndex::isValidCoreName(QStringLiteral("bsnes_hd+")));
        CHECK(BuildbotIndex::isValidCoreName(QString(64, QLatin1Char('a'))));
        CHECK(!BuildbotIndex::isValidCoreName(QString(65, QLatin1Char('a'))));
        CHECK(!BuildbotIndex::isValidCoreName(QString()));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("..")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("../x")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("a/b")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("a\\b")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("C:x")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("custom:x")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("a.b")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("a b")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("-x")));
        CHECK(!BuildbotIndex::isValidCoreName(QStringLiteral("_x")));
        CHECK(!BuildbotIndex::isValidCoreName(QString::fromUtf8("caf\xc3\xa9")));
        CHECK(BuildbotIndex::coreNameFromZip(QStringLiteral("2048_libretro.dll.zip"), QStringLiteral("_libretro.dll"))
              == QStringLiteral("2048"));
        CHECK(BuildbotIndex::coreNameFromZip(QStringLiteral("../2048_libretro.dll.zip"), QStringLiteral("_libretro.dll")).isEmpty());
        CHECK(BuildbotIndex::coreNameFromZip(QStringLiteral("2048_libretro.DLL.zip"), QStringLiteral("_libretro.dll")).isEmpty());
    }

    // ---- 3. Classification and search ---------------------------------------------------------------------
    const QString t = QStringLiteral("_libretro.dll");
    {
        const BuildbotIndex::Parsed p = BuildbotIndex::parseWithTail(
            "2024-03-01 1 2048_libretro.dll.zip\n"
            "2024-03-01 2 fceumm_libretro.dll.zip\n"
            "2024-01-10 3 mrboom_libretro.dll.zip\n"
            "2024-02-02 4 vitaquake2-rogue_libretro.dll.zip\n"
            "2024-02-02 5 dualcase_libretro.dll.zip\n", t);
        CHECK(p.entries.size() == 5);

        CustomCore fromBot;                       // installed here before, from the 1 January index
        fromBot.id = QStringLiteral("mr_boom"); fromBot.path = QStringLiteral("/x/cores/custom/mrboom_libretro.dll");
        fromBot.source = QStringLiteral("buildbot"); fromBot.sourceFile = QStringLiteral("mrboom_libretro.dll.zip");
        fromBot.sourceDate = QStringLiteral("2024-01-01");
        CustomCore byHand;                        // the same file as the 2048 entry, loaded by hand
        byHand.id = QStringLiteral("2048"); byHand.path = QStringLiteral("/x/cores/custom/2048_libretro.dll");
        CustomCore catalogueByHand;               // a catalogue core loaded by hand as well: catalogue still wins
        catalogueByHand.id = QStringLiteral("fceumm_fork"); catalogueByHand.path = QStringLiteral("/x/fceumm_libretro.dll");

        const QSet<QString> catalogue{ QStringLiteral("fceumm"), QStringLiteral("snes9x") };
        const QList<BuildbotIndex::Row> rows = BuildbotIndex::classify(
            p.entries, catalogue, QList<CustomCore>{ fromBot, byHand, catalogueByHand });
        CHECK(rows.size() == 5);
        if (rows.size() == 5)
        {
            CHECK(rows[0].status == BuildbotIndex::Status::InstalledCustom);
            CHECK(rows[0].customId == QStringLiteral("2048"));
            CHECK(!rows[0].fromBuildbot && !rows[0].updateAvailable);
            CHECK(rows[1].status == BuildbotIndex::Status::Catalogue);
            CHECK(rows[2].status == BuildbotIndex::Status::InstalledCustom);
            CHECK(rows[2].customId == QStringLiteral("mr_boom"));
            CHECK(rows[2].fromBuildbot && rows[2].updateAvailable);        // 2024-01-10 > 2024-01-01
            CHECK(rows[3].status == BuildbotIndex::Status::Available);
            CHECK(rows[4].status == BuildbotIndex::Status::Available);
        }

        // Search: catalogue rows are never listed; the query is a trimmed, case-insensitive substring.
        const QList<BuildbotIndex::Row> all = BuildbotIndex::browsable(rows, QString());
        CHECK(all.size() == 4);
        for (const BuildbotIndex::Row& r : all) CHECK(r.entry.name != QStringLiteral("fceumm"));
        const QList<BuildbotIndex::Row> mr = BuildbotIndex::browsable(rows, QStringLiteral("MR"));
        CHECK(mr.size() == 1 && mr[0].entry.name == QStringLiteral("mrboom"));
        const QList<BuildbotIndex::Row> rogue = BuildbotIndex::browsable(rows, QStringLiteral("  rogue "));
        CHECK(rogue.size() == 1 && rogue[0].entry.name == QStringLiteral("vitaquake2-rogue"));
        CHECK(BuildbotIndex::browsable(rows, QStringLiteral("fceumm")).isEmpty());
        CHECK(BuildbotIndex::browsable(rows, QStringLiteral("zzz")).isEmpty());

        // The real catalogue: the NES default is in it, so the browser never offers it.
        CHECK(BuildbotIndex::catalogueCoreNames().contains(QStringLiteral("fceumm")));
        CHECK(!BuildbotIndex::catalogueCoreNames().contains(QStringLiteral("2048")));
    }

    // ---- 3b. Update detection -----------------------------------------------------------------------------
    {
        BuildbotIndex::Entry e;
        e.name = QStringLiteral("mrboom"); e.file = QStringLiteral("mrboom_libretro.dll.zip");
        e.coreFile = QStringLiteral("mrboom_libretro.dll"); e.date = QDate(2024, 1, 10);
        CustomCore c;
        c.id = QStringLiteral("mr_boom"); c.path = QStringLiteral("/x/mrboom_libretro.dll");
        c.source = QStringLiteral("buildbot"); c.sourceFile = e.file;

        c.sourceDate = QStringLiteral("2024-01-09"); CHECK(BuildbotIndex::updateAvailable(c, e));    // newer index
        c.sourceDate = QStringLiteral("2024-01-10"); CHECK(!BuildbotIndex::updateAvailable(c, e));   // same day
        c.sourceDate = QStringLiteral("2024-01-11"); CHECK(!BuildbotIndex::updateAvailable(c, e));   // index older
        c.sourceDate = QStringLiteral("garbage");    CHECK(!BuildbotIndex::updateAvailable(c, e));
        c.sourceDate = QStringLiteral("2020-01-01");
        CustomCore hand = c; hand.source.clear(); hand.sourceFile.clear(); hand.sourceDate.clear();
        CHECK(!BuildbotIndex::updateAvailable(hand, e));                                            // hand-loaded
        CustomCore handWithStaleFields = c; handWithStaleFields.source.clear();
        CHECK(!BuildbotIndex::updateAvailable(handWithStaleFields, e));                             // no source
        CustomCore otherFile = c; otherFile.sourceFile = QStringLiteral("mrboom2_libretro.dll.zip");
        CHECK(!BuildbotIndex::updateAvailable(otherFile, e));
    }

    // ---- 4. The host rule (pure), the override gate, and the custom-ref guard ---------------------------
    {
        auto ok = [](const char* u) { return BuildbotIndex::isAllowedUrl(QUrl(QString::fromLatin1(u))); };
        CHECK(ok("https://buildbot.libretro.com/nightly/windows/x86_64/latest/2048_libretro.dll.zip"));
        CHECK(ok("https://buildbot.libretro.com:443/nightly/"));
        CHECK(!ok("http://buildbot.libretro.com/nightly/"));                  // scheme
        CHECK(!ok("ftp://buildbot.libretro.com/nightly/"));
        CHECK(!ok("file:///C:/cores/x.zip"));
        CHECK(!ok("https://buildbot.libretro.com.evil.example/nightly/"));    // suffix games
        CHECK(!ok("https://evilbuildbot.libretro.com/nightly/"));             // prefix games
        CHECK(!ok("https://libretro.com/nightly/"));
        CHECK(!ok("https://buildbot.libretro.com:8443/nightly/"));            // port
        CHECK(!ok("https://someone:secret@buildbot.libretro.com/nightly/"));  // user info
        CHECK(!ok("https://127.0.0.1/nightly/"));
        CHECK(!ok("http://127.0.0.1:8080/nightly/"));
        CHECK(!ok("not a url at all"));

        const BuildbotInstall::FetchPolicy prod = BuildbotInstall::productionPolicy();
        CHECK(prod.base == QUrl(QStringLiteral("https://buildbot.libretro.com/nightly/")));
        CHECK(prod.allow && !prod.allow(QUrl(QStringLiteral("http://127.0.0.1:8080/nightly/x"))));
        CHECK(BuildbotIndex::indexUrl(prod.base).toString()
              == QStringLiteral("https://buildbot.libretro.com/nightly/") + QString::fromLatin1(kSubpath)
                     + QStringLiteral(".index-extended"));

        // The override: only with EB_UITEST, only to loopback.
        const QString stubLike = QStringLiteral("http://127.0.0.1:8080/nightly");
        CHECK(BuildbotInstall::policyFor(false, stubLike).base == prod.base);                        // no EB_UITEST
        CHECK(BuildbotInstall::policyFor(true, QString()).base == prod.base);                        // no override
        CHECK(BuildbotInstall::policyFor(true, QStringLiteral("http://example.com/nightly/")).base == prod.base);
        CHECK(BuildbotInstall::policyFor(true, QStringLiteral("http://10.0.0.5/nightly/")).base == prod.base);
        CHECK(BuildbotInstall::policyFor(true, QStringLiteral("ftp://127.0.0.1/nightly/")).base == prod.base);
        const BuildbotInstall::FetchPolicy ov = BuildbotInstall::policyFor(true, stubLike);
        CHECK(ov.base == QUrl(QStringLiteral("http://127.0.0.1:8080/nightly/")));
        CHECK(ov.allow(QUrl(QStringLiteral("http://127.0.0.1:8080/nightly/x.zip"))));
        CHECK(!ov.allow(QUrl(QStringLiteral("http://127.0.0.1:8081/nightly/x.zip"))));             // other port
        CHECK(!ov.allow(QUrl(QStringLiteral("https://buildbot.libretro.com/nightly/x.zip"))));     // not even prod
        CHECK(!ov.allow(QUrl(QStringLiteral("http://localhost:8080/nightly/x.zip"))));             // other host
        // effectivePolicy reads the real environment: without EB_UITEST the override is ignored.
        qputenv("EB_UITEST_BUILDBOT_BASE", stubLike.toUtf8());
        CHECK(BuildbotInstall::effectivePolicy().base == prod.base);
        qputenv("EB_UITEST", "1");
        CHECK(BuildbotInstall::effectivePolicy().base == ov.base);
        qunsetenv("EB_UITEST");
        qunsetenv("EB_UITEST_BUILDBOT_BASE");

        // CoreManager's catalogue URL: unchanged for a catalogue core, and still NOTHING for a custom ref.
        CHECK(BuildbotIndex::catalogueZipUrl(QStringLiteral("fceumm"))
              == QStringLiteral("https://buildbot.libretro.com/nightly/") + QString::fromLatin1(kSubpath)
                     + QStringLiteral("fceumm") + tail() + QStringLiteral(".zip"));
        CHECK(BuildbotIndex::catalogueZipUrl(QStringLiteral("custom:eb_fixture_content")).isEmpty());
        CHECK(BuildbotIndex::catalogueZipUrl(QString()).isEmpty());
        CHECK(!CustomCores::mayDownload(QStringLiteral("custom:eb_fixture_content")));
    }

    // ---- 5. The byte path against a loopback stub (policy injected) ----------------------------------------
    Stub stub;
    CHECK(stub.start());
    QObject ctx;
    const BuildbotInstall::FetchPolicy pol = BuildbotInstall::policyFor(true, stub.base());
    CHECK(pol.base.toString() == stub.base());

    const QString coreFile = QStringLiteral("ebfixture") + tail();
    const QString zipName  = coreFile + QStringLiteral(".zip");
    const QString dir = stub.dirPath();
    auto setIndex = [&](const char* date) {
        stub.routes.insert(dir + QStringLiteral(".index-extended"),
                           Reply{ 200, QByteArray(date) + " cafef00d " + zipName.toLatin1() + "\n"
                                     + "2024-05-01 00000000 fceumm" + kTail + ".zip\n"
                                     + "2024-05-01 00000000 ../escape" + kTail + ".zip\n"
                                     + "2024-05-01 00000000 notacore" + kTail + ".zip\n", QByteArray() });
    };
    // The good zip: the core under a TRAVERSAL path (so the written name must be ours), plus decoys.
    const QByteArray goodZip = makeZip({ { QStringLiteral("../../evil.txt"), QByteArray("escaped") },
                                         { QStringLiteral("other") + tail(), QByteArray("not me") },
                                         { QStringLiteral("../../x/") + coreFile, coreBytes } });
    CHECK(!goodZip.isEmpty());

    {
        // The production policy against the stub: refused before any byte leaves, with the reason.
        const int before = stub.requested.size();
        const FetchResult r = runFetch(BuildbotInstall::productionPolicy(),
                                       QUrl(stub.base() + QStringLiteral("anything")), 1024, &ctx);
        CHECK(r.done && !r.error.isEmpty() && r.bytes.isEmpty());
        CHECK(r.error.contains(QStringLiteral("buildbot.libretro.com")));
        CHECK(stub.requested.size() == before);                             // ZERO requests reached the stub

        // A redirect to anywhere the predicate refuses is not followed.
        stub.routes.insert(QStringLiteral("/nightly/hop"),
                           Reply{ 302, QByteArray(), "Location: http://localhost:"
                                  + QByteArray::number(stub.srv.serverPort()) + "/nightly/landed\r\n" });
        stub.routes.insert(QStringLiteral("/nightly/landed"), Reply{ 200, QByteArray("landed"), QByteArray() });
        const FetchResult hop = runFetch(pol, QUrl(stub.base() + QStringLiteral("hop")), 1024, &ctx);
        CHECK(hop.done && !hop.error.isEmpty() && hop.bytes.isEmpty());
        CHECK(!stub.requested.contains(QStringLiteral("/nightly/landed")));
        // ...and one the predicate admits is followed.
        stub.routes.insert(QStringLiteral("/nightly/hop2"),
                           Reply{ 302, QByteArray(), "Location: /nightly/landed\r\n" });
        const FetchResult hop2 = runFetch(pol, QUrl(stub.base() + QStringLiteral("hop2")), 1024, &ctx);
        CHECK(hop2.done && hop2.error.isEmpty() && hop2.bytes == QByteArray("landed"));

        // The byte cap aborts rather than truncating.
        stub.routes.insert(QStringLiteral("/nightly/big"), Reply{ 200, QByteArray(64 * 1024, 'x'), QByteArray() });
        const FetchResult big = runFetch(pol, QUrl(stub.base() + QStringLiteral("big")), 1024, &ctx);
        CHECK(big.done && !big.error.isEmpty() && big.bytes.isEmpty());

        // The index through the real fetchIndex: parsed with this platform's naming, the escape refused.
        setIndex("2024-05-01");
        bool done = false; BuildbotIndex::Parsed parsed; QString err;
        BuildbotInstall::fetchIndex(pol, &ctx, [&](const BuildbotIndex::Parsed& p, const QString& e) {
            done = true; parsed = p; err = e; });
        pumpUntil([&] { return done; }, 15000);
        CHECK(done && err.isEmpty());
        CHECK(parsed.entries.size() == 3 && parsed.refused == 1);
        CHECK(entryNamed(parsed.entries, QStringLiteral("ebfixture")).date == QDate(2024, 5, 1));

        // An over-cap index body is refused (the transfer is aborted at 4 MiB).
        stub.routes.insert(dir + QStringLiteral(".index-extended"),
                           Reply{ 200, QByteArray(4194304 + 1, '\n'), QByteArray() });
        done = false; err.clear();
        BuildbotInstall::fetchIndex(pol, &ctx, [&](const BuildbotIndex::Parsed& p, const QString& e) {
            done = true; parsed = p; err = e; });
        pumpUntil([&] { return done; }, 15000);
        CHECK(done && !err.isEmpty() && parsed.entries.isEmpty());
        setIndex("2024-05-01");
    }

    const QString dest = CustomCores::customDir() + QStringLiteral("/") + coreFile;
    BuildbotIndex::Entry ebf;
    {
        const BuildbotIndex::Parsed p = BuildbotIndex::parse(
            QByteArray("2024-05-01 cafef00d ") + zipName.toLatin1() + "\n");
        CHECK(p.entries.size() == 1);
        ebf = p.entries.value(0);
    }

    // ---- 6. Install + inspection ---------------------------------------------------------------------------
    {
        // A brand-new entry whose "core" is not a core: refused, deleted, nothing registered.
        const QString notFile = QStringLiteral("notacore") + tail();
        stub.routes.insert(dir + notFile + QStringLiteral(".zip"),
                           Reply{ 200, makeZip({ { notFile, notBytes } }), QByteArray() });
        const BuildbotIndex::Entry notEntry = BuildbotIndex::parse(
            QByteArray("2024-05-01 0 ") + notFile.toLatin1() + ".zip\n").entries.value(0);
        const InstallResult bad = runInstall(pol, notEntry, &ctx);
        CHECK(bad.done && !bad.ok);
        CHECK(bad.error.contains(QStringLiteral("deleted")));
        CHECK(!QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/") + notFile));
        CHECK(!QFileInfo::exists(BuildbotInstall::stagingDir() + QStringLiteral("/") + notFile));
        CHECK(CustomCores::all().isEmpty());

        // The real install.
        stub.routes.insert(dir + zipName, Reply{ 200, goodZip, QByteArray() });
        const InstallResult r = runInstall(pol, ebf, &ctx);
        CHECK(r.done && r.ok);
        if (!r.ok) std::fprintf(stderr, "BUILDBOTINDEX: install said: %s\n", qUtf8Printable(r.error));
        CHECK(r.rec.id == QStringLiteral("eb_fixture_content"));
        CHECK(r.rec.name == QStringLiteral("EB Fixture Content"));
        CHECK(r.rec.version == QStringLiteral("0.1"));
        CHECK(r.rec.extensions == (QStringList{ QStringLiteral("ebf"), QStringLiteral("ebfixture"), QStringLiteral("nes") }));
        CHECK(r.rec.source == QStringLiteral("buildbot"));
        CHECK(r.rec.sourceFile == zipName);
        CHECK(r.rec.sourceDate == QStringLiteral("2024-05-01"));
        CHECK(QFileInfo(r.rec.path).absoluteFilePath() == QFileInfo(dest).absoluteFilePath());
        CHECK(readAll(dest) == coreBytes);                                   // the core, byte for byte
        const CustomCore* reg = byId(QStringLiteral("eb_fixture_content"));
        CHECK(reg && reg->sourceDate == QStringLiteral("2024-05-01"));
        // Only the expected file was written: no decoy, anywhere the traversal pointed.
        CHECK(!QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/other") + tail()));
        CHECK(!QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/evil.txt")));
        CHECK(!QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/../evil.txt")));
        CHECK(!QFileInfo::exists(CustomCores::customDir() + QStringLiteral("/../../evil.txt")));
        CHECK(QDir(BuildbotInstall::stagingDir()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
        // It is a candidate like any custom core: the registry resolves its ref to the installed file.
        CHECK(CustomCores::pathForRef(QStringLiteral("custom:eb_fixture_content")) == r.rec.path);

        // Classified as installed, from the buildbot, with no update against the same index date.
        const QList<BuildbotIndex::Row> rows = BuildbotIndex::classify(
            QList<BuildbotIndex::Entry>{ ebf }, QSet<QString>(), CustomCores::all());
        CHECK(rows.size() == 1 && rows[0].status == BuildbotIndex::Status::InstalledCustom
              && rows[0].fromBuildbot && !rows[0].updateAvailable);
    }

    // ---- 7. Updates ---------------------------------------------------------------------------------------
    {
        BuildbotIndex::Entry newer = ebf;
        newer.date = QDate(2024, 6, 1);
        const QList<BuildbotIndex::Row> rows = BuildbotIndex::classify(
            QList<BuildbotIndex::Entry>{ newer }, QSet<QString>(), CustomCores::all());
        CHECK(rows.size() == 1 && rows[0].updateAvailable);                  // the index moved on
        BuildbotIndex::Entry older = ebf;
        older.date = QDate(2024, 4, 1);
        CHECK(!BuildbotIndex::classify(QList<BuildbotIndex::Entry>{ older }, QSet<QString>(),
                                       CustomCores::all()).value(0).updateAvailable);

        const QByteArray oldHash = sha(readAll(dest));
        const CustomCore oldRec = byId(QStringLiteral("eb_fixture_content")) ? *byId(QStringLiteral("eb_fixture_content"))
                                                                             : CustomCore();
        auto oldKept = [&]() {
            CustomCores::reload();
            const CustomCore* now = byId(QStringLiteral("eb_fixture_content"));
            return sha(readAll(dest)) == oldHash && now && *now == oldRec
                && QDir(BuildbotInstall::stagingDir()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty();
        };

        // (a) the new "core" is not a core: refused, deleted, old kept.
        stub.routes.insert(dir + zipName, Reply{ 200, makeZip({ { coreFile, notBytes } }), QByteArray() });
        InstallResult f = runInstall(pol, newer, &ctx);
        CHECK(f.done && !f.ok && f.error.contains(QStringLiteral("deleted")) && f.error.contains(QStringLiteral("kept")));
        CHECK(oldKept());
        // (b) not a zip at all.
        stub.routes.insert(dir + zipName, Reply{ 200, QByteArray("<html>maintenance</html>"), QByteArray() });
        f = runInstall(pol, newer, &ctx);
        CHECK(f.done && !f.ok && f.error.contains(QStringLiteral("kept")));
        CHECK(oldKept());
        // (c) a zip without the core file in it.
        stub.routes.insert(dir + zipName, Reply{ 200, makeZip({ { QStringLiteral("readme.txt"), QByteArray("hi") } }),
                                                QByteArray() });
        f = runInstall(pol, newer, &ctx);
        CHECK(f.done && !f.ok && f.error.contains(QStringLiteral("kept")));
        CHECK(oldKept());
        // (d) the server fails.
        stub.routes.insert(dir + zipName, Reply{ 500, QByteArray("oops"), QByteArray() });
        f = runInstall(pol, newer, &ctx);
        CHECK(f.done && !f.ok && f.error.contains(QStringLiteral("kept")));
        CHECK(oldKept());

        // A good update: new bytes in place (trailing bytes after the image still load), new date recorded.
        const QByteArray v2 = coreBytes + QByteArray("EB-FIXTURE-V2");
        stub.routes.insert(dir + zipName, Reply{ 200, makeZip({ { coreFile, v2 } }), QByteArray() });
        const InstallResult up = runInstall(pol, newer, &ctx);
        CHECK(up.done && up.ok);
        if (!up.ok) std::fprintf(stderr, "BUILDBOTINDEX: update said: %s\n", qUtf8Printable(up.error));
        CHECK(readAll(dest) == v2);
        CustomCores::reload();
        const CustomCore* after = byId(QStringLiteral("eb_fixture_content"));
        CHECK(after && after->sourceDate == QStringLiteral("2024-06-01") && after->source == QStringLiteral("buildbot"));
        CHECK(CustomCores::all().size() == 1);                               // updated, not duplicated
        CHECK(!BuildbotIndex::classify(QList<BuildbotIndex::Entry>{ newer }, QSet<QString>(),
                                       CustomCores::all()).value(0).updateAvailable);

        // A hand-loaded core never shows an update. Load the same file by hand: the record loses its source,
        // and even an index far in the future offers nothing.
        const QString handDir = QDir::tempPath() + QStringLiteral("/eb-buildbotindex-hand-")
                                + QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(handDir);
        const QString handFile = handDir + QStringLiteral("/") + coreFile;
        QFile::remove(handFile);
        CHECK(QFile::copy(fixture, handFile));
        CustomCore handRec;
        QString handErr;
        CHECK(CustomCoreInstall::loadFromFile(handFile, &handRec, &handErr));
        CHECK(handRec.source.isEmpty() && handRec.sourceFile.isEmpty() && handRec.sourceDate.isEmpty());
        BuildbotIndex::Entry future = ebf;
        future.date = QDate(2099, 1, 1);
        const QList<BuildbotIndex::Row> hr = BuildbotIndex::classify(
            QList<BuildbotIndex::Entry>{ future }, QSet<QString>(), CustomCores::all());
        CHECK(hr.size() == 1 && hr[0].status == BuildbotIndex::Status::InstalledCustom);
        CHECK(hr.size() == 1 && !hr[0].fromBuildbot && !hr[0].updateAvailable);
        QFile::remove(handFile);
        QDir().rmdir(handDir);
    }

    if (failures == 0) std::printf("BUILDBOTINDEX-OK\n");
    else std::fprintf(stderr, "BUILDBOTINDEX: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
