// Headless check of OFFLINE DOWNLOADS FROM A JELLYFIN SERVER (issue #110, increment 1) — the
// original-quality path.
//
// A SEPARATE PROBE FROM probe_jellyfin, deliberately. That one is #160's: the qualified id, the migration,
// the server store and the union's failure isolation, and it is already a thousand lines about those. This
// one links things that one refuses to link — the REAL DownloadManager, over Qt6::Network — because the
// central assertion here cannot be made without it:
//
//     THE ACCESS TOKEN MUST NOT REACH ANY FILE THIS FEATURE WRITES.
//
// A Jellyfin download url carries the user's token in its query, and a DownloadJob is persisted. So §3 below
// drives the real manager, the real server store and the real settings store against a fixture token, and
// then reads back EVERY BYTE OF EVERY FILE the process wrote — not a hand-built JSON beside them, which
// would prove only that this probe can keep a token out of a file it wrote itself.
//
// NO NETWORK, AND NO REAL SERVER. Every url here is built from a fixture root and compared as a string;
// nothing is sent. The socket half (JellyfinClient) is not linked at all.
// (Section 9, #437, is the one exception and stays inside this process: it runs real transfers against a
// loopback server it binds itself on 127.0.0.1, because what it asserts is what a transfer leaves behind.)
//
// NO CREDENTIAL IS EVER PRINTED. The fixture token is compared and searched for, never written to stdout or
// stderr — including inside a failing CHECK, which is why every credential section asserts on a boolean
// computed beforehand rather than on an expression naming the token. The same rule probe_jellyfin states.
//
// EVERY STORE IS REDIRECTED. AppPaths::dataDir() is already this process's own scratch directory
// (EB_ISOLATED_DATA_DIR), and the three Jellyfin stores are additionally pointed at one ini INSIDE it
// through their EB_JELLYFIN_TEST_SEAM setters — so the byte scan has a single, named file to read and a
// production build cannot reach these setters at all.
//
// Prints JFDOWNLOAD-OK on success; any failure prints JFDOWNLOAD-FAIL <cond> and exits non-zero.
#include "AppPaths.h"
#include "DownloadManager.h"
#include "DownloadRecipe.h"   // #437: an add-on download's re-mint recipe, as a job's sourceRef
#include "DownloadsStore.h"
#include "Jellyfin.h"
#include "JellyfinDownload.h"
#include "JellyfinServerStore.h"
#include "OfflineProgress.h"
#include "UrlAtRest.h"        // #437: a resumable link as it may sit in queue.json

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <cstdio>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "JFDOWNLOAD-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Two real-shaped server ids and ONE item id shared by both. Section 1 is entirely about the fact that the
// same raw item id on two servers is two different downloads.
static const char* kSrvA  = "0123456789abcdef0123456789abcdef";
static const char* kSrvB  = "fedcba9876543210fedcba9876543210";
static const char* kItem  = "aaaaaaaabbbbccccddddeeeeeeeeeeee";
static const char* kItem2 = "bbbbbbbbccccddddeeeeffffffffffff";
static const char* kRootA = "https://a.example.test";
static const char* kRootB = "https://b.example.test:8920";

// THE FIXTURE TOKEN. Distinctive enough that a substring search is meaningful and structured like the real
// thing (a Jellyfin access token is a 32-character hex string). It is never printed.
static const char* kToken = "5f3c9ad14e7b48c2a06d81be29f5c7a3";

static QString qual(const char* srv, const char* item)
{
    return Jellyfin::qualify(QString::fromLatin1(srv), QString::fromLatin1(item));
}

static Jellyfin::UnionItem episode(const char* srv, const char* item, const QString& series,
                                   int season, int number, const QString& name, bool played)
{
    Jellyfin::UnionItem e;
    e.id                = qual(srv, item);
    e.title             = name;
    e.type              = QStringLiteral("Episode");
    e.seriesName        = series;
    e.parentIndexNumber = season;
    e.indexNumber       = number;
    e.played            = played;
    return e;
}

// ---------------------------------------------------------------------------------------------------------
// 1. THE URL, AND ITS SERVER QUALIFICATION
// ---------------------------------------------------------------------------------------------------------
static void sectionUrl()
{
    // The path is asserted with no token anywhere near it, which is why downloadPath is a function at all.
    CHECK(JellyfinDownload::downloadPath(QString::fromLatin1(kItem))
          == QStringLiteral("/Items/") + QString::fromLatin1(kItem) + QStringLiteral("/Download"));
    CHECK(JellyfinDownload::downloadPath(QString()).isEmpty());

    const QString token = QString::fromLatin1(kToken);
    const QString uA = JellyfinDownload::downloadUrl(QString::fromLatin1(kRootA),
                                                     QString::fromLatin1(kItem), token);
    const QString uB = JellyfinDownload::downloadUrl(QString::fromLatin1(kRootB),
                                                     QString::fromLatin1(kItem), token);

    // TWO SERVERS, ONE ITEM ID, TWO URLS — and each addressed at ITS OWN host. This is the property that
    // stops a download of server A's episode being fetched from server B (with server B's token, which
    // would also be an attempt to use one account's credential against another's box).
    CHECK(uA != uB);
    CHECK(uA.startsWith(QString::fromLatin1(kRootA) + QStringLiteral("/Items/")));
    CHECK(uB.startsWith(QString::fromLatin1(kRootB) + QStringLiteral("/Items/")));
    CHECK(uA.contains(QStringLiteral("/Download")));
    CHECK(uB.contains(QStringLiteral("/Download")));
    // The token is IN the url — that is what makes it a credential and the reason nothing stores one. The
    // assertion is on a boolean so the token never reaches a failure message.
    const bool tokenInUrl = uA.contains(token) && uB.contains(token);
    CHECK(tokenInUrl);
    CHECK(uA.contains(QStringLiteral("api_key=")));

    // Refusals: nothing half-built is ever handed back for a caller to notice.
    CHECK(JellyfinDownload::downloadUrl(QString(), QString::fromLatin1(kItem), token).isEmpty());
    CHECK(JellyfinDownload::downloadUrl(QString::fromLatin1(kRootA), QString(), token).isEmpty());
    // A tokenless url is still a url: an anonymous/public server is not this function's business to refuse.
    CHECK(!JellyfinDownload::downloadUrl(QString::fromLatin1(kRootA), QString::fromLatin1(kItem),
                                         QString()).isEmpty());
}

// ---------------------------------------------------------------------------------------------------------
// 2. JOB IDENTITY AND FILE NAMES
// ---------------------------------------------------------------------------------------------------------
static void sectionIdentity()
{
    const QString kA = JellyfinDownload::jobKey(qual(kSrvA, kItem));
    const QString kB = JellyfinDownload::jobKey(qual(kSrvB, kItem));
    CHECK(!kA.isEmpty());
    CHECK(kA != kB);                                         // server-qualified, so no cross-server de-dup
    CHECK(JellyfinDownload::refFromJobKey(kA) == qual(kSrvA, kItem));
    CHECK(JellyfinDownload::isJobKey(kA));
    // Not ours: an ordinary addon key must not be claimed, or every non-Jellyfin download would be minted
    // through a server that has never heard of it.
    CHECK(!JellyfinDownload::isJobKey(QStringLiteral("tt0111161")));
    CHECK(JellyfinDownload::refFromJobKey(QStringLiteral("jfdl:not-qualified")).isEmpty());
    CHECK(JellyfinDownload::jobKey(QStringLiteral("tt0111161")).isEmpty());

    // A film.
    Jellyfin::UnionItem film;
    film.id = qual(kSrvA, kItem);
    film.title = QStringLiteral("The Man: Who? / Knew *Too* Much");
    film.type = QStringLiteral("Movie");
    const QString fn = JellyfinDownload::fileNameFor(film, QStringLiteral("mp4"));
    CHECK(fn.endsWith(QStringLiteral(".mp4")));
    // Sanitised: none of these may survive into a path component.
    CHECK(!fn.contains(QLatin1Char('/')) && !fn.contains(QLatin1Char('\\')));
    CHECK(!fn.contains(QLatin1Char('*')) && !fn.contains(QLatin1Char('?')));
    CHECK(fn.contains(QStringLiteral("The Man")));

    // An episode: series, season and episode number in the name, because a folder of "Episode 4.mkv" is a
    // folder nobody can read.
    const Jellyfin::UnionItem ep = episode(kSrvA, kItem, QStringLiteral("Some Show"), 1, 2,
                                           QStringLiteral("The Second One"), false);
    const QString en = JellyfinDownload::fileNameFor(ep, QString());
    CHECK(en.startsWith(QStringLiteral("Some Show S01E02")));
    CHECK(en.endsWith(QStringLiteral(".mkv")));              // no container given -> the honest default

    // THE SAME EPISODE ON TWO SERVERS IS TWO FILES. Without the id suffix they would collide on one
    // destination and DownloadManager would de-dup them into a single job — one server's episode silently
    // standing in for the other's.
    const Jellyfin::UnionItem epB = episode(kSrvB, kItem, QStringLiteral("Some Show"), 1, 2,
                                            QStringLiteral("The Second One"), false);
    CHECK(JellyfinDownload::fileNameFor(ep, QString()) != JellyfinDownload::fileNameFor(epB, QString()));

    // A hostile container string cannot become an extension, a path, or a second dot-segment.
    const QString weird = JellyfinDownload::fileNameFor(film, QStringLiteral("../../etc"));
    CHECK(weird.endsWith(QStringLiteral(".mkv")));
}

