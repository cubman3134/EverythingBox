// Headless probe for LaunchContexts — the per-launch continuation contexts behind GameLauncher's launch
// supersession, and the rules for which of them a newly committed launch retires. Prints LAUNCHCONTEXTS-OK on
// success. No display, no network, no process spawns: the unit is QObject parent/child lifetime plus Qt's own
// receiver-bound connection drop, so a QCoreApplication event loop is the entire fixture.
//
// The bug this unit exists to prevent has two halves, and both are asserted here:
//
//   * A pending in-app continuation (archive extraction, core/BIOS download) that survives an external-emulator
//     launch boots a stale game on top of the emulator minutes later — supersession running as "whichever
//     finishes first wins" instead of "newest wins".
//   * Retiring the extraction context at that commit point can free the object owning the lambda currently on
//     the stack, because the commit point is reachable from inside that very continuation. The consume-on-entry
//     protocol is what makes the retire safe, so the probe drives the retire from INSIDE a continuation and
//     asserts the caller survives it.
#include "launch/LaunchContexts.h"

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #c, __LINE__); ++g_fail; } } while (0)

// A stand-in for the thing whose completion a launch continuation hangs off: the extraction worker's
// QThread::finished, CoreManager's download-complete callback. All the probe needs of it is one signal it can
// fire on demand.
class Trigger : public QObject
{
    Q_OBJECT
signals:
    void fire();
};

// Bind a continuation to `ctx` exactly the way GameLauncher does — receiver-context connect, so destroying the
// context makes Qt drop it. `ran` records whether it was allowed to run.
static void bind(Trigger* t, QObject* ctx, bool* ran, Qt::ConnectionType type = Qt::AutoConnection)
{
    QObject::connect(t, &Trigger::fire, ctx, [ran] { *ran = true; }, type);
}

// §1 The baseline: a live context delivers its continuation, a retired one does not. Without this pair the
// later "did not run" assertions would also pass against a unit that never delivers anything at all.
static void testRetireDropsPendingContinuation()
{
    QObject owner;
    LaunchContexts lc(&owner);
    Trigger t;

    bool ranA = false;
    bind(&t, lc.beginOpen(), &ranA);
    emit t.fire();
    CHECK(ranA);                       // live context: the continuation runs

    bool ranB = false;
    bind(&t, lc.beginOpen(), &ranB);
    lc.beginOpen();                    // a newer open() supersedes it
    emit t.fire();
    CHECK(!ranB);                      // retired context: dropped

    // Same rule on the core-fetch slot, and the two slots are independent — retiring one must not drop the
    // other, or a plain non-archive launch would cancel its own core download.
    bool ranExtract = false, ranCore = false;
    bind(&t, lc.beginOpen(), &ranExtract);
    bind(&t, lc.beginCoreFetch(), &ranCore);
    lc.beginCoreFetch();               // retire ONLY the core-fetch slot
    emit t.fire();
    CHECK(ranExtract);
    CHECK(!ranCore);
}

// §2 The fix itself: committing an external-emulator launch drops BOTH in-app continuations. This is the
// assertion that goes red if runEmulator's retire is removed — the wrong-winner bug.
static void testExternalLaunchRetiresBoth()
{
    QObject owner;
    LaunchContexts lc(&owner);
    Trigger t;

    bool ranExtract = false, ranCore = false;
    bind(&t, lc.beginOpen(), &ranExtract);
    bind(&t, lc.beginCoreFetch(), &ranCore);

    lc.retireForExternalLaunch();

    emit t.fire();
    CHECK(!ranExtract);                // a pending archive extraction cannot boot over the emulator
    CHECK(!ranCore);                   // nor can a pending core/BIOS download

    // The slots stay usable afterwards: the next launch gets live contexts, not dead ones.
    bool ranAfter = false;
    bind(&t, lc.beginOpen(), &ranAfter);
    emit t.fire();
    CHECK(ranAfter);
}

