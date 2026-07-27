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

    if (failures) { std::fprintf(stderr, "SAVESYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SAVESYNC-OK\n");
    return 0;
}