// ---------------------------------------------------------------------------------------------------------
// 3. THE CREDENTIAL BYTE SCAN — the assertion this probe exists for
// ---------------------------------------------------------------------------------------------------------
static void sectionCredential(const QString& iniPath)
{
    const QString token = QString::fromLatin1(kToken);
    const QString ref   = qual(kSrvA, kItem);

    // A REAL SIGNED-IN SERVER, through the real store. This is the one place the token is allowed to live.
    JellyfinServer s;
    s.id = QString::fromLatin1(kSrvA);
    s.name = QStringLiteral("Fixture");
    s.url = QString::fromLatin1(kRootA);
    s.userId = QStringLiteral("11112222333344445555666677778888");
    s.userName = QStringLiteral("someone");
    s.token = token;
    s.enabled = true;
    JellyfinServerStore::add(s);

    // ...and the minted url, exactly as JellyfinClient::downloadUrlFor would build it. Held in a local and
    // never assigned to the job, which IS the design.
    JellyfinServer back;
    CHECK(JellyfinServerStore::get(QString::fromLatin1(kSrvA), back));
    const QString minted = JellyfinDownload::downloadUrl(
        Jellyfin::normalizeRoot(back.url, back.allowPlainHttp), QString::fromLatin1(kItem), back.token);
    CHECK(!minted.isEmpty());

    // THE REAL MANAGER, and the real queue.json under this process's scratch data dir.
    {
        DownloadManager dm;
        // The minter, exactly as MainWindow installs it — a job's ref in, a fresh url out. Asserted here
        // because a minter that answered "" would make every Jellyfin download fail with a sentence about
        // a signed-out server, which reads as the feature being broken rather than as a wiring bug.
        dm.setUrlMinter([](const QString& r) {
            const Jellyfin::Ref pr = Jellyfin::parse(r);
            JellyfinServer srv;
            if (!pr.ok || !JellyfinServerStore::get(pr.serverId, srv)) return QString();
            return JellyfinDownload::downloadUrl(Jellyfin::normalizeRoot(srv.url, srv.allowPlainHttp),
                                                 pr.itemId, srv.token);
        });

        DownloadJob j;
        j.title = QStringLiteral("Fixture Film");
        j.sourceRef = ref;                     // ...and NO url. That is the whole decision.
        j.dest = AppPaths::dataDir() + QStringLiteral("/downloads/Fixture Film.mkv");
        j.kind = QStringLiteral("video");
        j.key = ref;
        dm.enqueue(j);

        // ACCEPTED DESPITE THE EMPTY URL. Before #110 enqueue() dropped a job with no url on the floor, so
        // this is the assertion that the ref-backed shape is a shape the manager knows about at all.
        CHECK(dm.jobs().size() == 1);
        if (!dm.jobs().isEmpty())
        {
            CHECK(dm.jobs().first().sourceRef == ref);
            // The live object holds NO url either. Not "a scrubbed one" — none: there is no member for a
            // save(), a log or a crash dump to find one in.
            const bool liveJobHoldsNoToken = !dm.jobs().first().url.contains(token);
            CHECK(liveJobHoldsNoToken);
        }
        // A job whose server is unknown to this device is refused BY THE MINTER, not by a url it kept.
        CHECK(dm.jobs().isEmpty() || !dm.jobs().first().url.contains(QStringLiteral("api_key")));

        // THE BELT BESIDE THE BRACES, made load-bearing. Every site in the app today builds a ref-backed
        // job with no url, so save()'s "a ref means no url is written" line is inert against them and a
        // mutation of it survives every other assertion in this file. This is the caller that mutation
        // would be a bug for: one that set BOTH — a plausible future site, or a careless copy of the
        // ordinary enqueueDownload beside it. The link must not reach the file even so.
        DownloadJob both;
        both.title = QStringLiteral("Careless Caller");
        both.sourceRef = ref;
        both.url = minted;                  // ...which carries the token
        both.dest = AppPaths::dataDir() + QStringLiteral("/downloads/Careless Caller.mkv");
        both.kind = QStringLiteral("video");
        both.key = ref + QStringLiteral("#2");
        dm.enqueue(both);
        CHECK(dm.jobs().size() == 2);
    }
    // ...and a fresh manager reads the same job back off disk, ref intact. The restart case: unlike a
    // header-gated job, this one still knows what to ask for.
    {
        DownloadManager dm2;
        CHECK(dm2.jobs().size() == 2);
        if (!dm2.jobs().isEmpty()) CHECK(dm2.jobs().first().sourceRef == ref);
        // ...and NEITHER restored job has a url, including the one whose caller supplied one.
        for (const DownloadJob& rj : dm2.jobs())
        {
            const bool restoredJobHoldsNoToken = !rj.url.contains(token);
            CHECK(restoredJobHoldsNoToken);
            CHECK(rj.url.isEmpty());
        }
    }

    // The other two stores this feature writes.
    JellyfinDownload::setCapGb(25);
    JellyfinDownload::setRemoveAfterWatched(true);
    OfflineProgress::Report r;
    r.qualifiedId     = ref;
    r.positionSeconds = 120.0;
    r.playSessionId   = QStringLiteral("session-1");
    r.mediaSourceId   = QStringLiteral("source-1");
    r.whenMs          = 1000;
    OfflineProgress::enqueue(r);

    // ---- THE SCAN ----------------------------------------------------------------------------------
    // Every file this process wrote, read as bytes. A directory walk rather than a list of paths, because
    // the failure this guards against is a file NOBODY REMEMBERED — the whole point of #200 was a store
    // that had not been thought about.
    const QByteArray needle = token.toUtf8();
    QStringList offenders;
    QDirIterator it(AppPaths::dataDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (f.readAll().contains(needle)) offenders << QFileInfo(path).fileName();
    }
    // The ini is the ONE legitimate home: JellyfinServerStore keeps the token under "jellyfin/…/servers",
    // which CloudSync::isDeviceLocalKey carves out of everything this app syncs. Anything else is a leak.
    const QString iniName = QFileInfo(iniPath).fileName();
    offenders.removeAll(iniName);
    // Named, not printed: the assertion says WHICH FILE without saying what was found in it.
    if (!offenders.isEmpty())
        std::fprintf(stderr, "JFDOWNLOAD: the token reached %s\n", offenders.join(QLatin1Char(' ')).toUtf8().constData());
    CHECK(offenders.isEmpty());

    // ...and inside that one file, only the server-store key. The download queue, the two settings and the
    // offline-progress buffer are each read back and checked on their own, so a future key that DID carry a
    // token is named rather than hidden behind "the ini is allowed to have it".
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.sync();
    QStringList badKeys;
    const QStringList keys = ini.allKeys();
    for (const QString& k : keys)
        if (ini.value(k).toString().contains(token) && !k.startsWith(QStringLiteral("jellyfin/"))
            && !k.endsWith(QStringLiteral("/servers")))
            badKeys << k;
    if (!badKeys.isEmpty())
        std::fprintf(stderr, "JFDOWNLOAD: the token reached key(s) %s\n",
                     badKeys.join(QLatin1Char(' ')).toUtf8().constData());
    CHECK(badKeys.isEmpty());

    // The queue file itself, by name and by bytes: no token, and no api_key parameter either — a url with
    // the token stripped would still be a url that had been there.
    QFile qf(AppPaths::dataDir() + QStringLiteral("/downloads/queue.json"));
    CHECK(qf.exists());
    if (qf.open(QIODevice::ReadOnly))
    {
        const QByteArray body = qf.readAll();
        const bool queueHoldsToken = body.contains(needle);
        CHECK(!queueHoldsToken);
        CHECK(!body.contains("api_key"));
        CHECK(!body.contains("http"));          // no url of any kind reached this file
        CHECK(body.contains(ref.toUtf8()));     // ...but the REF did: the durable half is there
    }

    // And the progress queue's stored rows: an id, a position and two session ids. Never a link.
    const QString qKey = OfflineProgress::queueKey(QString::fromLatin1(kSrvA));
    const QString stored = ini.value(qKey).toString();
    CHECK(!stored.isEmpty());
    const bool progressHoldsToken = stored.contains(token);
    CHECK(!progressHoldsToken);
    CHECK(!stored.contains(QStringLiteral("http")));
}

