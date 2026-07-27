// Headless coverage for the save-sync decision table (sections 1-7: pure — no network, no Drive, no files on
// disk) AND for the transport (section 8: the real SaveSync driven against an in-memory Drive and a scratch
// directory under the temp dir; still no network).
//
// The section-8 seam is CloudSync's four Drive primitives, NOT SaveSync's own methods. That matters: with the
// seam one level up, the fake REPLACED the torn-write guard, both listOk guards, the Drive-name mapping and
// QSaveFile's atomicity — two of the three rules SaveSync's header calls load-bearing were asserted nowhere.
// Here everything in SaveSync.cpp is the real code, including the index compare-and-swap.
//
// Prints SAVESYNC-OK on success; any failure prints SAVESYNC-FAIL <what> and exits non-zero.
#include "SaveSyncPlan.h"
#include "SaveSync.h"

#include "CloudSync.h"
#include "Tombstones.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
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
// Section 8 scaffolding: an in-memory Drive behind CloudSync's four virtuals. `calls` is an ordered
// transcript — the conflict rule is an ORDERING rule, so the assertion that matters is which entry comes
// first, not which files exist after.
// ---------------------------------------------------------------------------------------------------

static const QLatin1String kIdx("saves-index.json");
static const QLatin1String kTombStore("saves");

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

// Saves are keyed in the cloud by SaveSync's OWN mapping, so the probe addresses the fake Drive exactly the
// way the code under test does (a duplicated mapping here would hide a change to the real one).
static QString dlOf(const QString& rel) { return QStringLiteral("dl:") + SaveSync::driveNameFor(rel); }
static QString upOf(const QString& rel) { return QStringLiteral("up:") + SaveSync::driveNameFor(rel); }

// Tombstones live in the shared ini next to the exe, so every case starts from a clean deleted/saves group —
// otherwise a tombstone written by one case silently deletes a save in the next (and across probe runs).
static void clearTombs()
{
    const QVector<Tombstones::Entry> ts = Tombstones::all(kTombStore);
    for (const Tombstones::Entry& t : ts) Tombstones::remove(kTombStore, t.key);
}

static QStringList g_roots;
static QString freshRoot(const QString& tag)
{
    const QString r = QDir::tempPath() + QStringLiteral("/mmv-savesync-probe-%1-%2")
                                             .arg(QCoreApplication::applicationPid()).arg(tag);
    QDir(r).removeRecursively();
    QDir().mkpath(r);
    g_roots << r;
    clearTombs();
    return r;
}
static void cleanupRoots() { for (const QString& r : g_roots) QDir(r).removeRecursively(); }

// Index of the first transcript entry starting with `prefix`, or -1.
static int firstCall(const QStringList& calls, const QString& prefix)
{
    for (int i = 0; i < calls.size(); ++i) if (calls.at(i).startsWith(prefix)) return i;
    return -1;
}
// Any upload/download of a SAVE (the index's own traffic uses its own name, and matching "up:"/"dl:" bare
// would make "nothing moved" assertions pass on the index round trip alone).
static int firstBlobUpload(const QStringList& calls)   { return firstCall(calls, QStringLiteral("up:save-")); }
static int firstBlobDownload(const QStringList& calls) { return firstCall(calls, QStringLiteral("dl:save-")); }

class FakeCloud : public CloudSync
{
public:
    struct Blob { QByteArray data; QString hash; };   // hash == Drive's appProperties.stateHash

    QHash<QString, Blob> files;                       // DRIVE name -> content
    QStringList          calls;                       // "find:<n>", "up:<n>", "dl:<n>", in call order
    QSet<QString>        failFind, failUpload, failDownload;   // by DRIVE name
    bool                 failFolder = false;
    QSet<QString>        deferFind;                   // park the reply in `pending` instead of answering
    std::function<void()> pending;
    std::function<void(const QString& op, const QString& name)> hook;   // fired BEFORE each primitive

    static QString idFor(const QString& name)  { return QStringLiteral("id:") + name; }
    static QString nameFor(const QString& id)  { return id.mid(3); }

    void ensureFolder(std::function<void(const QString&)> cb) override
    {
        if (hook) hook(QStringLiteral("folder"), QString());
        cb(failFolder ? QString() : QStringLiteral("folder"));
    }

    void findFile(const QString&, const QString& name,
                  std::function<void(bool, const QString&, const QString&, const QString&)> cb) override
    {
        if (hook) hook(QStringLiteral("find"), name);
        calls << (QStringLiteral("find:") + name);
        auto reply = [this, name, cb] {
            if (failFind.contains(name)) { cb(false, QString(), QString(), QString()); return; }
            const auto it = files.constFind(name);
            if (it == files.constEnd()) { cb(true, QString(), QString(), QString()); return; }
            cb(true, idFor(name), QStringLiteral("2026-01-01T00:00:00Z"), it.value().hash);
        };
        if (deferFind.contains(name)) { pending = reply; return; }
        reply();
    }

