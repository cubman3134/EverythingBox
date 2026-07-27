// Headless coverage for the save-sync decision table (sections 1-7: pure — no network, no Drive, no files on
// disk) AND for the transport's two load-bearing orderings (section 8: real SaveSync driven against an
// in-memory cloud and a scratch directory under the temp dir; still no network).
// Prints SAVESYNC-OK on success; any failure prints SAVESYNC-FAIL <what> and exits non-zero.
#include "SaveSyncPlan.h"
#include "SaveSync.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "SAVESYNC-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace SaveSyncPlan;

static Entry e(const QString& name, const QString& sha, qint64 mtimeMs = 1000,
               const QString& dev = QStringLiteral("devA"))
{
    Entry x; x.name = name; x.sha = sha; x.mtimeMs = mtimeMs; x.size = sha.size(); x.deviceId = dev;
    return x;
}
static QHash<QString, Entry> one(const Entry& x) { QHash<QString, Entry> h; h.insert(x.name, x); return h; }

static Act actFor(const QVector<Decision>& ds, const QString& name)
{
    for (const Decision& d : ds) if (d.name == name) return d.act;
    return Act::None;
}
static const Decision* decFor(const QVector<Decision>& ds, const QString& name)
{
    for (const Decision& d : ds) if (d.name == name) return &d;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// Section 8 scaffolding: a SaveSync whose FIVE Drive calls are in-memory, so the real plan/execute/
// baseline path can be driven end to end. `calls` is an ordered transcript — the conflict rule is an
// ORDERING rule, so the assertion that matters is which entry comes first, not which files exist after.
// ---------------------------------------------------------------------------------------------------

static QString shaOf(const QByteArray& d)
{
    return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex());
}
static bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    return ok;
}
static QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

static QStringList g_roots;
static QString freshRoot(const QString& tag)
{
    const QString r = QDir::tempPath() + QStringLiteral("/mmv-savesync-probe-%1-%2")
                                             .arg(QCoreApplication::applicationPid()).arg(tag);
    QDir(r).removeRecursively();
    QDir().mkpath(r);
    g_roots << r;
    return r;
}
static void cleanupRoots() { for (const QString& r : g_roots) QDir(r).removeRecursively(); }

// Index of the first transcript entry starting with `prefix`, or -1.
static int firstCall(const QStringList& calls, const QString& prefix)
{
    for (int i = 0; i < calls.size(); ++i) if (calls.at(i).startsWith(prefix)) return i;
    return -1;
}

class FakeSync : public SaveSync
{
public:
    explicit FakeSync(const QString& root)
        : SaveSync(nullptr, root, QStringLiteral("devLocal")) {}

    // the in-memory cloud
    QHash<QString, QByteArray> blobs;    // sync name -> bytes
    QHash<QString, Entry>      index;    // the saves-index.json
    QStringList                calls;    // "dl:<name>", "up:<name>", "putidx", in call order
    QSet<QString>              failDownload, failUpload;
    bool failFetch = false, failPut = false;

    using SaveSync::firstRun;
    using SaveSync::readBaseline;
    using SaveSync::scanLocal;
    using SaveSync::writeBaseline;

protected:
    void resolveFolder(std::function<void(const QString&)> cb) override { cb(QStringLiteral("folder")); }

    void fetchManifest(const QString&, std::function<void(bool, const QHash<QString, Entry>&)> cb) override
    {
        if (failFetch) { cb(false, {}); return; }
        cb(true, index);
    }

    void putManifest(const QString&, const QHash<QString, Entry>& m, std::function<void(bool)> cb) override
    {
        calls << QStringLiteral("putidx");
        if (failPut) { cb(false); return; }
        index = m;
        cb(true);
    }

    void downloadInto(const QString&, const QString& name, const QString& destPath,
                      std::function<void(bool)> cb) override
    {
        calls << (QStringLiteral("dl:") + name);
        if (failDownload.contains(name) || !blobs.contains(name)) { cb(false); return; }
        cb(writeFile(destPath, blobs.value(name)));
    }