// ---------------------------------------------------------------------------------------------------------
// 4. THE BATCH VERBS
// ---------------------------------------------------------------------------------------------------------
static void sectionBatch()
{
    // A season, deliberately handed over OUT OF ORDER and with one already watched.
    QVector<Jellyfin::UnionItem> eps;
    eps << episode(kSrvA, "e3", QStringLiteral("Show"), 1, 3, QStringLiteral("Three"), false)
        << episode(kSrvA, "e1", QStringLiteral("Show"), 1, 1, QStringLiteral("One"),   true)
        << episode(kSrvA, "e4", QStringLiteral("Show"), 1, 4, QStringLiteral("Four"),  false)
        << episode(kSrvA, "e2", QStringLiteral("Show"), 1, 2, QStringLiteral("Two"),   false);

    const QVector<Jellyfin::UnionItem> all = JellyfinDownload::seasonBatch(eps, {});
    CHECK(all.size() == 4);
    // ORDERED BY (season, episode), NOT BY THE ORDER THE SERVER RETURNED. A batch that queues episode 3
    // first downloads the wrong thing first on a connection that will not finish.
    if (all.size() == 4)
    {
        CHECK(all.at(0).indexNumber == 1);
        CHECK(all.at(1).indexNumber == 2);
        CHECK(all.at(2).indexNumber == 3);
        CHECK(all.at(3).indexNumber == 4);
    }

    // ALREADY DOWNLOADED -> SKIPPED. Not re-queued: six gigabytes over a hotel connection for a file that
    // is already on the disk is the worst thing this verb could do.
    QSet<QString> have;
    have.insert(qual(kSrvA, "e2"));
    const QVector<Jellyfin::UnionItem> missing = JellyfinDownload::seasonBatch(eps, have);
    CHECK(missing.size() == 3);
    for (const Jellyfin::UnionItem& e : missing) CHECK(e.id != qual(kSrvA, "e2"));

    // NEXT N UNWATCHED: skips the watched one (e1) and the held one (e2), in order.
    const QVector<Jellyfin::UnionItem> next2 = JellyfinDownload::nextUnwatched(eps, 2, have);
    CHECK(next2.size() == 2);
    if (next2.size() == 2)
    {
        CHECK(next2.at(0).id == qual(kSrvA, "e3"));
        CHECK(next2.at(1).id == qual(kSrvA, "e4"));
    }
    // More asked for than exist: what there is, not an error and not a wrap-around.
    CHECK(JellyfinDownload::nextUnwatched(eps, 99, have).size() == 2);
    // n <= 0 means NOTHING. "All of them" is a different verb, and a caller that arrived here with 0 has a
    // bug this must not paper over by downloading a whole series.
    CHECK(JellyfinDownload::nextUnwatched(eps, 0, {}).isEmpty());
    CHECK(JellyfinDownload::nextUnwatched(eps, -1, {}).isEmpty());

    // A row with an UNQUALIFIABLE id contributes to neither verb: there is no server to mint a url from,
    // so queueing it would produce a job that can only ever fail.
    QVector<Jellyfin::UnionItem> bad = eps;
    Jellyfin::UnionItem orphan;
    orphan.id = QStringLiteral("not-a-jellyfin-id");
    orphan.type = QStringLiteral("Episode");
    orphan.indexNumber = 5;
    orphan.parentIndexNumber = 1;
    bad << orphan;
    CHECK(JellyfinDownload::seasonBatch(bad, {}).size() == 4);
    CHECK(JellyfinDownload::nextUnwatched(bad, 99, {}).size() == 3);

    // TWO SEASONS: episode 1 of season 2 comes AFTER episode 4 of season 1, which is what "next" means.
    QVector<Jellyfin::UnionItem> two;
    two << episode(kSrvA, "s2e1", QStringLiteral("Show"), 2, 1, QStringLiteral("Next year"), false)
        << episode(kSrvA, "s1e9", QStringLiteral("Show"), 1, 9, QStringLiteral("Finale"),    false);
    const QVector<Jellyfin::UnionItem> ordered = JellyfinDownload::seasonBatch(two, {});
    CHECK(ordered.size() == 2);
    if (ordered.size() == 2) CHECK(ordered.at(0).parentIndexNumber == 1);
}

// ---------------------------------------------------------------------------------------------------------
// 5. THE STORE-AND-FORWARD PROGRESS QUEUE
// ---------------------------------------------------------------------------------------------------------
static OfflineProgress::Report rep(const QString& id, double pos, qint64 when)
{
    OfflineProgress::Report r;
    r.qualifiedId = id;
    r.positionSeconds = pos;
    r.whenMs = when;
    return r;
}

static void sectionQueue()
{
    const QString a = qual(kSrvA, kItem);
    const QString b = qual(kSrvA, kItem2);

    // ---- The bound ----
    QVector<OfflineProgress::Report> q;
    for (int i = 0; i < OfflineProgress::kMaxPerServer + 25; ++i)
        q = OfflineProgress::boundedAppend(q, rep(a, double(i), qint64(i)), OfflineProgress::kMaxPerServer);
    CHECK(q.size() == OfflineProgress::kMaxPerServer);
    // OLDEST OUT, newest kept: the bound sheds history, never the latest position — which is the only row
    // the server actually wants.
    CHECK(q.first().whenMs == 25);
    CHECK(q.last().whenMs == OfflineProgress::kMaxPerServer + 24);
    // "No bound" is not a state this queue may be in.
    CHECK(OfflineProgress::boundedAppend(q, rep(a, 1.0, 1), 0).isEmpty());

    // ---- Order and idempotence ----
    QVector<OfflineProgress::Report> mixed;
    mixed << rep(a, 10.0, 300) << rep(b, 5.0, 100) << rep(a, 60.0, 500) << rep(a, 30.0, 400);
    const QVector<OfflineProgress::Report> flat = OfflineProgress::collapse(mixed);
    // ONE ROW PER ITEM — ten reports for one episode are ten statements of the same fact.
    CHECK(flat.size() == 2);
    // ...ascending by when, so the flush replays in the order the viewing happened.
    if (flat.size() == 2)
    {
        CHECK(flat.at(0).qualifiedId == b);
        CHECK(flat.at(1).qualifiedId == a);
        // ...and the row kept for `a` is the NEWEST, not the first seen and not the largest position.
        CHECK(flat.at(1).whenMs == 500);
        CHECK(flat.at(1).positionSeconds == 60.0);
    }
    // A REWIND IS STILL THE NEWEST TRUTH. The user seeking back to 5 s and stopping there must not be
    // collapsed away in favour of the 60 s row that came before it.
    QVector<OfflineProgress::Report> rewound;
    rewound << rep(a, 60.0, 500) << rep(a, 5.0, 900);
    const QVector<OfflineProgress::Report> rf = OfflineProgress::collapse(rewound);
    CHECK(rf.size() == 1);
    if (rf.size() == 1) CHECK(rf.first().positionSeconds == 5.0);

    // ---- The stale-report rule ----
    const OfflineProgress::Report queued = rep(a, 1200.0, 1000);   // twenty minutes, from the plane

    Jellyfin::UserState unknown;                                    // the server never heard of it
    unknown.ok = false;
    CHECK(OfflineProgress::shouldApply(queued, unknown));

    Jellyfin::UserState behind;                                     // the server is EARLIER: apply
    behind.ok = true;
    behind.positionTicks = Jellyfin::ticksFromSeconds(60.0);
    CHECK(OfflineProgress::shouldApply(queued, behind));

    Jellyfin::UserState ahead;                                      // THE CASE. The television got further.
    ahead.ok = true;
    ahead.positionTicks = Jellyfin::ticksFromSeconds(2400.0);
    CHECK(!OfflineProgress::shouldApply(queued, ahead));

    Jellyfin::UserState finished;                                   // ...and finishing it counts double
    finished.ok = true;
    finished.played = true;
    finished.positionTicks = 0;
    CHECK(!OfflineProgress::shouldApply(queued, finished));

    // THE SLACK. Two measurements of the same viewing, a report interval apart, are not a stale report —
    // without this, a report queued a moment before the connection returned would be judged stale against
    // the position it had itself just caused.
    Jellyfin::UserState justAhead;
    justAhead.ok = true;
    justAhead.positionTicks = Jellyfin::ticksFromSeconds(1200.0 + OfflineProgress::kStaleSlackSeconds - 0.5);
    CHECK(OfflineProgress::shouldApply(queued, justAhead));
    Jellyfin::UserState wellAhead;
    wellAhead.ok = true;
    wellAhead.positionTicks = Jellyfin::ticksFromSeconds(1200.0 + OfflineProgress::kStaleSlackSeconds + 0.5);
    CHECK(!OfflineProgress::shouldApply(queued, wellAhead));

    // An empty id is never applied and never queued: there is nobody to tell.
    CHECK(!OfflineProgress::shouldApply(rep(QString(), 10.0, 1), unknown));

    // ---- Per server, and surviving a restart ----
    OfflineProgress::clearServer(QString::fromLatin1(kSrvA));
    OfflineProgress::clearServer(QString::fromLatin1(kSrvB));
    OfflineProgress::enqueue(rep(qual(kSrvA, kItem),  10.0, 100));
    OfflineProgress::enqueue(rep(qual(kSrvB, kItem),  20.0, 200));
    OfflineProgress::enqueue(rep(qual(kSrvA, kItem2), 30.0, 300));
    CHECK(OfflineProgress::pending(QString::fromLatin1(kSrvA)).size() == 2);
    CHECK(OfflineProgress::pending(QString::fromLatin1(kSrvB)).size() == 1);
    const QStringList owed = OfflineProgress::serversWithPending();
    CHECK(owed.contains(QString::fromLatin1(kSrvA)));
    CHECK(owed.contains(QString::fromLatin1(kSrvB)));

    // THE RESTART. Read back through the store's own reader, byte for byte — the position, the ids and the
    // timestamp all have to survive, or a flush after a reboot would send a different report from the one
    // the viewing produced.
    const QVector<OfflineProgress::Report> backA = OfflineProgress::pending(QString::fromLatin1(kSrvA));
    CHECK(backA.size() == 2);
    if (backA.size() == 2)
    {
        CHECK(backA.at(0).qualifiedId == qual(kSrvA, kItem));
        CHECK(backA.at(0).positionSeconds == 10.0);
        CHECK(backA.at(0).whenMs == 100);
        CHECK(backA.at(1).qualifiedId == qual(kSrvA, kItem2));
    }
    // Clearing one server leaves the other's alone — the reason the queue is keyed by server at all.
    OfflineProgress::clearServer(QString::fromLatin1(kSrvA));
    CHECK(OfflineProgress::pending(QString::fromLatin1(kSrvA)).isEmpty());
    CHECK(OfflineProgress::pending(QString::fromLatin1(kSrvB)).size() == 1);
    // An unqualifiable id is not owed to anybody and must not invent a server to owe it to.
    OfflineProgress::enqueue(rep(QStringLiteral("not-a-jellyfin-id"), 1.0, 1));
    CHECK(!OfflineProgress::serversWithPending().contains(QStringLiteral("not-a-jellyfin-id")));
}