    void uploadFile(const QString&, const QString&, const QString& name, const QString&,
                    const QByteArray& data, const QString& stateHash,
                    std::function<void(const QString&)> cb) override
    {
        if (hook) hook(QStringLiteral("up"), name);
        calls << (QStringLiteral("up:") + name);
        if (failUpload.contains(name)) { cb(QString()); return; }
        files.insert(name, Blob{ data, stateHash });
        cb(idFor(name));
    }

    void downloadFile(const QString& fileId, std::function<void(bool, const QByteArray&)> cb) override
    {
        const QString name = nameFor(fileId);
        if (hook) hook(QStringLiteral("dl"), name);
        calls << (QStringLiteral("dl:") + name);
        if (failDownload.contains(name) || !files.contains(name)) { cb(false, {}); return; }
        cb(true, files.value(name).data);
    }
};

// The real class, with the protected local-state helpers and the torn-write hook exposed.
class TestSync : public SaveSync
{
public:
    TestSync(CloudSync* c, const QString& root, const QString& dev = QStringLiteral("devLocal"))
        : SaveSync(c, root, dev) {}

    using SaveSync::firstRun;
    using SaveSync::readBaseline;
    using SaveSync::scanLocal;
    using SaveSync::writeBaseline;

    void setMidReadHook(std::function<void(const QString&)> h) { midReadHook_ = std::move(h); }
};

// ---- the cloud index, written and read the way the wire format says --------------------------------