// §3 The retire must be IMMEDIATE, not deferred. A deferred retire would leave a window in which the worker
// finishes and its already-queued delivery lands before the retire runs — the very race being closed. Queue a
// delivery, retire, then let the event loop turn: the delivery must be gone. (~QObject removes posted metacall
// events for the receiver; a deleteLater-based retire would not, since the object outlives the turn.)
static void testRetireDropsAlreadyQueuedDelivery()
{
    QObject owner;
    LaunchContexts lc(&owner);
    Trigger t;

    bool ran = false;
    bind(&t, lc.beginOpen(), &ran, Qt::QueuedConnection);
    emit t.fire();                     // posts the delivery; it has NOT run yet
    CHECK(!ran);

    lc.retireForExternalLaunch();
    QCoreApplication::processEvents();
    CHECK(!ran);                       // the queued delivery died with its receiver

    // Control: without the retire the same queued delivery does arrive, so the check above is about the retire
    // and not about the delivery never being posted.
    bool ranControl = false;
    bind(&t, lc.beginOpen(), &ranControl, Qt::QueuedConnection);
    emit t.fire();
    QCoreApplication::processEvents();
    CHECK(ranControl);
}

// §4 + §5 The trap. runEmulator is reachable from inside the extraction continuation (an archived game on a
// standalone emulator), so the continuation consumes its own context on entry and the retire that follows must
// be a no-op on that slot. Driven exactly that way: the continuation consumes, then retires, then keeps
// running and touching its own captures.
static void testConsumeOnEntryMakesTheRetireSafe()
{
    QObject owner;
    LaunchContexts lc(&owner);
    Trigger t;

    QObject* ectx = lc.beginOpen();
    QPointer<QObject> alive(ectx);     // watch the context object across the call
    bool reachedEnd = false;
    int captured = 0x5eed;             // a capture the lambda reads AFTER the retire

    QObject::connect(&t, &Trigger::fire, ectx, [&lc, ectx, &alive, &reachedEnd, &captured] {
        lc.takeExtractContext(ectx);       // continuation entry: the context has no queued work left to protect
        CHECK(alive);                      // consumed, not destroyed — this lambda is still running out of it
        lc.retireForExternalLaunch();      // the tail commits an external launch from inside this continuation
        CHECK(alive);                      // ...and that retire did NOT free the owner of this frame
        CHECK(captured == 0x5eed);         // the captures are intact, so the lambda's storage is intact
        reachedEnd = true;
    });

    emit t.fire();
    CHECK(reachedEnd);

    // The consumed context is destroyed on a later event-loop turn, not during the call: deferred, so the
    // running lambda outlives it, but still destroyed, so it is not leaked into the owner's child list.
    // sendPostedEvents rather than processEvents, because a DeferredDelete posted at loop level 0 — which is
    // where a probe's main() sits — is delivered only when it is asked for by name. The app never sees that
    // distinction: GameLauncher always runs under a live event loop.
    CHECK(alive);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!alive);
}

// §6 A context that a newer open() already retired is not ours to touch. takeExtractContext is handed a stale
// pointer only in impossible-in-practice orderings, but if it ever nulled the slot on a mismatch it would strand
// the CURRENT launch's context — unretirable, so the next external launch would not drop it.
static void testStaleTakeIsANoOp()
{
    QObject owner;
    LaunchContexts lc(&owner);
    Trigger t;

    // A live object that is NOT the slot's — stands in for a context a newer open() already superseded. Live
    // rather than the actual retired pointer, because the retired one is freed and its address can be handed
    // straight back to the replacement, which would make the mismatch the probe is testing disappear.
    QObject notOurs;

    QObject* current = lc.beginOpen();
    CHECK(&notOurs != current);

    bool ran = false;
    bind(&t, current, &ran);
    lc.takeExtractContext(&notOurs);        // must not detach `current`
    lc.takeExtractContext(nullptr);         // nor must a null

    lc.retireForExternalLaunch();
    emit t.fire();
    CHECK(!ran);                            // `current` was still the slot's, so the retire really dropped it
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testRetireDropsPendingContinuation();
    testExternalLaunchRetiresBoth();
    testRetireDropsAlreadyQueuedDelivery();
    testConsumeOnEntryMakesTheRetireSafe();
    testStaleTakeIsANoOp();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("LAUNCHCONTEXTS-OK\n");
    return 0;
}

#include "probe_launchcontexts.moc"