// ---------------------------------------------------------------------------------------------------------
// 6. THE CAP: A SUGGESTION THAT DELETES NOTHING
// ---------------------------------------------------------------------------------------------------------
static void sectionCap()
{
    const qint64 gb = JellyfinDownload::kBytesPerGb;
    QVector<JellyfinDownload::StoredItem> items;
    // Four items, 4 GB each = 16 GB. Their "last useful" moments are deliberately not their sizes and not
    // their download order, so a suggestion that sorted by either would pick the wrong victims.
    items.push_back({ qual(kSrvA, "old"),    QStringLiteral("/x/old.mkv"),    4 * gb, 100, 50,  false });
    items.push_back({ qual(kSrvA, "newest"), QStringLiteral("/x/newest.mkv"), 4 * gb, 900, 50,  false });
    items.push_back({ qual(kSrvA, "middle"), QStringLiteral("/x/middle.mkv"), 4 * gb, 500, 50,  true  });
    items.push_back({ qual(kSrvA, "never"),  QStringLiteral("/x/never.mkv"),  4 * gb, 0,   300, false });

    // Under the cap: nothing to say. A cap that nagged while there was room would be turned off.
    const JellyfinDownload::CapVerdict under = JellyfinDownload::evictionSuggestion(items, 20 * gb);
    CHECK(!under.over);
    CHECK(under.victims.isEmpty());
    CHECK(under.usedBytes == 16 * gb);

    // No cap set: never over, whatever is on the disk.
    CHECK(!JellyfinDownload::evictionSuggestion(items, 0).over);
    CHECK(JellyfinDownload::evictionSuggestion(items, 0).victims.isEmpty());
    CHECK(JellyfinDownload::evictionSuggestion(items, -1).victims.isEmpty());

    // Over: 16 GB against a 10 GB cap. Two items have to go, LEAST RECENTLY USEFUL FIRST — "old" (played at
    // 100) and then "never" (never played, downloaded at 300). "middle" and "newest" stay.
    const JellyfinDownload::CapVerdict over = JellyfinDownload::evictionSuggestion(items, 10 * gb);
    CHECK(over.over);
    CHECK(over.capBytes == 10 * gb);
    CHECK(over.victims.size() == 2);
    if (over.victims.size() == 2)
    {
        CHECK(over.victims.at(0) == qual(kSrvA, "old"));
        CHECK(over.victims.at(1) == qual(kSrvA, "never"));
    }
    CHECK(over.freedBytes == 8 * gb);
    // STOPS AS SOON AS IT IS ENOUGH: a suggestion that listed everything old would be a suggestion to empty
    // the downloads folder.
    CHECK(over.usedBytes - over.freedBytes <= over.capBytes);

    // A never-played item falls back to its DOWNLOAD time, not to "oldest possible" — otherwise everything
    // you have not watched yet is always the first thing suggested for removal, which is exactly backwards.
    QVector<JellyfinDownload::StoredItem> two;
    two.push_back({ qual(kSrvA, "watchedLongAgo"), QStringLiteral("/x/a"), 8 * gb, 10,  5,   false });
    two.push_back({ qual(kSrvA, "freshUnplayed"),  QStringLiteral("/x/b"), 8 * gb, 0,   999, false });
    const JellyfinDownload::CapVerdict pick = JellyfinDownload::evictionSuggestion(two, 8 * gb);
    CHECK(pick.victims.size() == 1);
    if (pick.victims.size() == 1) CHECK(pick.victims.first() == qual(kSrvA, "watchedLongAgo"));

    // Stable across two runs: a list that reshuffles between two openings of the same screen reads as a bug.
    CHECK(JellyfinDownload::evictionSuggestion(items, 10 * gb).victims
          == JellyfinDownload::evictionSuggestion(items, 10 * gb).victims);

    // NOTHING WAS DELETED. The verdict is a list of ids and byte counts; it names no writer, and the four
    // items are all still here to be asked about again.
    CHECK(JellyfinDownload::evictionSuggestion(items, 10 * gb).usedBytes == 16 * gb);

    // "Remove after watched" is the same shape: it lists, it does not act.
    const QStringList watched = JellyfinDownload::watchedCandidates(items);
    CHECK(watched.size() == 1);
    if (watched.size() == 1) CHECK(watched.first() == qual(kSrvA, "middle"));
}

// ---------------------------------------------------------------------------------------------------------
// 7. THE SETTINGS KEYS THIS FEATURE ADDS
// ---------------------------------------------------------------------------------------------------------
// The DEVICE-LOCAL CLASSIFICATION of these keys is asserted where every other carve-out is —
// probe_cloudmerge, which is the only target that links CloudSync. Here we pin the SHAPES that classification
// depends on, so a rename that quietly moved a key out of the carved prefix fails in both places.
static void sectionSettingsKeys()
{
    CHECK(JellyfinDownload::capKey().startsWith(QStringLiteral("downloads/")));
    CHECK(JellyfinDownload::removeWatchedKey().startsWith(QStringLiteral("downloads/")));
    CHECK(OfflineProgress::queueKey(QString::fromLatin1(kSrvA)).startsWith(QStringLiteral("jellyfin/")));
    CHECK(OfflineProgress::queueKey(QString::fromLatin1(kSrvA))
          != OfflineProgress::queueKey(QString::fromLatin1(kSrvB)));   // per server, not one flat buffer

    // Defaults: no cap, and nothing removed by omission.
    JellyfinDownload::setCapGb(0);
    JellyfinDownload::setRemoveAfterWatched(false);
    CHECK(JellyfinDownload::capGb() == 0);
    CHECK(!JellyfinDownload::removeAfterWatched());
    // A nonsense cap is not stored as a nonsense cap.
    JellyfinDownload::setCapGb(-5);
    CHECK(JellyfinDownload::capGb() == 0);
    JellyfinDownload::setCapGb(50);
    CHECK(JellyfinDownload::capGb() == 50);
}

// ---------------------------------------------------------------------------------------------------------
// 8. "REMOVE AFTER WATCHED" — THE ONLY THING IN THIS FEATURE THAT DELETES A USER'S FILE
// ---------------------------------------------------------------------------------------------------------
// The switch is visible in both settings builders, so it has to DO something; and what it does is delete
// somebody's content, so every branch of it is pinned here rather than described.
//
// Three separate questions, and they fail separately on purpose:
//
//   (a) WHEN is the offer made at all — and, far more importantly, when is it NOT. A mid-item exit must
//       never reach the card; nor must a streamed item; nor must a second stop after the user already said
//       Keep; nor must an item whose length was never measured.
//   (b) WHAT MAY BE DELETED. A real file is created OUTSIDE a real downloads folder and handed to the
//       remover, and the assertion is not merely that it returned a refusal — it is that the file is STILL
//       THERE afterwards.
//   (c) WHAT THE STORE DOES ON EACH BRANCH, driven against the REAL DownloadsStore: the row goes when the
//       file went, and stays, exactly as it was, when it did not.
static void sectionRemoveAfterWatched()
{
    // ---- (a) the trigger -------------------------------------------------------------------------------
    // The fraction itself is pinned, because moving it is the difference between "you finished this" and
    // "you were most of the way through this".
    CHECK(JellyfinDownload::kWatchedFraction > 0.85 && JellyfinDownload::kWatchedFraction <= 0.95);

    const double dur = 100.0;
    // THE SETTING OFF MEANS NO OFFER AT ALL — not a quieter one, not one on the next item.
    CHECK(!JellyfinDownload::shouldOfferRemoval(false, true, false, 100.0, dur));
    CHECK(!JellyfinDownload::shouldOfferRemoval(false, true, false,  95.0, dur));
    // On, finished, from a local file, never declined -> the one true case.
    CHECK(JellyfinDownload::shouldOfferRemoval(true, true, false, 100.0, dur));
    CHECK(JellyfinDownload::shouldOfferRemoval(true, true, false,  90.0, dur));   // exactly at the fraction
    CHECK(JellyfinDownload::shouldOfferRemoval(true, true, false, 140.0, dur));   // past the end (mpv does)
    // A MID-ITEM EXIT IS NOT A FINISH. This is the assertion the whole design is arranged around: somebody
    // who stops halfway must never be offered the deletion of what they are halfway through.
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false,  50.0, dur));
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false,  89.9, dur));
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false,   0.0, dur));
    // Streaming an item puts no copy of it on this disk, so there is nothing to offer to remove.
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, false, false, 100.0, dur));
    // DECLINING IS REMEMBERED: the same file is not put to the user twice.
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, true, 100.0, dur));
    // No measured length -> no fraction to be past. Guessing here is exactly a mid-item deletion offer.
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false, 100.0,  0.0));
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false, 100.0, -1.0));
    CHECK(!JellyfinDownload::shouldOfferRemoval(true, true, false,  -5.0, dur));

    // ---- (b) what may be deleted -----------------------------------------------------------------------
    // Two REAL folders and three REAL files, inside this process's own scratch dir. The refusal is asserted
    // by the file still being there, which is the only form of that assertion worth making.
    const QString base = AppPaths::dataDir() + QStringLiteral("/s8");
    QDir(base).removeRecursively();
    const QString dl   = base + QStringLiteral("/downloads");
    const QString away = base + QStringLiteral("/elsewhere");
    QDir().mkpath(dl);
    QDir().mkpath(away);
    auto put = [](const QString& p) {
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write("x", 1);
        f.close();
        return true;
    };
    const QString inside  = dl   + QStringLiteral("/Fixture Film [aaaaaaaa-bbbbbbbb].mkv");
    const QString outside = away + QStringLiteral("/Somebody's Own Library Copy.mkv");
    CHECK(put(inside));
    CHECK(put(outside));

    CHECK(JellyfinDownload::isInsideDownloads(inside, dl));
    CHECK(!JellyfinDownload::isInsideDownloads(outside, dl));
    // The folder ITSELF is not a file inside the folder — a row whose path had been emptied down to the
    // directory would otherwise hand the deletion the whole downloads folder.
    CHECK(!JellyfinDownload::isInsideDownloads(dl, dl));
    CHECK(!JellyfinDownload::isInsideDownloads(dl + QStringLiteral("/"), dl));
    // A path that SPELLS its way out of the folder is out of the folder, however it is spelled.
    CHECK(!JellyfinDownload::isInsideDownloads(dl + QStringLiteral("/../elsewhere/x.mkv"), dl));
    CHECK(!JellyfinDownload::isInsideDownloads(QString(), dl));
    CHECK(!JellyfinDownload::isInsideDownloads(inside, QString()));
    // "downloads2" is not "downloads": a prefix match without the separator would delete out of it.
    CHECK(!JellyfinDownload::isInsideDownloads(dl + QStringLiteral("2/x.mkv"), dl));
    // Native separators are the same path (this is how a Windows store row is actually spelled).
    CHECK(JellyfinDownload::isInsideDownloads(QDir::toNativeSeparators(inside),
                                              QDir::toNativeSeparators(dl)));

    // A SYMLINK THAT SITS INSIDE THE FOLDER AND POINTS OUT OF IT — the case cleaning the TEXT of a path
    // cannot see, and the reason isInsideDownloads resolves through canonicalFilePath at all.
    //
    // ASSERTED ONLY WHERE THIS PROCESS CAN ACTUALLY MAKE ONE. On Windows QFile::link writes a .lnk
    // shortcut, which is a document and not a link the filesystem follows, and a real symlink needs a
    // privilege CI does not have — so staging it there would assert the wrong thing rather than the same
    // thing weakly. On the Linux runner it is a real symlink and this is a real assertion.
