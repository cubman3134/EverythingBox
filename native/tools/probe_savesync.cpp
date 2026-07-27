// Headless coverage for the save-sync decision table. Pure — no network, no Drive, no files on disk.
// Prints SAVESYNC-OK on success; any failure prints SAVESYNC-FAIL <what> and exits non-zero.
#include "SaveSyncPlan.h"

#include <QCoreApplication>
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
        // Inside the skew window the mtimes are a tie, broken deterministically on sha then deviceId.
        Entry L = e(N, "aaa", 10000, QStringLiteral("devA"));
        Entry R = e(N, "bbb", 10000 + kSkewWindowMs - 1, QStringLiteral("devB"));
        const QVector<Decision> ds1 = plan(one(L), one(R), one(e(N, "base")), {}, false);
        // Now compute the SAME conflict from the other device's point of view: its local is what was remote.
        const QVector<Decision> ds2 = plan(one(R), one(L), one(e(N, "base")), {}, false);
        const Decision* d1 = decFor(ds1, N);
        const Decision* d2 = decFor(ds2, N);
        CHECK(d1 && d2 && d1->act == Act::Conflict && d2->act == Act::Conflict, "a near-tie is still a conflict");
        // If A says "local wins" then B, looking at the same pair, must say "local LOSES" — i.e. the two
        // devices must pick the SAME physical copy. Disagreement here means both keep their own and diverge
        // forever, which is worse than having no rule at all.
        CHECK(d1 && d2 && d1->localWins != d2->localWins, "both devices independently choose the same winner");

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
    }

    if (failures) { std::fprintf(stderr, "SAVESYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SAVESYNC-OK\n");
    return 0;
}
