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
#include "Jellyfin.h"
#include "JellyfinDownload.h"
#include "JellyfinServerStore.h"
#include "OfflineProgress.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>

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

    if (failures) { std::fprintf(stderr, "JFDOWNLOAD-FAIL %d check(s)\n", failures); return 1; }
    std::printf("JFDOWNLOAD-OK\n");
    return 0;
}