#ifndef Q_OS_WIN
    const QString escape = dl + QStringLiteral("/escape.mkv");
    if (QFile::link(outside, escape))
    {
        CHECK(!JellyfinDownload::isInsideDownloads(escape, dl));
        CHECK(JellyfinDownload::removeDownloadedFile(escape, dl)
              == JellyfinDownload::RemovalOutcome::RefusedOutsideDownloads);
        CHECK(QFileInfo::exists(outside));   // the thing it pointed at is untouched
        QFile::remove(escape);
    }
#endif

    using RO = JellyfinDownload::RemovalOutcome;
    // THE REFUSAL, AND THE FILE SURVIVING IT.
    CHECK(JellyfinDownload::removeDownloadedFile(outside, dl) == RO::RefusedOutsideDownloads);
    CHECK(QFileInfo::exists(outside));
    CHECK(!JellyfinDownload::entryMayLeaveDownloads(RO::RefusedOutsideDownloads));
    // ...and it is refused whether or not anything is at the end of it: the answer is about what this
    // feature is allowed to touch, not about what happens to be there.
    CHECK(JellyfinDownload::removeDownloadedFile(away + QStringLiteral("/never-existed.mkv"), dl)
          == RO::RefusedOutsideDownloads);

    // A DELETE THE OS REFUSES. A directory stands in for it — QFile::remove declines one on every platform,
    // and it is the portable way to ask "what happens when the file does not go?".
    const QString notAFile = dl + QStringLiteral("/a-folder-not-a-file");
    QDir().mkpath(notAFile);
    CHECK(JellyfinDownload::removeDownloadedFile(notAFile, dl) == RO::DeleteFailed);
    CHECK(QFileInfo::exists(notAFile));
    CHECK(!JellyfinDownload::entryMayLeaveDownloads(RO::DeleteFailed));

    // ---- (c) the store, on every branch ----------------------------------------------------------------
    // The REAL DownloadsStore, over this process's own isolated data dir.
    const QString refIn   = qual(kSrvA, kItem);
    const QString refOut  = qual(kSrvA, kItem2);
    const QString refFail = qual(kSrvB, kItem);
    DownloadsStore::add({ inside,   QStringLiteral("Fixture Film"), QStringLiteral("video"),
                          QString(), refIn,   QString(), QString() });
    DownloadsStore::add({ outside,  QStringLiteral("Library Copy"), QStringLiteral("video"),
                          QString(), refOut,  QString(), QString() });
    DownloadsStore::add({ notAFile, QStringLiteral("Not A File"),   QStringLiteral("video"),
                          QString(), refFail, QString(), QString() });
    auto held = [](const QString& key) {
        for (const DownloadedItem& d : DownloadsStore::list()) if (d.key == key) return true;
        return false;
    };
    CHECK(held(refIn) && held(refOut) && held(refFail));

    // REFUSED: nothing removed, and the row is exactly where it was.
    CHECK(JellyfinDownload::removeDownloadedItem(refOut, outside, dl) == RO::RefusedOutsideDownloads);
    CHECK(QFileInfo::exists(outside));
    CHECK(held(refOut));
    // FAILED: the same. A Downloads folder that forgets an item it did not manage to remove lies about the
    // disk, and that is the state a user finds when they go looking for the space back.
    CHECK(JellyfinDownload::removeDownloadedItem(refFail, notAFile, dl) == RO::DeleteFailed);
    CHECK(QFileInfo::exists(notAFile));
    CHECK(held(refFail));
    // ACCEPTED: the file goes, and only then does the row.
    CHECK(JellyfinDownload::removeDownloadedItem(refIn, inside, dl) == RO::Removed);
    CHECK(!QFileInfo::exists(inside));
    CHECK(!held(refIn));
    // ...and the neighbours are untouched by it.
    CHECK(held(refOut) && held(refFail));
    // ALREADY GONE is still gone: asked a second time the file is absent, which is the state the row was
    // being removed for, so the row may go too (it already has).
    CHECK(JellyfinDownload::removeDownloadedFile(inside, dl) == RO::NotFound);
    CHECK(JellyfinDownload::entryMayLeaveDownloads(RO::NotFound));

    QDir(base).removeRecursively();
    DownloadsStore::remove(refOut);
    DownloadsStore::remove(refFail);
}

// ---------------------------------------------------------------------------------------------------------
// 9. A DOWNLOAD LINK AT REST (issue #437) — every other download's url, and where queue.json may go
// ---------------------------------------------------------------------------------------------------------
// §3 is about the Jellyfin family, which never had a url to keep. This is about everything else: a job whose
// url came from an add-on (a debrid link, a stream link with a key in its query) and used to be written into
// queue.json as it was. The rules, each asserted against the REAL manager over a REAL loopback transfer:
//
//   (a) a job whose source can mint the link again keeps a RECIPE and no url, and resumes through the minter
//       (DownloadRecipe.h + DownloadManager::setAsyncUrlMinter);
//   (b) a resumable plain job round-trips: on Windows its stored field is not the plaintext and unseals back
//       to it (DPAPI); elsewhere the file is its owner's alone (0600);
//   (c) an old plaintext "url" is read once and written back in today's shape;
//   (d) a job that fails, or is cancelled, loses its query and fragment at once;
//   (e) a re-minted link that names a different file starts the download over instead of splicing;
//   (f) a ref-backed job (Jellyfin/Subsonic/Audiobookshelf) is exactly what it was;
//   (g) nothing that carries files off the device carries queue.json.
//
// NO REAL SERVER: every url is 127.0.0.1 on a port this process bound. The fixture tokens are compared and
// searched for, never printed — the same rule as §3.

// Distinct per case, so a hit names the case. Fixtures, not credentials; still never printed.
static const char* kTokA = "t437aaa0c0ffee11";
static const char* kTokB = "t437bbb0c0ffee22";
static const char* kTokC = "t437ccc0c0ffee33";
static const char* kTokD = "t437ddd0c0ffee44";
static const char* kTokE = "t437eee0c0ffee55";
static const char* kTokF = "t437fff0c0ffee66";
static const char* kTokG = "t437ggg0c0ffee77";

// The bytes a fixture file holds: deterministic per name, and "other.bin" a DIFFERENT SIZE from the rest,
// which is what (e) needs.
static QByteArray fixtureBody(const QString& name)
{
    const int n = name == QStringLiteral("other.bin") ? 150000 : 200000;
    QByteArray b(n, '\0');
    const uint seed = qHash(name);
    for (int i = 0; i < n; ++i) b[i] = char((i * 31 + int(seed % 251)) & 0xff);
    return b;
}