static QByteArray indexJson(const QHash<QString, Entry>& files, const QVector<SaveSync::Tomb>& tombs)
{
    QJsonObject f;
    for (auto it = files.constBegin(); it != files.constEnd(); ++it)
    {
        const Entry& x = it.value();
        f.insert(it.key(), QJsonObject{ { QStringLiteral("sha"), x.sha },
                                        { QStringLiteral("mtimeMs"), static_cast<double>(x.mtimeMs) },
                                        { QStringLiteral("size"), static_cast<double>(x.size) },
                                        { QStringLiteral("deviceId"), x.deviceId } });
    }
    QJsonArray t;
    for (const SaveSync::Tomb& x : tombs)
        t.append(QJsonObject{ { QStringLiteral("key"), x.key },
                              { QStringLiteral("ts"), static_cast<double>(x.ts) } });
    QJsonObject root;
    root.insert(QStringLiteral("v"), 2);
    root.insert(QStringLiteral("files"), f);
    root.insert(QStringLiteral("tombs"), t);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static void seedIndex(FakeCloud& c, const QHash<QString, Entry>& files,
                      const QVector<SaveSync::Tomb>& tombs = {})
{
    const QByteArray j = indexJson(files, tombs);
    c.files.insert(kIdx, FakeCloud::Blob{ j, shaOf(j) });
}
static void seedBlob(FakeCloud& c, const QString& rel, const QByteArray& bytes)
{
    c.files.insert(SaveSync::driveNameFor(rel), FakeCloud::Blob{ bytes, shaOf(bytes) });
}
static QByteArray blobOf(const FakeCloud& c, const QString& rel)
{
    return c.files.value(SaveSync::driveNameFor(rel)).data;
}
static QJsonObject indexDoc(const FakeCloud& c)
{
    return QJsonDocument::fromJson(c.files.value(kIdx).data).object();
}
static QHash<QString, Entry> cloudIndex(const FakeCloud& c)
{
    QHash<QString, Entry> out;
    const QJsonObject f = indexDoc(c).value(QStringLiteral("files")).toObject();
    for (auto it = f.constBegin(); it != f.constEnd(); ++it)
    {
        const QJsonObject o = it.value().toObject();
        Entry x;
        x.name    = it.key();
        x.sha     = o.value(QStringLiteral("sha")).toString();
        x.mtimeMs = static_cast<qint64>(o.value(QStringLiteral("mtimeMs")).toDouble());
        out.insert(x.name, x);
    }
    return out;
}
static bool indexHasTomb(const FakeCloud& c, const QString& key)
{
    const QJsonArray t = indexDoc(c).value(QStringLiteral("tombs")).toArray();
    for (const auto& v : t) if (v.toObject().value(QStringLiteral("key")).toString() == key) return true;
    return false;
}

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
    const auto staleBaseline = [&](TestSync& s) {
        Entry b; b.name = N; b.sha = QStringLiteral("baseline-sha"); b.size = 1; b.mtimeMs = 1;
        QHash<QString, Entry> h; h.insert(N, b);
        CHECK(s.writeBaseline(h), "the baseline is writable");
    };

    // ---- 8a. scanLocal: recursive, prefixed, blind to its own conflict artifacts — and RESTRICTED ----
    {
        const QString root = freshRoot(QStringLiteral("scan"));
        writeFile(root + QStringLiteral("/saves/Flat.srm"), "a");
        writeFile(root + QStringLiteral("/saves/snes/Deep.srm"), "b");   // save-sync T4 namespacing
        writeFile(root + QStringLiteral("/states/Game.state1"), "c");
        writeFile(root + QStringLiteral("/saves/Flat.conflict-devB-20260101-000000.srm"), "d");
        writeFile(root + QStringLiteral("/addons/thing.js"), "e");        // outside the synced trees

        FakeCloud cloud;
        TestSync s(&cloud, root);
        const QHash<QString, Entry> got = s.scanLocal();
        CHECK(got.contains(QStringLiteral("saves/Flat.srm")), "scanLocal finds a flat save");
        CHECK(got.contains(QStringLiteral("saves/snes/Deep.srm")),
              "scanLocal RECURSES into saves/<system>/ and keeps the subdirectory in the name");
        CHECK(got.contains(QStringLiteral("states/Game.state1")), "scanLocal walks states/ too");
        CHECK(got.size() == 3, "scanLocal syncs neither .conflict-* artifacts nor anything outside saves/states");
        CHECK(got.value(QStringLiteral("saves/Flat.srm")).sha == shaOf("a"), "…hashing the contents");

        // The debounced push is a RESTRICTED run and runs on the GUI thread: hashing the whole tree there
        // freezes the UI over multi-MB states for one F2 press.
        const QSet<QString> only{ QStringLiteral("saves/Flat.srm") };
        const QHash<QString, Entry> narrow = s.scanLocal(nullptr, only);
        CHECK(narrow.size() == 1 && narrow.contains(QStringLiteral("saves/Flat.srm")),
              "a restricted scan hashes ONLY the names it was asked for");
        CHECK(narrow.value(QStringLiteral("saves/Flat.srm")).sha == got.value(QStringLiteral("saves/Flat.srm")).sha,
              "…and reports exactly what the full walk would have");
        // markDirty's contract: the name is root-relative INCLUDING the saves/|states/ prefix.
        CHECK(s.scanLocal(nullptr, QSet<QString>{ QStringLiteral("Flat.srm") }).isEmpty(),
              "…while a bare filename (no saves/ prefix) matches nothing at all");
    }

    // ---- 8b. THE RULE: when the REMOTE copy loses, it is fetched BEFORE the winner is uploaded ----
    {
        const QString root = freshRoot(QStringLiteral("remoteloses"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        staleBaseline(s);
        const Entry rr = remoteRow(N, REMOTE, kHour, QStringLiteral("devB"));   // cloud copy is an hour old
        seedIndex(cloud, one(rr));
        seedBlob(cloud, N, REMOTE);

        QString keptTitle, keptAs;
        QObject::connect(&s, &SaveSync::conflictKept, &s, [&](const QString& t, const QString& k) {
            keptTitle = t; keptAs = k;
        });

        bool ran = false; int up = 0, down = 0, conf = 0;
        s.syncNow([&](bool ok, int u, int d, int c) { ran = ok; up = u; down = d; conf = c; });
        CHECK(ran && conf == 1, "the local-newer conflict is resolved");

        const int dl = firstCall(cloud.calls, dlOf(N));
        const int ul = firstCall(cloud.calls, upOf(N));
        // The whole preservation promise. Uploading first overwrites the only copy of the loser's bytes.
        CHECK(dl >= 0 && ul >= 0 && dl < ul,
              "the losing CLOUD copy is downloaded BEFORE the winner is uploaded");

        const QString kept = conflictName(N, QStringLiteral("devB"), rr.mtimeMs);
        CHECK(readFile(root + QStringLiteral("/") + kept) == REMOTE,
              "…and the loser's bytes are on disk under its .conflict-* name");
        CHECK(kept.startsWith(QStringLiteral("saves/")), "…beside the original, not at the root");
        CHECK(readFile(root + QStringLiteral("/") + N) == LOCAL, "the winner keeps the real name");
        CHECK(blobOf(cloud, N) == LOCAL, "…and is what the cloud now holds");
        CHECK(keptAs == kept && !keptTitle.isEmpty(), "conflictKept names the preserved copy and a title");
        CHECK(s.readBaseline().value(N).sha == shaOf(LOCAL),
              "the baseline records the bytes that actually reached the cloud");
        (void)up; (void)down;
    }

    // ---- 8c. …and if the loser cannot be fetched, NOTHING is uploaded ----
    {
        const QString root = freshRoot(QStringLiteral("remotelosesfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        staleBaseline(s);
        seedIndex(cloud, one(remoteRow(N, REMOTE, kHour, QStringLiteral("devB"))));
        seedBlob(cloud, N, REMOTE);
        cloud.failDownload.insert(SaveSync::driveNameFor(N));

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a conflict whose loser could not be preserved is a FAILED sync");
        CHECK(firstBlobUpload(cloud.calls) < 0,
              "the winner is NOT uploaded when the losing cloud copy could not be preserved");
        CHECK(blobOf(cloud, N) == REMOTE, "…so the cloud copy is untouched");
        CHECK(readFile(root + QStringLiteral("/") + N) == LOCAL, "…and so is the local one");
        CHECK(s.readBaseline().value(N).sha == QStringLiteral("baseline-sha"),
              "a failed conflict does not advance the baseline");
    }

    // ---- 8d. when the LOCAL copy loses, it is renamed aside before the winner lands on it ----
    {
        const QString root = freshRoot(QStringLiteral("localloses"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        const qint64 localMtime = QFileInfo(root + QStringLiteral("/") + N).lastModified().toMSecsSinceEpoch();
        FakeCloud cloud;
        TestSync s(&cloud, root);
        staleBaseline(s);
        seedIndex(cloud, one(remoteRow(N, REMOTE, -kHour, QStringLiteral("devB"))));   // cloud copy is newer
        seedBlob(cloud, N, REMOTE);

        bool ok = false; int conf = 0;
        s.syncNow([&](bool o, int, int, int c) { ok = o; conf = c; });
        CHECK(ok && conf == 1, "the cloud-newer conflict is resolved");
        const QString kept = conflictName(N, QStringLiteral("devLocal"), localMtime);
        CHECK(readFile(root + QStringLiteral("/") + kept) == LOCAL,
              "the losing LOCAL copy is preserved under its .conflict-* name");
        CHECK(readFile(root + QStringLiteral("/") + N) == REMOTE, "the cloud winner takes the real name");
        CHECK(firstBlobUpload(cloud.calls) < 0, "nothing is uploaded when the local copy lost");
        CHECK(s.readBaseline().value(N).sha == shaOf(REMOTE), "the baseline records what was written locally");
    }

    // ---- 8e. …and if the winner cannot be fetched, the local copy goes back under its real name ----
    {
        const QString root = freshRoot(QStringLiteral("locallosesfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        staleBaseline(s);
        seedIndex(cloud, one(remoteRow(N, REMOTE, -kHour, QStringLiteral("devB"))));
        seedBlob(cloud, N, REMOTE);
        cloud.failDownload.insert(SaveSync::driveNameFor(N));

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
        FakeCloud cloud;
        TestSync s(&cloud, root);
        CHECK(s.writeBaseline({}), "an empty baseline is writable");   // exists => not firstRun
        cloud.failUpload.insert(SaveSync::driveNameFor(BAD));

        bool ok = true; int up = 0;
        s.syncNow([&](bool o, int u, int, int) { ok = o; up = u; });
        CHECK(!ok && up == 1, "one upload lands, one fails, and the sync reports failure");
        const QHash<QString, Entry> base = s.readBaseline();
        CHECK(base.value(GOOD).sha == shaOf("good"), "the file that uploaded IS recorded as synced");
        CHECK(!base.contains(BAD),
              "the file that FAILED is not — recording it would make the next run believe the cloud has it");
        CHECK(!cloudIndex(cloud).contains(BAD), "…and it is not in the cloud index either");
        CHECK(cloudIndex(cloud).contains(GOOD), "…while the one that landed is");
    }

    // ---- 8g. an index we could not publish does not advance the baseline ----
    {
        const QString root = freshRoot(QStringLiteral("failedindex"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        cloud.failUpload.insert(kIdx);
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
        FakeCloud cloud;
        TestSync s(&cloud, root);
        cloud.failFind.insert(kIdx);   // listOk == false on the index lookup

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "an unreadable index fails the sync");
        CHECK(s.firstRun() && readFile(root + QStringLiteral("/") + N) == LOCAL,
              "…and changes nothing: an empty remote view plus a tombstone is a DELETE");
        CHECK(firstBlobUpload(cloud.calls) < 0 && firstBlobDownload(cloud.calls) < 0,
              "…nothing moved in either direction");
    }

    // ---- 8i. the ordinary two-way pass ----
    {
        const QString root = freshRoot(QStringLiteral("happy"));
        const QString MINE   = QStringLiteral("saves/Mine.srm");
        const QString THEIRS = QStringLiteral("states/Theirs.state1");
        writeFile(root + QStringLiteral("/") + MINE, "mine");
        FakeCloud cloud;
        TestSync s(&cloud, root);
        seedIndex(cloud, one(remoteRow(THEIRS, "theirs", kHour, QStringLiteral("devB"))));
        seedBlob(cloud, THEIRS, "theirs");

        bool ok = false; int up = 0, down = 0, conf = 0;
        s.syncNow([&](bool o, int u, int d, int c) { ok = o; up = u; down = d; conf = c; });
        CHECK(ok && up == 1 && down == 1 && conf == 0, "a local-only save goes up and a cloud-only one comes down");
        CHECK(readFile(root + QStringLiteral("/") + THEIRS) == "theirs", "the downloaded save is on disk");
        CHECK(blobOf(cloud, MINE) == "mine", "the uploaded save is in the cloud");
        const QHash<QString, Entry> base = s.readBaseline();
        CHECK(base.value(MINE).sha == shaOf("mine") && base.value(THEIRS).sha == shaOf("theirs"),
              "the baseline now describes both sides");
        CHECK(!s.firstRun(), "…so the next run is not a firstRun");

        // Running again with nothing changed must be a no-op, not a re-upload of everything.
        cloud.calls.clear();
        s.syncNow([&](bool o, int u, int d, int c) { ok = o; up = u; down = d; conf = c; });
        CHECK(ok && up == 0 && down == 0 && conf == 0
                  && firstBlobUpload(cloud.calls) < 0 && firstBlobDownload(cloud.calls) < 0
                  && firstCall(cloud.calls, QStringLiteral("up:") + kIdx) < 0,
              "a second pass with nothing changed moves nothing and does not republish the index");
    }

    // ---- 8j. the TORN-WRITE guard: a save that changes between the two hashes is not published ----
    // Previously invisible: the old fake replaced uploadFrom outright, so this guard was asserted nowhere.
    // The window is microseconds wide, hence midReadHook_ — the mutation that matters (dropping the second
    // hash) makes this case upload half a file under the real name, which is what the next device would read
    // as the truth.
    {
        const QString root = freshRoot(QStringLiteral("torn"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        s.setMidReadHook([](const QString& p) { writeFile(p, "TORN-HALF-WRITTEN-BYTES-DIFFERENT"); });

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a save that changed while it was being read fails the sync");
        CHECK(!cloud.files.contains(SaveSync::driveNameFor(N)), "…and no torn copy reaches the cloud");
        CHECK(!cloud.files.contains(kIdx), "…nor does the index claim one");
        CHECK(!s.readBaseline().contains(N), "…and the baseline does not record it as synced");
    }

    // ---- 8k. listOk == false on a SAVE's lookup aborts that upload (an empty id would CREATE a twin) ----
    {
        const QString root = freshRoot(QStringLiteral("blobfindfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        cloud.failFind.insert(SaveSync::driveNameFor(N));

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "a save we could not look up fails the sync");
        CHECK(firstBlobUpload(cloud.calls) < 0,
              "…and is not uploaded blind: an empty existingId CREATES a second file with the same name");
        CHECK(!s.readBaseline().contains(N), "…and is not recorded as synced");
    }

    // ---- 8l. listOk == false at PUBLISH time leaves the index alone and the baseline unadvanced ----
    {
        const QString root = freshRoot(QStringLiteral("putfindfail"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        int idxFinds = 0;
        cloud.hook = [&](const QString& op, const QString& name) {
            if (op == QLatin1String("find") && name == kIdx && ++idxFinds == 2) cloud.failFind.insert(kIdx);
        };

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "an index lookup that failed at publish time fails the sync");
        CHECK(!cloud.files.contains(kIdx), "…and never creates a SECOND saves-index.json");
        CHECK(s.firstRun(), "…and does not advance the baseline");
    }

    // ---- 8m. THE COMPARE-AND-SWAP: publishing never overwrites an index another device moved ----
    // Two devices, one cloud. B publishes between A's read of the index and A's own publish. A blind
    // whole-document overwrite drops B's row while A's baseline records success: the index then permanently
    // disagrees with the blobs, both devices believe they are in sync, and neither ever converges.
    {
        const QString rootA = freshRoot(QStringLiteral("casA"));
        const QString rootB = freshRoot(QStringLiteral("casB"));
        const QString X = QStringLiteral("saves/Shared.srm");
        const QString Y = QStringLiteral("saves/OnlyB.srm");
        FakeCloud cloud;
        TestSync a(&cloud, rootA, QStringLiteral("devA"));
        TestSync b(&cloud, rootB, QStringLiteral("devB"));

        // Get both devices in step on X, through the real code.
        writeFile(rootA + QStringLiteral("/") + X, "X0");
        bool ok = false;
        a.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok, "the shared save is published by A");
        b.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && readFile(rootB + QStringLiteral("/") + X) == "X0", "…and picked up by B");

        // Now each device has an edit, and B's lands first.
        writeFile(rootA + QStringLiteral("/") + X, "X-A");
        writeFile(rootB + QStringLiteral("/") + Y, "Y-B");

        QStringList aLog;
        QObject::connect(&a, &SaveSync::log, &a, [&](const QString& l) { aLog << l; });

        int idxFinds = 0;
        bool fired = false;
        cloud.hook = [&](const QString& op, const QString& name) {
            // A's publish-time lookup is the second index find of its run: let B publish in that window.
            if (fired || op != QLatin1String("find") || name != kIdx) return;
            if (++idxFinds != 2) return;
            fired = true;
            bool bok = false;
            b.syncNow([&](bool o, int, int, int) { bok = o; });
            CHECK(bok, "B publishes while A is mid-run");
        };

        bool aok = false;
        a.syncNow([&](bool o, int, int, int) { aok = o; });
        cloud.hook = nullptr;

        CHECK(fired, "the interleave actually happened");
        CHECK(aok, "A recovers by reconciling again rather than failing");
        bool retried = false;
        for (const QString& l : aLog) if (l.contains(QStringLiteral("another device published first"))) retried = true;
        CHECK(retried, "A detected that the index had moved and did NOT overwrite it");

        const QHash<QString, Entry> idx = cloudIndex(cloud);
        CHECK(idx.value(Y).sha == shaOf("Y-B"),
              "B's row survives A's publish — a blind overwrite is silent, permanent divergence");
        CHECK(idx.value(X).sha == shaOf("X-A"), "…and A's own change is published");
        CHECK(blobOf(cloud, X) == "X-A" && idx.value(X).sha == shaOf(blobOf(cloud, X)),
              "the index and the blob agree — the state the divergence bug makes unreachable");
        CHECK(a.readBaseline().value(X).sha == shaOf("X-A"), "A's baseline records what it published");
    }

    // ---- 8s. the same compare-and-swap on a SAVE's own bytes ----
    // The index told us this save's hash and the plan was made against THAT. A blob carrying anything else
    // was written by another device since, so uploading over it skips the conflict rule for exactly the pair
    // the rule exists for — the loser's only copy is destroyed and nothing records that it happened.
    {
        const QString root = freshRoot(QStringLiteral("blobcas"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        const Entry row = remoteRow(N, REMOTE, kHour, QStringLiteral("devB"));
        seedIndex(cloud, one(row));
        QHash<QString, Entry> base; base.insert(N, row);
        CHECK(s.writeBaseline(base), "the baseline matches the index, so this is a plain Upload");
        seedBlob(cloud, N, "ANOTHER-DEVICE-WROTE-THIS");   // …but the blob has moved on

        bool ok = true;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(!ok, "an upload onto a blob the index no longer describes fails the sync");
        CHECK(blobOf(cloud, N) == "ANOTHER-DEVICE-WROTE-THIS", "…and the other device's bytes survive");
        CHECK(s.readBaseline().value(N).sha == row.sha, "…and the baseline is not advanced over it");
    }

    // ---- 8s2. …but a blob that ALREADY holds what we are sending is our own retry, not a stranger ----
    // Without this exception a run whose index publish failed can never upload that save again: the blob is
    // permanently "ahead of" the index and every later pass refuses it.
    {
        const QString root = freshRoot(QStringLiteral("blobcasretry"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        const Entry row = remoteRow(N, REMOTE, kHour, QStringLiteral("devB"));
        seedIndex(cloud, one(row));
        QHash<QString, Entry> base; base.insert(N, row);
        CHECK(s.writeBaseline(base), "the baseline matches the index");
        seedBlob(cloud, N, LOCAL);          // a previous pass got the bytes up and then failed to publish

        bool ok = false;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok, "re-sending bytes the cloud already holds is allowed");
        CHECK(cloudIndex(cloud).value(N).sha == shaOf(LOCAL), "…so the index finally catches up with the blob");
    }

    // ---- 8n. a delete reaches the OTHER device — and stays deleted on the one that made it ----
    // Tombstones used to be local-only, so the peer saw a file missing from the index, re-uploaded it, and
    // the delete was undone everywhere including on the deleting machine.
    {
        const QString rootA = freshRoot(QStringLiteral("tombA"));
        const QString rootB = freshRoot(QStringLiteral("tombB"));
        const QString T = QStringLiteral("saves/Doomed.srm");
        FakeCloud cloud;
        TestSync a(&cloud, rootA, QStringLiteral("devA"));
        TestSync b(&cloud, rootB, QStringLiteral("devB"));

        writeFile(rootA + QStringLiteral("/") + T, "doomed");
        bool ok = false;
        a.syncNow([&](bool o, int, int, int) { ok = o; });
        b.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && readFile(rootB + QStringLiteral("/") + T) == "doomed", "both devices hold the save");

        clearTombs();
        CHECK(QFile::remove(rootA + QStringLiteral("/") + T), "the user deletes it on A");
        a.recordDelete(T);
        a.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && !cloudIndex(cloud).contains(T), "A's delete removes the row from the index");
        CHECK(indexHasTomb(cloud, T), "…and PUBLISHES the tombstone, which is what makes it a delete at all");

        // B is a different machine: it has none of A's local tombstone state, only the index.
        clearTombs();
        cloud.calls.clear();
        b.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok, "B's sync succeeds");
        CHECK(!QFileInfo::exists(rootB + QStringLiteral("/") + T),
              "B applies the delete instead of re-uploading the save");
        CHECK(firstBlobUpload(cloud.calls) < 0, "…so nothing is pushed back up");
        CHECK(!cloudIndex(cloud).contains(T), "…and the index still says it is gone");
        bool imported = false;
        for (const Tombstones::Entry& t : Tombstones::all(kTombStore)) if (t.key == T) imported = true;
        CHECK(imported, "…and B imported the tombstone, so it re-propagates to a third device");

        // And the deleting device does not get its own delete undone on the next pass.
        a.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && !QFileInfo::exists(rootA + QStringLiteral("/") + T),
              "A's next sync does not restore the save it deleted");
    }

    // ---- 8t. a published tombstone NEWER than the row it names deletes, and an older one does not ----
    // The row and the tombstone can both be in the index at once (a peer's removal that lost a publish race,
    // an older client). Whichever is newer decides — the same comparison Tombstones/CloudMerge use for the
    // progress document: ts >= the entry's own timestamp suppresses it, a strictly newer re-add beats it.
    {
        const QString root = freshRoot(QStringLiteral("stalerow"));
        const QString T = QStringLiteral("saves/Stale.srm");
        writeFile(root + QStringLiteral("/") + T, "bytes");
        FakeCloud cloud;
        TestSync s(&cloud, root);
        const Entry row = remoteRow(T, "bytes", kHour, QStringLiteral("devB"));   // the row is an hour old
        seedIndex(cloud, one(row), { SaveSync::Tomb{ T, QDateTime::currentSecsSinceEpoch() } });  // delete is newer
        seedBlob(cloud, T, "bytes");
        QHash<QString, Entry> base; base.insert(T, row);
        CHECK(s.writeBaseline(base), "this device is in step with the row");

        bool ok = false;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok, "the pass succeeds");
        CHECK(!QFileInfo::exists(root + QStringLiteral("/") + T),
              "a row a published tombstone out-dates is a delete, not a file to keep in step with");
        CHECK(!cloudIndex(cloud).contains(T), "…and the stale row leaves the index");
    }
    {
        const QString root = freshRoot(QStringLiteral("readd"));
        const QString T = QStringLiteral("saves/Stale.srm");
        writeFile(root + QStringLiteral("/") + T, "bytes");
        FakeCloud cloud;
        TestSync s(&cloud, root);
        const Entry row = remoteRow(T, "bytes", 0, QStringLiteral("devB"));       // written just now
        seedIndex(cloud, one(row), { SaveSync::Tomb{ T, QDateTime::currentSecsSinceEpoch() - 3600 } });
        seedBlob(cloud, T, "bytes");
        QHash<QString, Entry> base; base.insert(T, row);
        CHECK(s.writeBaseline(base), "…and in step with this one too");

        bool ok = false;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && QFileInfo::exists(root + QStringLiteral("/") + T),
              "a save re-added AFTER the delete beats the tombstone — update never loses to a stale delete");
        CHECK(cloudIndex(cloud).contains(T), "…and keeps its row");
    }

    // ---- 8o. a ROM name with an apostrophe survives the Drive query ----
    {
        // The value goes INSIDE a quoted Drive `q=` literal, so both characters have to be escaped there.
        CHECK(CloudSync::driveQueryQuote(QStringLiteral("Link's Awakening")) ==
                  QStringLiteral("Link\\'s Awakening"), "an apostrophe is escaped for the Drive query");
        CHECK(CloudSync::driveQueryQuote(QStringLiteral("a\\b's")) == QStringLiteral("a\\\\b\\'s"),
              "…and a backslash is escaped FIRST, so the escapes are not re-escaped");
        CHECK(CloudSync::driveQueryQuote(QStringLiteral("plain.srm")) == QStringLiteral("plain.srm"),
              "…while an ordinary name is untouched");

        const QString APOS = QStringLiteral("saves/Link's Awakening.srm");
        CHECK(!SaveSync::driveNameFor(APOS).contains(QLatin1Char('\'')) &&
                  !SaveSync::driveNameFor(APOS).contains(QLatin1Char('/')),
              "a save's Drive name is percent-encoded, so neither '/' nor an apostrophe reaches the query");

        const QString rootA = freshRoot(QStringLiteral("aposA"));
        const QString rootB = freshRoot(QStringLiteral("aposB"));
        FakeCloud cloud;
        TestSync a(&cloud, rootA, QStringLiteral("devA"));
        TestSync b(&cloud, rootB, QStringLiteral("devB"));
        writeFile(rootA + QStringLiteral("/") + APOS, "zelda");
        bool ok = false;
        a.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && blobOf(cloud, APOS) == "zelda", "a save named after a ROM with an apostrophe uploads");
        b.syncNow([&](bool o, int, int, int) { ok = o; });
        CHECK(ok && readFile(rootB + QStringLiteral("/") + APOS) == "zelda", "…and comes back down by name");
    }

    // ---- 8p. a Drive reply that lands after this object is gone touches nothing ----
    // The intended wiring is flush() at exit inside the shutdown watchdog, so a sync in flight while SaveSync
    // is destroyed is the NORMAL case: the callbacks belong to CloudSync's replies, not to us.
    {
        const QString root = freshRoot(QStringLiteral("lifetime"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        cloud.deferFind.insert(kIdx);
        auto* s = new TestSync(&cloud, root);

        bool ran = false;
        s->syncNow([&](bool, int, int, int) { ran = true; });
        CHECK(!ran && cloud.pending, "the run is parked waiting on Drive");
        const int before = cloud.calls.size();
        delete s;                     // …and the app quits
        cloud.pending();              // Drive answers a dead SaveSync
        CHECK(!ran, "a reply after destruction runs no callback");
        CHECK(cloud.calls.size() == before, "…and issues no further Drive calls");
    }

    // ---- 8q. a save written DURING its own push is not dropped from the fast path ----
    // F2, debounce, upload in flight, F2 again: markDirty re-inserts the name and finish() used to remove it
    // unconditionally, so the next timer fire found an empty set and flush() at shutdown reported success
    // having pushed nothing.
    {
        const QString root = freshRoot(QStringLiteral("dirtyrace"));
        writeFile(root + QStringLiteral("/") + N, "SAVE-1");
        FakeCloud cloud;
        TestSync s(&cloud, root);
        s.markDirty(N);

        // The second F2 lands after the blob went up but before the run finishes (the index publish is the
        // last thing a run does).
        bool again = false;
        cloud.hook = [&](const QString& op, const QString& name) {
            if (again || op != QLatin1String("up") || name != kIdx) return;
            again = true;
            writeFile(root + QStringLiteral("/") + N, "SAVE-2");
            s.markDirty(N);
        };

        bool ok = false;
        s.flush([&](bool o) { ok = o; });
        cloud.hook = nullptr;
        CHECK(ok && again && blobOf(cloud, N) == "SAVE-1", "the first push lands");

        bool ok2 = false;
        s.flush([&](bool o) { ok2 = o; });
        CHECK(ok2 && blobOf(cloud, N) == "SAVE-2",
              "the save written mid-flight is still pending, so the next push sends it");
        CHECK(cloudIndex(cloud).value(N).sha == shaOf("SAVE-2"), "…and the index agrees with the blob");

        // Nothing left over: a name whose bytes DID reach the cloud is retired, so flush() at exit is cheap.
        cloud.calls.clear();
        bool ok3 = false;
        s.flush([&](bool o) { ok3 = o; });
        CHECK(ok3 && cloud.calls.isEmpty(), "…and a name that is genuinely in sync stops being pushed");
    }

    // ---- 8r. a baseline we could not parse is said out loud and never silently believed ----
    {
        const QString root = freshRoot(QStringLiteral("corruptbase"));
        writeFile(root + QStringLiteral("/") + N, LOCAL);
        FakeCloud cloud;
        TestSync s(&cloud, root);
        writeFile(root + QStringLiteral("/save-baseline.json"), "{not json at all");
        CHECK(!s.firstRun(), "the corrupt baseline EXISTS, so firstRun() alone will not save us");

        QStringList lines;
        QObject::connect(&s, &SaveSync::log, &s, [&](const QString& l) { lines << l; });
        bool ok = false;
        s.syncNow([&](bool o, int, int, int) { ok = o; });
        bool said = false;
        for (const QString& l : lines) if (l.contains(QStringLiteral("unreadable"))) said = true;
        CHECK(said, "an unparseable baseline is logged, not read as 'nothing has ever happened'");
        CHECK(ok, "…the pass still completes");
        CHECK(s.readBaseline().value(N).sha == shaOf(LOCAL), "…and leaves a baseline that parses again");
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

    // ------------------------------------------------- 8. the TRANSPORT: ordering, the compare-and-swap,
    //                                                       published tombstones, and a baseline that only
    //                                                       ever records what actually happened.
    //
    // None of these can be asserted against a live Google account — so the cloud is substituted at
    // CloudSync's four primitives and everything above them (plan, execute, conflict ordering, the index
    // CAS, the torn-write guard, the baseline write) is the real code running against real files.
    {
        transportChecks();
    }

    clearTombs();
    cleanupRoots();
    if (failures) { std::fprintf(stderr, "SAVESYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SAVESYNC-OK\n");
    return 0;
}