    void uploadFrom(const QString&, const QString& name, const QString& srcPath,
                    std::function<void(bool, const Entry&)> cb) override
    {
        calls << (QStringLiteral("up:") + name);
        if (failUpload.contains(name)) { cb(false, {}); return; }
        QFile f(srcPath);
        if (!f.open(QIODevice::ReadOnly)) { cb(false, {}); return; }
        const QByteArray d = f.readAll();
        f.close();
        blobs.insert(name, d);
        Entry sent;
        sent.name     = name;
        sent.sha      = shaOf(d);
        sent.size     = d.size();
        sent.mtimeMs  = QFileInfo(srcPath).lastModified().toMSecsSinceEpoch();
        sent.deviceId = QStringLiteral("devLocal");
        cb(true, sent);
    }
};

// A remote index row. `ageMs` is signed: negative puts the cloud copy in the future, i.e. it wins.
static Entry remoteRow(const QString& name, const QByteArray& bytes, qint64 ageMs, const QString& dev)
{
    Entry r;
    r.name     = name;
    r.sha      = shaOf(bytes);
    r.size     = bytes.size();
    r.mtimeMs  = QDateTime::currentMSecsSinceEpoch() - ageMs;
    r.deviceId = dev;
    return r;
}

static void transportChecks()
{
    const QString N = QStringLiteral("saves/Zelda.srm");
    const QByteArray LOCAL  = "LOCAL-SAVE-BYTES";
    const QByteArray REMOTE = "REMOTE-SAVE-BYTES";
    const qint64 kHour = 3600LL * 1000;

    // Both sides differ from this, so plan() sees "both changed" and declares a conflict.
    const auto staleBaseline = [&](FakeSync& s) {
        Entry b; b.name = N; b.sha = QStringLiteral("baseline-sha"); b.size = 1; b.mtimeMs = 1;
        QHash<QString, Entry> h; h.insert(N, b);
        CHECK(s.writeBaseline(h), "the baseline is writable");
    };

    // ---- 8a. scanLocal: recursive, prefixed, and blind to its own conflict artifacts ----
    {
        const QString root = freshRoot(QStringLiteral("scan"));
        writeFile(root + QStringLiteral("/saves/Flat.srm"), "a");
        writeFile(root + QStringLiteral("/saves/snes/Deep.srm"), "b");   // save-sync T4 namespacing
        writeFile(root + QStringLiteral("/states/Game.state1"), "c");
        writeFile(root + QStringLiteral("/saves/Flat.conflict-devB-20260101-000000.srm"), "d");
        writeFile(root + QStringLiteral("/addons/thing.js"), "e");        // outside the synced trees

        FakeSync s(root);
        const QHash<QString, Entry> got = s.scanLocal();
        CHECK(got.contains(QStringLiteral("saves/Flat.srm")), "scanLocal finds a flat save");
        CHECK(got.contains(QStringLiteral("saves/snes/Deep.srm")),
              "scanLocal RECURSES into saves/<system>/ and keeps the subdirectory in the name");
        CHECK(got.contains(QStringLiteral("states/Game.state1")), "scanLocal walks states/ too");
        CHECK(got.size() == 3, "scanLocal syncs neither .conflict-* artifacts nor anything outside saves/states");
        CHECK(got.value(QStringLiteral("saves/Flat.srm")).sha == shaOf("a"), "…hashing the contents");
    }

    // ---- 8b. THE RULE: when the REMOTE copy loses, it is fetched BEFORE the winner is uploaded ----
    {
        const QString root = freshRoot(QStringLiteral("remoteloses"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeSync s(root);
        staleBaseline(s);
        const Entry rr = remoteRow(N, REMOTE, kHour, QStringLiteral("devB"));   // cloud copy is an hour old
        s.index.insert(N, rr);
        s.blobs.insert(N, REMOTE);

        QString keptTitle, keptAs;
        QObject::connect(&s, &SaveSync::conflictKept, &s, [&](const QString& t, const QString& k) {
            keptTitle = t; keptAs = k;
        });

        bool ran = false; int up = 0, down = 0, conf = 0;
        s.syncNow([&](bool ok, int u, int d, int c) { ran = ok; up = u; down = d; conf = c; });
        CHECK(ran && conf == 1, "the local-newer conflict is resolved");

        const int dl = firstCall(s.calls, QStringLiteral("dl:"));
        const int ul = firstCall(s.calls, QStringLiteral("up:"));
        // The whole preservation promise. Uploading first overwrites the only copy of the loser's bytes.
        CHECK(dl >= 0 && ul >= 0 && dl < ul,
              "the losing CLOUD copy is downloaded BEFORE the winner is uploaded");

        const QString kept = conflictName(N, QStringLiteral("devB"), rr.mtimeMs);
        CHECK(readFile(root + QStringLiteral("/") + kept) == REMOTE,
              "…and the loser's bytes are on disk under its .conflict-* name");
        CHECK(kept.startsWith(QStringLiteral("saves/")), "…beside the original, not at the root");
        CHECK(readFile(root + QStringLiteral("/") + N) == LOCAL, "the winner keeps the real name");
        CHECK(s.blobs.value(N) == LOCAL, "…and is what the cloud now holds");
        CHECK(keptAs == kept && !keptTitle.isEmpty(), "conflictKept names the preserved copy and a title");
        CHECK(s.readBaseline().value(N).sha == shaOf(LOCAL),
              "the baseline records the bytes that actually reached the cloud");
        (void)up; (void)down;
    }

    // ---- 8c. …and if the loser cannot be fetched, NOTHING is uploaded ----
    {
        const QString root = freshRoot(QStringLiteral("remotelosesfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeSync s(root);
        staleBaseline(s);
        const Entry rr = remoteRow(N, REMOTE, kHour, QStringLiteral("devB"));
        s.index.insert(N, rr);
        s.blobs.insert(N, REMOTE);
        s.failDownload.insert(N);

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a conflict whose loser could not be preserved is a FAILED sync");
        CHECK(firstCall(s.calls, QStringLiteral("up:")) < 0,
              "the winner is NOT uploaded when the losing cloud copy could not be preserved");
        CHECK(s.blobs.value(N) == REMOTE, "…so the cloud copy is untouched");
        CHECK(readFile(root + QStringLiteral("/") + N) == LOCAL, "…and so is the local one");
        CHECK(s.readBaseline().value(N).sha == QStringLiteral("baseline-sha"),
              "a failed conflict does not advance the baseline");
    }

    // ---- 8d. when the LOCAL copy loses, it is renamed aside before the winner lands on it ----
    {
        const QString root = freshRoot(QStringLiteral("localloses"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        const qint64 localMtime = QFileInfo(root + QStringLiteral("/") + N).lastModified().toMSecsSinceEpoch();
        FakeSync s(root);
        staleBaseline(s);
        s.index.insert(N, remoteRow(N, REMOTE, -kHour, QStringLiteral("devB")));   // cloud copy is newer
        s.blobs.insert(N, REMOTE);

        bool ok = false; int conf = 0;
        s.syncNow([&](bool o, int, int, int c) { ok = o; conf = c; });
        CHECK(ok && conf == 1, "the cloud-newer conflict is resolved");
        const QString kept = conflictName(N, QStringLiteral("devLocal"), localMtime);
        CHECK(readFile(root + QStringLiteral("/") + kept) == LOCAL,
              "the losing LOCAL copy is preserved under its .conflict-* name");
        CHECK(readFile(root + QStringLiteral("/") + N) == REMOTE, "the cloud winner takes the real name");
        CHECK(firstCall(s.calls, QStringLiteral("up:")) < 0, "nothing is uploaded when the local copy lost");
        CHECK(s.readBaseline().value(N).sha == shaOf(REMOTE), "the baseline records what was written locally");
    }

    // ---- 8e. …and if the winner cannot be fetched, the local copy goes back under its real name ----
    {
        const QString root = freshRoot(QStringLiteral("locallosesfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeSync s(root);
        staleBaseline(s);
        s.index.insert(N, remoteRow(N, REMOTE, -kHour, QStringLiteral("devB")));
        s.blobs.insert(N, REMOTE);
        s.failDownload.insert(N);

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a conflict whose winner could not be fetched is a FAILED sync");
        CHECK(readFile(root + QStringLiteral("/") + N) == LOCAL,
              "the local copy is put BACK under its real name — a failed conflict is not half-applied");
        CHECK(s.readBaseline().value(N).sha == QStringLiteral("baseline-sha"),
              "…and the baseline still describes the pre-conflict state");
    }

    // ---- 8f. the baseline is written from what SUCCEEDED, never from the plan ----
    {
        const QString root = freshRoot(QStringLiteral("failedupload"));
        const QString GOOD = QStringLiteral("saves/Good.srm");
        const QString BAD  = QStringLiteral("saves/Bad.srm");
        writeFile(root + QStringLiteral("/") + GOOD, "good");
        writeFile(root + QStringLiteral("/") + BAD,  "bad");
        FakeSync s(root);
        CHECK(s.writeBaseline({}), "an empty baseline is writable");   // exists => not firstRun
        s.failUpload.insert(BAD);

        bool ok = true; int up = 0;
        s.syncNow([&](bool o, int u, int, int) { ok = o; up = u; });
        CHECK(!ok && up == 1, "one upload lands, one fails, and the sync reports failure");
        const QHash<QString, Entry> base = s.readBaseline();
        CHECK(base.value(GOOD).sha == shaOf("good"), "the file that uploaded IS recorded as synced");
        CHECK(!base.contains(BAD),
              "the file that FAILED is not — recording it would make the next run believe the cloud has it");
        CHECK(!s.index.contains(BAD), "…and it is not in the cloud index either");
    }

    // ---- 8g. an index we could not publish does not advance the baseline ----
    {
        const QString root = freshRoot(QStringLiteral("failedindex"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeSync s(root);
        s.failPut = true;
        CHECK(s.firstRun(), "no baseline yet");

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a sync whose index could not be published reports failure");
        CHECK(s.firstRun(),
              "…and writes no baseline: bytes the index does not list are not bytes the cloud has");
    }

    // ---- 8h. "could not reach the cloud" is never "the cloud is empty" ----
    {
        const QString root = freshRoot(QStringLiteral("unreachable"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeSync s(root);
        s.failFetch = true;

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "an unreadable index fails the sync");
        CHECK(s.firstRun() && readFile(root + QStringLiteral("/") + N) == LOCAL,
              "…and changes nothing: an empty remote view plus a tombstone is a DELETE");
        CHECK(firstCall(s.calls, QStringLiteral("up:")) < 0 && firstCall(s.calls, QStringLiteral("dl:")) < 0,
              "…nothing moved in either direction");
    }

    // ---- 8i. the ordinary two-way pass ----
    {
        const QString root = freshRoot(QStringLiteral("happy"));
        const QString MINE   = QStringLiteral("saves/Mine.srm");
        const QString THEIRS = QStringLiteral("states/Theirs.state1");
        writeFile(root + QStringLiteral("/") + MINE, "mine");
        FakeSync s(root);
        s.index.insert(THEIRS, remoteRow(THEIRS, "theirs", kHour, QStringLiteral("devB")));
        s.blobs.insert(THEIRS, "theirs");

        bool ok = false; int up = 0, down = 0, conf = 0;
        s.syncNow([&](bool o, int u, int d, int c) { ok = o; up = u; down = d; conf = c; });
        CHECK(ok && up == 1 && down == 1 && conf == 0, "a local-only save goes up and a cloud-only one comes down");
        CHECK(readFile(root + QStringLiteral("/") + THEIRS) == "theirs", "the downloaded save is on disk");
        CHECK(s.blobs.value(MINE) == "mine", "the uploaded save is in the cloud");
        const QHash<QString, Entry> base = s.readBaseline();
        CHECK(base.value(MINE).sha == shaOf("mine") && base.value(THEIRS).sha == shaOf("theirs"),
              "the baseline now describes both sides");
        CHECK(!s.firstRun(), "…so the next run is not a firstRun");

        // Running again with nothing changed must be a no-op, not a re-upload of everything.
        s.calls.clear();
        s.syncNow([&](bool o, int u, int d, int c) { ok = o; up = u; down = d; conf = c; });
        CHECK(ok && up == 0 && down == 0 && conf == 0 && s.calls.isEmpty(),
              "a second pass with nothing changed moves nothing");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString N = QStringLiteral("Zelda.srm");

    // ------------------------------------------------- 1. the core table
    {
        // unchanged locally, changed remotely -> Download
        CHECK(actFor(plan(one(e(N, "a")), one(e(N, "b", 2000)), one(e(N, "a")), {}, false), N) == Act::Download,
              "remote-only change downloads");
        // changed locally, unchanged remotely -> Upload
        CHECK(actFor(plan(one(e(N, "b", 2000)), one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::Upload,
              "local-only change uploads");
        // neither changed -> None
        CHECK(actFor(plan(one(e(N, "a")), one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::None,
              "no change does nothing");
        // both changed, DIFFERENT content -> Conflict
        CHECK(actFor(plan(one(e(N, "b", 3000)), one(e(N, "c", 2000)), one(e(N, "a")), {}, false), N) == Act::Conflict,
              "both changed and differ is a conflict");
        // both changed to the SAME content -> None (they independently reached the same bytes)
        CHECK(actFor(plan(one(e(N, "z", 3000)), one(e(N, "z", 2000)), one(e(N, "a")), {}, false), N) == Act::None,
              "both changed to identical bytes is not a conflict");

        // Totality: once the two sides differ, EVERY baseline shape yields a real action. plan() has no
        // "neither side changed yet they differ" case to fall into — that would need L.sha == B.sha and
        // R.sha == B.sha, i.e. L.sha == R.sha, which returns earlier. This pins that there is no baseline
        // a caller can hand us that drops a differing pair on the floor.
        for (const char* b : { "a", "b", "c", "" })
        {
            const QHash<QString, Entry> base =
                *b ? one(e(N, QString::fromLatin1(b))) : QHash<QString, Entry>();
            const QVector<Decision> ds = plan(one(e(N, "b", 3000)), one(e(N, "c", 2000)), base, {}, false);
            CHECK(actFor(ds, N) != Act::None, "two differing sides always produce an action");
        }
    }

    // ------------------------------------------------- 2. absence is a restore unless a tombstone says otherwise
    {
        const QHash<QString, Entry> gone;   // local missing entirely
        CHECK(actFor(plan(gone, one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::Download,
              "a locally-missing file with NO tombstone is a restore, never a delete");
        CHECK(actFor(plan(gone, one(e(N, "a")), one(e(N, "a")), { N }, false), N) == Act::DeleteRemote,
              "…with a tombstone it is a real delete");
        CHECK(actFor(plan(one(e(N, "a")), gone, one(e(N, "a")), {}, false), N) == Act::Upload,
              "a remotely-missing file with NO tombstone is re-uploaded, never deleted locally");
        CHECK(actFor(plan(one(e(N, "a")), gone, one(e(N, "a")), { N }, false), N) == Act::DeleteLocal,
              "…with a tombstone it is a real delete");

        // …but ONLY of the copy that was tombstoned. Devices A and B are in sync on "a". B saves and
        // uploads "b". A, still holding the "a" baseline, deletes its copy and tombstones it. If the
        // tombstone won here, B's brand-new save would be destroyed — and since no conflict is declared,
        // no .conflict-* copy would exist to recover it from. Update beats delete.
        CHECK(actFor(plan(gone, one(e(N, "b", 2000)), one(e(N, "a")), { N }, false), N) == Act::Download,
              "a tombstone must NOT delete a cloud copy that changed after the baseline");

        // The mirror, and the one an undifferentiated tombstone set makes easy to hit: A deletes the file
        // (tombstone written), plays again and recreates it, and the earlier upload never landed so the
        // cloud has no copy. A live local file that differs from the baseline post-dates the tombstone and
        // contradicts it — deleting it here would destroy the user's own save.
        CHECK(actFor(plan(one(e(N, "c", 2000)), gone, one(e(N, "a")), { N }, false), N) == Act::Upload,
              "a stale tombstone must NOT delete a local save recreated since the baseline");
    }

    // ------------------------------------------------- 3. THE SAFETY RULE: firstRun never deletes
    {
        const QHash<QString, Entry> gone;
        const QHash<QString, Entry> noBaseline;
        // Every shape that would otherwise delete, with firstRun set:
        for (const auto& tomb : { QSet<QString>{}, QSet<QString>{ N } })
        {
            const QVector<Decision> a = plan(gone, one(e(N, "a")), noBaseline, tomb, true);
            const QVector<Decision> b = plan(one(e(N, "a")), gone, noBaseline, tomb, true);
            CHECK(actFor(a, N) != Act::DeleteRemote && actFor(a, N) != Act::DeleteLocal,
                  "firstRun never deletes (cloud-only file)");
            CHECK(actFor(b, N) != Act::DeleteRemote && actFor(b, N) != Act::DeleteLocal,
                  "firstRun never deletes (local-only file)");
        }
        // …and it still moves data in both directions.
        CHECK(actFor(plan(gone, one(e(N, "a")), noBaseline, {}, true), N) == Act::Download,
              "firstRun downloads a cloud-only save");
        CHECK(actFor(plan(one(e(N, "a")), gone, noBaseline, {}, true), N) == Act::Upload,
              "firstRun uploads a local-only save");

        // The actual first-run case for two devices that each already have saves: both sides present and
        // differing, no baseline. This is precisely what the OLD rule got wrong — it took the cloud
        // wholesale and the local save was gone. It must be a conflict, so both copies survive.
        CHECK(actFor(plan(one(e(N, "b", 3000)), one(e(N, "c", 2000)), noBaseline, {}, true), N) == Act::Conflict,
              "firstRun with saves on BOTH sides is a conflict, never 'take the cloud'");
    }

    // ------------------------------------------------- 4. conflict winner + localWins
    {
        // NOTE: the plan() result is held in a NAMED local before decFor() takes a pointer into it. Pointing
        // into the temporary directly is a use-after-free, and a freed block gets reused by the very next
        // plan() call — which silently made two independent decisions compare as one.
        // Local is newer by well over the skew window.
        const QVector<Decision> dsLocalNewer =
            plan(one(e(N, "b", 90000)), one(e(N, "c", 10000)), one(e(N, "a")), {}, false);
        const Decision* d = decFor(dsLocalNewer, N);
        CHECK(d && d->act == Act::Conflict && d->localWins, "the newer LOCAL copy wins the real name");
        // Remote is newer. localWins must be FALSE — this is the case where the transport has to fetch the
        // losing remote copy BEFORE overwriting it, so getting this flag wrong destroys a save.
        const QVector<Decision> dsRemoteNewer =
            plan(one(e(N, "b", 10000)), one(e(N, "c", 90000)), one(e(N, "a")), {}, false);
        const Decision* r = decFor(dsRemoteNewer, N);
        CHECK(r && r->act == Act::Conflict && !r->localWins, "the newer REMOTE copy wins the real name");
    }

    // ------------------------------------------------- 5. clock skew: both devices must agree
    {
        // Inside the skew window the mtimes are a tie, broken deterministically on sha.
        //
        // The sha deliberately points the OPPOSITE way from the mtime: local has the LATER sha but the
        // EARLIER mtime. Only the skew rule can return "local wins" here; a bare mtime comparison returns
        // the other answer. A fixture where both rules agree cannot tell you which one ran — and an
        // antisymmetry-only assertion holds under ANY antisymmetric rule, so it tests agreement, not the
        // window. Hence the ABSOLUTE assertion below alongside it.
        Entry L = e(N, "zzz", 10000, QStringLiteral("devA"));
        Entry R = e(N, "aaa", 10000 + kSkewWindowMs - 1, QStringLiteral("devB"));
        const QVector<Decision> ds1 = plan(one(L), one(R), one(e(N, "base")), {}, false);
        // Now compute the SAME conflict from the other device's point of view: its local is what was remote.
        const QVector<Decision> ds2 = plan(one(R), one(L), one(e(N, "base")), {}, false);
        const Decision* d1 = decFor(ds1, N);
        const Decision* d2 = decFor(ds2, N);
        CHECK(d1 && d2 && d1->act == Act::Conflict && d2->act == Act::Conflict, "a near-tie is still a conflict");
        CHECK(d1 && d1->localWins,
              "inside the skew window the mtimes are a tie and the sha decides — not the newer mtime");
        // If A says "local wins" then B, looking at the same pair, must say "local LOSES" — i.e. the two
        // devices must pick the SAME physical copy. Disagreement here means both keep their own and diverge
        // forever, which is worse than having no rule at all.
        CHECK(d1 && d2 && d1->localWins != d2->localWins, "both devices independently choose the same winner");

        // EXACTLY kSkewWindowMs apart is outside the window (`<`, not `<=`), so mtime decides and the older
        // local copy loses despite holding the greater sha. Same shape one ms in either direction gives a
        // different answer, which is what pins the boundary itself rather than "somewhere near it".
        Entry EdgeL = e(N, "zzz", 10000, QStringLiteral("devA"));
        Entry EdgeR = e(N, "aaa", 10000 + kSkewWindowMs, QStringLiteral("devB"));
        const QVector<Decision> dsEdge = plan(one(EdgeL), one(EdgeR), one(e(N, "base")), {}, false);
        const Decision* edge = decFor(dsEdge, N);
        CHECK(edge && edge->act == Act::Conflict && !edge->localWins,
              "exactly kSkewWindowMs apart is OUTSIDE the window: mtime decides, sha does not");

        // Outside the window, time decides and the tie-break must not interfere.
        Entry Old = e(N, "zzz", 10000, QStringLiteral("devZ"));           // lexically greatest sha
        Entry New = e(N, "aaa", 10000 + kSkewWindowMs + 1, QStringLiteral("devA"));
        const QVector<Decision> dsSkew = plan(one(New), one(Old), one(e(N, "base")), {}, false);
        const Decision* t = decFor(dsSkew, N);
        CHECK(t && t->localWins, "outside the skew window the newer file wins regardless of sha");
    }

    // ------------------------------------------------- 6. in the baseline, gone from both sides
    {
        const QHash<QString, Entry> gone;
        const QVector<Decision> ds = plan(gone, gone, one(e(N, "a")), {}, false);
        CHECK(decFor(ds, N) == nullptr || actFor(ds, N) == Act::None,
              "a file gone from BOTH sides yields no action — it is not a deletion");
    }

    // ------------------------------------------------- 7. names
    {
        const QString c = conflictName(QStringLiteral("Zelda.state1"), QStringLiteral("devA"), 0);
        CHECK(c.endsWith(QStringLiteral(".state1")), "the conflict copy keeps its extension");
        CHECK(c.contains(QStringLiteral("devA")), "…and names the device that lost");
        CHECK(c != conflictName(QStringLiteral("Zelda.state1"), QStringLiteral("devB"), 0),
              "two devices' conflict copies cannot collide");
        CHECK(isConflictArtifact(c), "a conflict copy is recognised as an artifact");
        CHECK(!isConflictArtifact(QStringLiteral("Zelda.state1")), "an ordinary save is not");
        // The artifact must never be planned for sync — it is local recovery only.
        const QVector<Decision> ds = plan(one(e(c, "x")), {}, {}, {}, false);
        CHECK(decFor(ds, c) == nullptr || actFor(ds, c) == Act::None, "a conflict artifact is never synced");

        // Names are paths relative to the saves root, so the directory has to survive: dropping it writes
        // the preserved copy into the wrong folder…
        const QString p = conflictName(QStringLiteral("saves/nes/Zelda.srm"), QStringLiteral("devA"), 0);
        CHECK(p.startsWith(QStringLiteral("saves/nes/")), "the conflict copy is written beside its original");
        CHECK(p.endsWith(QStringLiteral(".srm")), "…still keeping its extension");
        CHECK(isConflictArtifact(p), "…and is still recognised as an artifact, so it is never synced");
        // …and, worse, collapses two systems' same-named saves onto ONE output name, so preserving the
        // second silently overwrites the first — destroying the very copy the conflict rule exists to keep.
        CHECK(conflictName(QStringLiteral("saves/nes/Zelda.srm"), QStringLiteral("devA"), 0) !=
                  conflictName(QStringLiteral("saves/snes/Zelda.srm"), QStringLiteral("devA"), 0),
              "the same basename under two systems cannot collide");
    }

    // ------------------------------------------------- 8. the TRANSPORT: ordering, and a baseline that
    //                                                       only ever records what actually happened.
    //
    // These are the two rules SaveSync exists to keep, and neither can be asserted against a live Google
    // account — so the cloud is substituted through SaveSync's transport seam and everything else (plan,
    // execute, conflict ordering, baseline write) is the real code running against real files.
    {
        transportChecks();
    }

    cleanupRoots();
    if (failures) { std::fprintf(stderr, "SAVESYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SAVESYNC-OK\n");
    return 0;
}