// A loopback HTTP/1.1 file server, one response per connection.
//   GET /f/<name>     200 with the whole file, or 206 from the offset a "Range: bytes=N-" names. A name
//                     starting "hold-" answers its FIRST un-ranged request with half the body and then HOLDS
//                     the connection open — a transfer caught in the middle, for Pause and Cancel to find.
//   GET /gone/<name>  404.
// Every request is recorded as (path, query, ranged) so a case can ask what was actually fetched.
struct StubRequest { QString path; QString query; bool ranged = false; };
class FileStub
{
public:
    FileStub()
    {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket* s = server_.nextPendingConnection())
            {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    buf->append(s->readAll());
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray head = buf->left(end);
                    buf->clear();
                    answer(s, head);
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    quint16 port() const { return server_.serverPort(); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }
    const QVector<StubRequest>& requests() const { return requests_; }
    int count(const QString& path) const
    {
        int n = 0;
        for (const StubRequest& r : requests_) if (r.path == path) ++n;
        return n;
    }

private:
    void answer(QTcpSocket* s, const QByteArray& head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QUrl target(QString::fromLatin1(first.value(1)));
        StubRequest rq;
        rq.path = target.path();
        rq.query = target.query();
        qint64 from = -1;
        for (const QByteArray& l : lines)
        {
            const QByteArray t = l.trimmed();
            if (t.toLower().startsWith("range: bytes="))
            {
                const QByteArray spec = t.mid(13);
                from = spec.left(spec.indexOf('-')).toLongLong();
                rq.ranged = true;
            }
        }
        requests_.push_back(rq);

        if (rq.path.startsWith(QStringLiteral("/gone/")))
        {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QString name = rq.path.section(QLatin1Char('/'), -1);
        const QByteArray body = fixtureBody(name);
        if (from > 0 && from < body.size())
        {
            const QByteArray tail = body.mid(int(from));
            s->write("HTTP/1.1 206 Partial Content\r\nContent-Length: " + QByteArray::number(tail.size())
                     + "\r\nContent-Range: bytes " + QByteArray::number(from) + "-"
                     + QByteArray::number(body.size() - 1) + "/" + QByteArray::number(body.size())
                     + "\r\nConnection: close\r\n\r\n");
            s->write(tail);
            s->disconnectFromHost();
            return;
        }
        s->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
                 + "\r\nConnection: close\r\n\r\n");
        if (name.startsWith(QStringLiteral("hold-")) && !held_.contains(name))
        {
            held_.insert(name);
            s->write(body.left(body.size() / 2));    // ...and hold: the transfer is now in the middle
            return;
        }
        s->write(body);
        s->disconnectFromHost();
    }

    QTcpServer server_;
    QVector<StubRequest> requests_;
    QSet<QString> held_;
};

// Run the event loop until `cond` holds, or `ms` passes. Returns whether it held.
static bool spinUntil(const std::function<bool()>& cond, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!cond())
    {
        if (t.elapsed() > ms) return false;
        QEventLoop loop;                               // wait, delivering events, a few ms at a time
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

static const DownloadJob* jobByKey(const DownloadManager& dm, const QString& key)
{
    for (const DownloadJob& j : dm.jobs()) if (j.key == key) return &j;
    return nullptr;
}

static QString atRestQueuePath() { return AppPaths::dataDir() + QStringLiteral("/downloads/queue.json"); }

static QByteArray atRestQueueBytes()
{
    QFile f(atRestQueuePath());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QJsonObject atRestRecord(const QString& key)
{
    for (const QJsonValue& v : QJsonDocument::fromJson(atRestQueueBytes()).array())
        if (v.toObject().value(QStringLiteral("key")).toString() == key) return v.toObject();
    return QJsonObject();
}

static bool fileHolds(const QString& path, const QByteArray& want)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.readAll() == want;
}

// The owner-only rule, POSIX only (on Windows the protection is the seal, and Qt's permission bits there
// describe a read-only attribute, not who may read the file).
static bool ownerOnly(const QString& path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path);
    return true;
#else
    const QFileDevice::Permissions p = QFileInfo(path).permissions();
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                          | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    return (p & others) == 0 && (p & QFileDevice::ReadOwner) && (p & QFileDevice::WriteOwner);
#endif
}

static void sectionAtRest()
{
    // A clean queue: §3's Jellyfin jobs are not this section's, and a restored one would start on construction.
    QFile::remove(atRestQueuePath());
    const QString dir = AppPaths::dataDir() + QStringLiteral("/downloads/s9");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    FileStub stub;
    CHECK(stub.port() != 0);
    const QString base = stub.base();
    const qint64 half = fixtureBody(QStringLiteral("x")).size() / 2;

    // ---- (b) a plain resumable job round-trips ------------------------------------------------------------
    const QString tokA = QString::fromLatin1(kTokA);
    const QString urlA = base + QStringLiteral("/f/hold-plain.bin?token=") + tokA + QStringLiteral("#frag");
    {
        DownloadManager dm;
        DownloadJob j;
        j.title = QStringLiteral("Plain");
        j.url = urlA;
        j.dest = dir + QStringLiteral("/hold-plain.bin");
        j.kind = QStringLiteral("video");
        j.key = QStringLiteral("k-plain");
        dm.enqueue(j);
        CHECK(spinUntil([&] { const DownloadJob* p = jobByKey(dm, j.key); return p && p->received >= half; }));
        const DownloadJob* p = jobByKey(dm, j.key);
        if (p) dm.pauseJob(p->id);
        CHECK(p && p->state == DownloadJob::Paused);

        const QByteArray bytes = atRestQueueBytes();
        const QJsonObject rec = atRestRecord(j.key);
        const bool fileHoldsTokA = bytes.contains(tokA.toUtf8());
        if (UrlAtRest::sealsAtRest())
        {
            // WINDOWS: the stored field is not the plaintext, and it unseals back to exactly the link.
            CHECK(!fileHoldsTokA);
            CHECK(!bytes.contains("token="));
            CHECK(!bytes.contains("hold-plain.bin?"));
            CHECK(rec.value(QStringLiteral("url")).toString().isEmpty());
            const QString sealed = rec.value(QStringLiteral("urlp")).toString();
            CHECK(sealed.startsWith(QStringLiteral("dpapi1:")));
            const bool unsealsToTheLink = UrlAtRest::unseal(sealed) == urlA;
            CHECK(unsealsToTheLink);
            // …and a seal that is not ours does not open as one.
            CHECK(UrlAtRest::unseal(QStringLiteral("dpapi1:AAAA")).isEmpty());
            CHECK(UrlAtRest::unseal(QStringLiteral("dpapi9:") + sealed.mid(7)).isEmpty());
        }
        else
        {
            // ELSEWHERE: kept as it is (no secret store is linked), in a file only its owner can read.
            CHECK(fileHoldsTokA);
            CHECK(!rec.contains(QStringLiteral("urlp")));
        }
        CHECK(ownerOnly(atRestQueuePath()));
    }
    {
        // A RESTART, then Resume: the link comes back and the .part is continued, not restarted.
        DownloadManager dm;
        const DownloadJob* p = jobByKey(dm, QStringLiteral("k-plain"));
        CHECK(p && p->state == DownloadJob::Paused);
        const bool linkRestored = p && p->url == urlA;
        CHECK(linkRestored);
        if (p) dm.resumeJob(p->id);
        CHECK(spinUntil([&] { const DownloadJob* q = jobByKey(dm, QStringLiteral("k-plain"));
                              return q && q->state == DownloadJob::Done; }));
        CHECK(fileHolds(dir + QStringLiteral("/hold-plain.bin"), fixtureBody(QStringLiteral("hold-plain.bin"))));
        CHECK(stub.count(QStringLiteral("/f/hold-plain.bin")) == 2);
        const bool resumedRanged = !stub.requests().isEmpty() && stub.requests().last().ranged
                                && stub.requests().last().query == QStringLiteral("token=") + tokA;
        CHECK(resumedRanged);
    }

    // ---- (d) a failed job loses its query and fragment AT ONCE ---------------------------------------------
    const QString tokB = QString::fromLatin1(kTokB);
    {
        DownloadManager dm;
        DownloadJob j;
        j.title = QStringLiteral("Gone");
        j.url = base + QStringLiteral("/gone/fail.bin?token=") + tokB + QStringLiteral("#frag");
        j.dest = dir + QStringLiteral("/fail.bin");
        j.kind = QStringLiteral("video");
        j.key = QStringLiteral("k-fail");
        dm.enqueue(j);
        CHECK(spinUntil([&] { const DownloadJob* p = jobByKey(dm, j.key); return p && p->state == DownloadJob::Failed; }));
        const DownloadJob* p = jobByKey(dm, j.key);
        // In memory, not only on disk: the token must not outlive the transfer it was issued for.
        CHECK(p && p->url == base + QStringLiteral("/gone/fail.bin"));
        CHECK(p && p->linkDropped);
        CHECK(p && p->error.contains(QStringLiteral("start the download again from the item")));
        const QByteArray bytes = atRestQueueBytes();
        const bool fileHoldsTokB = bytes.contains(tokB.toUtf8());
        CHECK(!fileHoldsTokB);
        CHECK(!bytes.contains("token="));
        CHECK(atRestRecord(j.key).value(QStringLiteral("dropped")).toBool());
        // Retry cannot help a link without its query: it says so, and sends nothing.
        const int asked = stub.count(QStringLiteral("/gone/fail.bin"));
        if (p) dm.retry(p->id);
        p = jobByKey(dm, j.key);
        CHECK(p && p->state == DownloadJob::Failed);
        CHECK(stub.count(QStringLiteral("/gone/fail.bin")) == asked);
        // Starting it again from the item (a fresh enqueue of the same destination) is the way back.
        DownloadJob again = j;
        again.url = base + QStringLiteral("/gone/fail.bin?token=") + tokB;
        dm.enqueue(again);
        p = jobByKey(dm, j.key);
        CHECK(p && !p->linkDropped);
        CHECK(spinUntil([&] { const DownloadJob* q = jobByKey(dm, j.key); return q && q->state == DownloadJob::Failed; }));
        CHECK(stub.count(QStringLiteral("/gone/fail.bin")) == asked + 1);
    }
    {
        // …and a restart reads the dropped job back as dropped, with its sentence (the error text itself is
        // not persisted; the flag is, and the sentence comes back with it).
        DownloadManager dm;
        const DownloadJob* p = jobByKey(dm, QStringLiteral("k-fail"));
        CHECK(p && p->state == DownloadJob::Failed && p->linkDropped);
        CHECK(p && p->url == base + QStringLiteral("/gone/fail.bin"));
        CHECK(p && p->error.contains(QStringLiteral("start the download again from the item")));
        if (p) dm.removeJob(p->id);
    }

    // ---- (d) a cancelled job's link goes with it, at once -------------------------------------------------
    const QString tokC = QString::fromLatin1(kTokC);
    {
        DownloadManager dm;
        DownloadJob j;
        j.title = QStringLiteral("Cancelled");
        j.url = base + QStringLiteral("/f/hold-cancel.bin?token=") + tokC;
        j.dest = dir + QStringLiteral("/hold-cancel.bin");
        j.kind = QStringLiteral("video");
        j.key = QStringLiteral("k-cancel");
        dm.enqueue(j);
        CHECK(spinUntil([&] { const DownloadJob* p = jobByKey(dm, j.key); return p && p->received > 0; }));
        const bool heldWhileActive = UrlAtRest::sealsAtRest() || atRestQueueBytes().contains(tokC.toUtf8());
        CHECK(heldWhileActive);   // (the POSIX file does hold it while it can resume: that is rule (b))
        if (const DownloadJob* p = jobByKey(dm, j.key)) dm.cancel(p->id);
        CHECK(jobByKey(dm, j.key) == nullptr);
        const bool fileHoldsTokC = atRestQueueBytes().contains(tokC.toUtf8());
        CHECK(!fileHoldsTokC);
        CHECK(!QFileInfo::exists(j.dest + QStringLiteral(".part")));
    }

    // ---- (c) an old plaintext queue.json migrates on load ---------------------------------------------------
    const QString tokD = QString::fromLatin1(kTokD);
    const QString tokE = QString::fromLatin1(kTokE);
    const QString urlD = base + QStringLiteral("/f/mig.bin?token=") + tokD;
    {
        QJsonArray old;
        old.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("m-paused") },
                                { QStringLiteral("title"), QStringLiteral("Old paused") },
                                { QStringLiteral("url"), urlD },
                                { QStringLiteral("ref"), QString() },
                                { QStringLiteral("dest"), dir + QStringLiteral("/mig.bin") },
                                { QStringLiteral("kind"), QStringLiteral("video") },
                                { QStringLiteral("key"), QStringLiteral("k-mig") },
                                { QStringLiteral("state"), int(DownloadJob::Paused) } });
        old.append(QJsonObject{ { QStringLiteral("id"), QStringLiteral("m-failed") },
                                { QStringLiteral("title"), QStringLiteral("Old failed") },
                                { QStringLiteral("url"), base + QStringLiteral("/gone/old.bin?token=") + tokE },
                                { QStringLiteral("ref"), QString() },
                                { QStringLiteral("dest"), dir + QStringLiteral("/old.bin") },
                                { QStringLiteral("kind"), QStringLiteral("video") },
                                { QStringLiteral("key"), QStringLiteral("k-old") },
                                { QStringLiteral("state"), int(DownloadJob::Failed) } });
        QFile f(atRestQueuePath());
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(old).toJson(QJsonDocument::Compact));
        f.close();
#ifndef Q_OS_WIN
        // A pre-#437 file was created with the process umask — world-readable on a typical desktop.
        QFile::setPermissions(atRestQueuePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ReadGroup | QFileDevice::ReadOther);
#endif
    }
    {
        DownloadManager dm;
        const DownloadJob* paused = jobByKey(dm, QStringLiteral("k-mig"));
        const DownloadJob* failed = jobByKey(dm, QStringLiteral("k-old"));
        const bool pausedKeepsItsLink = paused && paused->url == urlD && paused->state == DownloadJob::Paused;
        CHECK(pausedKeepsItsLink);
        CHECK(failed && failed->url == base + QStringLiteral("/gone/old.bin"));
        CHECK(failed && failed->linkDropped && failed->state == DownloadJob::Failed);
        CHECK(failed && failed->error.contains(QStringLiteral("start the download again from the item")));
        // Rewritten AT ONCE, by the constructor — not at whatever change next happens to save.
        const QByteArray bytes = atRestQueueBytes();
        const bool fileHoldsTokD = bytes.contains(tokD.toUtf8());
        const bool fileHoldsTokE = bytes.contains(tokE.toUtf8());
        CHECK(!fileHoldsTokE);
        if (UrlAtRest::sealsAtRest())
        {
            CHECK(!fileHoldsTokD);
            const bool migratedSealed = UrlAtRest::unseal(atRestRecord(QStringLiteral("k-mig"))
                                                              .value(QStringLiteral("urlp")).toString()) == urlD;
            CHECK(migratedSealed);
        }
        else
        {
            CHECK(fileHoldsTokD);
        }
        CHECK(ownerOnly(atRestQueuePath()));
        dm.removeJob(QStringLiteral("m-paused"));
        dm.removeJob(QStringLiteral("m-failed"));
    }

    // ---- (a) a re-mintable job stores no url, and resumes through the minter -------------------------------
    const QString tokF = QString::fromLatin1(kTokF);
    const QString tokG = QString::fromLatin1(kTokG);
    const DownloadRecipe::Recipe recipe{ QStringLiteral("direct"), QStringLiteral("movie"),
                                         QStringLiteral("org.fixture.provider"), QStringLiteral("meta:Rml4dHVyZQ") };
    const QString ref = DownloadRecipe::encode(recipe);
    {
        // The ref is the four ids, readable back, and refuses anything shaped like a link.
        CHECK(DownloadRecipe::isRef(ref));
        DownloadRecipe::Recipe back;
        CHECK(DownloadRecipe::decode(ref, &back));
        CHECK(back.route == recipe.route && back.type == recipe.type && back.addonId == recipe.addonId
              && back.itemId == recipe.itemId);
        const DownloadRecipe::Recipe episode{ QStringLiteral("imdb"), QStringLiteral("series"), QString(),
                                              QStringLiteral("tt0000001:2:3") };
        DownloadRecipe::Recipe epBack;
        CHECK(DownloadRecipe::decode(DownloadRecipe::encode(episode), &epBack) && epBack.itemId == episode.itemId);
        CHECK(DownloadRecipe::encode({ QStringLiteral("direct"), QStringLiteral("movie"), QStringLiteral("a"),
                                       QStringLiteral("https://h/x?token=y") }).isEmpty());
        CHECK(DownloadRecipe::encode({ QStringLiteral("direct"), QStringLiteral("movie"), QStringLiteral("a"),
                                       QStringLiteral("id?token=y") }).isEmpty());
        CHECK(DownloadRecipe::encode({ QStringLiteral("direct"), QStringLiteral("movie"), QString(),
                                       QStringLiteral("meta:x") }).isEmpty());   // direct names its addon
        CHECK(!DownloadRecipe::decode(QStringLiteral("jf:0123:abcd"), nullptr));
    }
    const QString urlF = base + QStringLiteral("/f/hold-remint.bin?token=") + tokF;
    QString remintId;
    {
        DownloadManager dm;
        int asked = 0;
        dm.setAsyncUrlMinter([&asked](const QString&, DownloadManager::MintDone done) { ++asked; done(QString(), {}); });
        DownloadJob j;
        j.title = QStringLiteral("Re-mintable");
        j.url = urlF;
        j.dest = dir + QStringLiteral("/hold-remint.bin");
        j.kind = QStringLiteral("video");
        j.key = QStringLiteral("k-remint");
        // THE SITE's half: MainWindow::enqueueDownload binds exactly like this.
        CHECK(DownloadRecipe::bindJob(j, recipe));
        CHECK(j.url.isEmpty() && j.sourceRef == ref);
        dm.enqueue(j);
        CHECK(spinUntil([&] { const DownloadJob* p = jobByKey(dm, j.key); return p && p->received >= half; }));
        // The first transfer used the link the resolve had already minted: the source was not asked again.
        CHECK(asked == 0);
        const DownloadJob* p = jobByKey(dm, j.key);
        CHECK(p && p->url.isEmpty() && p->mintedUrl.isEmpty());
        if (p) { remintId = p->id; dm.pauseJob(p->id); }
        const QByteArray bytes = atRestQueueBytes();
        const bool fileHoldsTokF = bytes.contains(tokF.toUtf8());
        CHECK(!fileHoldsTokF);
        CHECK(!bytes.contains("hold-remint.bin?"));
        const QJsonObject rec = atRestRecord(j.key);
        CHECK(rec.value(QStringLiteral("ref")).toString() == ref);
        CHECK(rec.value(QStringLiteral("url")).toString().isEmpty());
        CHECK(!rec.contains(QStringLiteral("urlp")));     // not sealed: there is nothing to seal
    }
    {
        // A restart. The job waits for its minter; installed, the minter is asked with the REF and answers
        // later (a real re-mint is a network round trip); the fresh link resumes the .part.
        DownloadManager dm;
        const DownloadJob* p = jobByKey(dm, QStringLiteral("k-remint"));
        CHECK(p && p->state == DownloadJob::Paused && p->url.isEmpty() && p->sourceRef == ref);
        QStringList refsAsked;
        const QString urlG = base + QStringLiteral("/f/hold-remint.bin?token=") + tokG;
        dm.setAsyncUrlMinter([&refsAsked, urlG](const QString& r, DownloadManager::MintDone done) {
            refsAsked << r;
            QTimer::singleShot(0, [done, urlG] { done(urlG, {}); });
        });
        dm.resumeJob(remintId);
        CHECK(spinUntil([&] { const DownloadJob* q = jobByKey(dm, QStringLiteral("k-remint"));
                              return q && q->state == DownloadJob::Done; }));
        CHECK(refsAsked == QStringList{ ref });
        CHECK(fileHolds(dir + QStringLiteral("/hold-remint.bin"), fixtureBody(QStringLiteral("hold-remint.bin"))));
        const bool resumedOnTheFreshLink = !stub.requests().isEmpty() && stub.requests().last().ranged
                                        && stub.requests().last().query == QStringLiteral("token=") + tokG;
        CHECK(resumedOnTheFreshLink);
        const bool fileHoldsTokG = atRestQueueBytes().contains(tokG.toUtf8());
        CHECK(!fileHoldsTokG);
    }

    // ---- (e) a re-minted link naming a DIFFERENT file starts over ------------------------------------------
    {
        const DownloadRecipe::Recipe other{ QStringLiteral("imdb"), QStringLiteral("movie"), QString(),
                                            QStringLiteral("tt0000002") };
        DownloadJob j;
        j.title = QStringLiteral("Another release");
        j.url = base + QStringLiteral("/f/hold-first.bin?token=") + tokF;
        j.dest = dir + QStringLiteral("/swap.bin");
        j.kind = QStringLiteral("video");
        j.key = QStringLiteral("k-swap");
        CHECK(DownloadRecipe::bindJob(j, other));
        QString swapId;
        {
            DownloadManager dm;
            dm.enqueue(j);
            CHECK(spinUntil([&] { const DownloadJob* p = jobByKey(dm, j.key); return p && p->received >= half; }));
            if (const DownloadJob* p = jobByKey(dm, j.key)) { swapId = p->id; dm.pauseJob(p->id); }
        }
        DownloadManager dm;
        int asked = 0;
        const QString otherUrl = base + QStringLiteral("/f/other.bin?token=") + tokG;
        dm.setAsyncUrlMinter([&asked, otherUrl](const QString&, DownloadManager::MintDone done) {
            ++asked;
            done(otherUrl, {});
        });
        dm.resumeJob(swapId);
        CHECK(spinUntil([&] { const DownloadJob* q = jobByKey(dm, j.key); return q && q->state == DownloadJob::Done; }));
        // Not a splice of two files: the whole of the one the source now serves, fetched from the top.
        CHECK(fileHolds(dir + QStringLiteral("/swap.bin"), fixtureBody(QStringLiteral("other.bin"))));
        CHECK(asked == 2);                                          // the resume, then the restart
        CHECK(stub.count(QStringLiteral("/f/other.bin")) == 2);    // a ranged ask, then the whole file
    }

    // ---- (f) a ref-backed job is exactly what it was --------------------------------------------------------
    {
        DownloadManager dm;
        QStringList syncAsked;
        int asyncAsked = 0;
        dm.setUrlMinter([&syncAsked](const QString& r) { syncAsked << r; return QString(); });
        dm.setAsyncUrlMinter([&asyncAsked](const QString&, DownloadManager::MintDone done) {
            ++asyncAsked;
            done(QString(), {});
        });
        DownloadJob j;
        j.title = QStringLiteral("Server film");
        j.sourceRef = qual(kSrvA, kItem2);
        j.dest = dir + QStringLiteral("/server.mkv");
        j.kind = QStringLiteral("video");
        j.key = j.sourceRef;
        dm.enqueue(j);
        // The SYNCHRONOUS minter, as before; never the recipe one.
        CHECK(syncAsked == QStringList{ j.sourceRef });
        CHECK(asyncAsked == 0);
        const DownloadJob* p = jobByKey(dm, j.key);
        CHECK(p && p->state == DownloadJob::Failed && !p->linkDropped);
        CHECK(p && p->error.startsWith(QStringLiteral("the server this was downloaded from isn't set up")));
        const QJsonObject rec = atRestRecord(j.key);
        CHECK(rec.value(QStringLiteral("ref")).toString() == j.sourceRef);
        CHECK(rec.contains(QStringLiteral("url")) && rec.value(QStringLiteral("url")).toString().isEmpty());
        CHECK(!rec.contains(QStringLiteral("urlp")));
        CHECK(!rec.contains(QStringLiteral("dropped")));
        if (p) dm.removeJob(p->id);
    }

    QDir(dir).removeRecursively();
}

// (g) NOTHING THAT CARRIES FILES OFF THE DEVICE CARRIES queue.json. Read from the source tree, because each
// carrier is configuration or a list of paths rather than behaviour a headless run can exercise:
//   * Android Auto Backup and device-to-device transfer (android:allowBackup="true", no rules until #437,
//     so the whole files/ dir — queue.json included — went to the user's cloud backup);
//   * CloudSync's settings bundle, which zips named data-folder subdirectories;
//   * any other source that names the file at all.
static QByteArray readSource(const QString& rel)
{
    QFile f(QStringLiteral(EB_JFDOWNLOAD_NATIVE_DIR) + QLatin1Char('/') + rel);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static void sectionNoCarrier()
{
    // The rules name the file by its path under the data folder, which is where the manager keeps it.
    CHECK(QDir(AppPaths::dataDir()).relativeFilePath(atRestQueuePath()) == QStringLiteral("downloads/queue.json"));
    const QByteArray exclude = "<exclude domain=\"file\" path=\"downloads/queue.json\" />";

    const QByteArray manifest = readSource(QStringLiteral("android/AndroidManifest.xml"));
    CHECK(!manifest.isEmpty());
    CHECK(manifest.contains("android:fullBackupContent=\"@xml/backup_rules\""));
    CHECK(manifest.contains("android:dataExtractionRules=\"@xml/data_extraction_rules\""));
    const QByteArray legacy = readSource(QStringLiteral("android/res/xml/backup_rules.xml"));
    CHECK(legacy.count(exclude) == 1);
    CHECK(legacy.contains("<full-backup-content>"));
    CHECK(!legacy.contains("<include"));   // exclude-only: everything else backs up as before
    const QByteArray modern = readSource(QStringLiteral("android/res/xml/data_extraction_rules.xml"));
    CHECK(modern.count(exclude) == 2);
    const int cloud = modern.indexOf("<cloud-backup>"), transfer = modern.indexOf("<device-transfer>");
    CHECK(cloud >= 0 && transfer > cloud);
    CHECK(modern.indexOf(exclude, cloud) < transfer && modern.indexOf(exclude, transfer) > transfer);
    CHECK(!modern.contains("<include"));

    // CloudSync's bundle: every directory it zips is one of the two it has always zipped.
    const QString cloudSync = QString::fromUtf8(readSource(QStringLiteral("src/core/CloudSync.cpp")));
    CHECK(!cloudSync.isEmpty());
    const QRegularExpression call(QStringLiteral("zipAddDir\\(z,[^;]*QStringLiteral\\(\"([^\"]*)\"\\)\\s*[,)]"));
    QStringList zipped;
    for (auto m = call.globalMatch(cloudSync); m.hasNext();) zipped << m.next().captured(1);
    zipped.removeDuplicates();
    zipped.sort();
    CHECK(zipped == (QStringList{ QStringLiteral("addons"), QStringLiteral("themes") }));

    // No source but the manager names the file: a new carrier would have to.
    QStringList namers;
    QDirIterator it(QStringLiteral(EB_JFDOWNLOAD_NATIVE_DIR) + QStringLiteral("/src"),
                    { QStringLiteral("*.cpp"), QStringLiteral("*.h") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && f.readAll().contains("queue.json\"")) namers << QFileInfo(path).fileName();
    }
    CHECK(namers == QStringList{ QStringLiteral("DownloadManager.cpp") });

    // (a)'s SITE. bindJob is held to account above against the real manager; this pins that the add-on
    // download site still calls it with the recipe Recents uses, since MainWindow cannot be linked here.
    const QString mw = QString::fromUtf8(readSource(QStringLiteral("src/ui/MainWindow.cpp")));
    const int at = mw.indexOf(QStringLiteral("void MainWindow::enqueueDownload(const MediaItem& item)"));
    const int end = at < 0 ? -1 : mw.indexOf(QStringLiteral("dm_->enqueue(j);"), at);
    CHECK(at >= 0 && end > at);
    const QString site = (at >= 0 && end > at) ? mw.mid(at, end - at) : QString();
    CHECK(site.contains(QStringLiteral("applyRemintRecipe(recipe, item);")));
    CHECK(site.contains(QStringLiteral("DownloadRecipe::bindJob(j,")));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("probe_jfdownload"));

    // One ini, inside this process's scratch data dir, for all three stores — which is what the app has in
    // production (they share everythingbox.ini) and what lets §3 scan one named file.
    const QString ini = AppPaths::dataDir() + QStringLiteral("/probe-jfdownload.ini");
    QFile::remove(ini);
    JellyfinServerStore::setIniPathForTesting(ini);
    JellyfinDownload::setIniPathForTesting(ini);
    OfflineProgress::setIniPathForTesting(ini);

    sectionUrl();
    sectionIdentity();
    sectionCredential(ini);
    sectionBatch();
    sectionQueue();
    sectionCap();
    sectionSettingsKeys();
    // LAST, and deliberately after the credential scan in §3: this is the only section that creates and
    // destroys real files, and §3 walks the whole data dir asserting what is in it.
    sectionRemoveAfterWatched();
    // #437, after everything above: §9 runs real transfers over a loopback server with an event loop, and
    // starts from an empty queue.json of its own.
    sectionAtRest();
    sectionNoCarrier();

    if (failures) { std::fprintf(stderr, "JFDOWNLOAD-FAIL %d check(s)\n", failures); return 1; }
    std::printf("JFDOWNLOAD-OK\n");
    return 0;
}
