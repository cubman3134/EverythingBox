// The per-launch context objects GameLauncher parents its asynchronous launch continuations to, and the
// protocol deciding which of them a newly committed launch retires. Pulled out of GameLauncher because that
// class needs a window, a RetroView and a RetroParkView to exist, and this protocol is the part that has to be
// asserted on (house rule: a new component that can be tested without a window gets a probe).
//
// GameLauncher starts work that finishes minutes later — an archive extraction on a worker thread, a core
// download, a BIOS fetch. Each continuation is bound to a context QObject, so destroying that object makes Qt
// drop the continuation instead of letting it boot a game the user has since moved on from. Two contexts and
// not one, because the extraction continuation is what CALLS the code that retires the core-fetch context:
// sharing a single object would make the continuation delete its own owner.
//
// One rule governs the slots: RETIRING A SLOT DESTROYS ITS OBJECT AND INSTALLS A FRESH ONE. Destroying it is
// what drops the pending continuations — including any delivery already queued behind it, which ~QObject
// removes from the event queue. There is exactly one exception to the rule, takeExtractContext, and it exists
// because of the trap below.
//
// THE TRAP. runEmulator — the commit point of an external-emulator launch — must retire both contexts, or an
// in-app launch that finishes later boots on top of the emulator and supersession becomes "whichever finishes
// first wins" instead of "newest wins". But runEmulator is REACHABLE FROM INSIDE the extraction continuation:
//
//     open(archived external game) -> extraction worker finishes -> continuation -> openResolved
//         -> launchExternalGame -> runEmulator
//
// so a plain `delete extract_` there would free the object owning the lambda currently on the stack — the
// crash-#28 / deferPastQmlEmission class. It would also be pointless: open()'s top already retired the PREVIOUS
// extraction, so on that path the context holds nothing stale. Hence consume-on-entry: takeExtractContext nulls
// the slot at the top of the continuation and defers the object's own destruction past the running lambda,
// after which the retire in runEmulator is unconditionally safe — a no-op inside the continuation, a real
// immediate drop everywhere else. Immediate and not deferred, because a deferred retire leaves a window in
// which the extraction worker finishes and its already-queued delivery lands first, which is the very race
// being closed.
#pragma once
#include <QObject>

class LaunchContexts
{
public:
    // `owner` parents every context object, so an owner destroyed mid-launch takes its pending continuations
    // with it and no continuation ever runs against a freed GameLauncher.
    explicit LaunchContexts(QObject* owner) : owner_(owner) {}

    LaunchContexts(const LaunchContexts&) = delete;
    LaunchContexts& operator=(const LaunchContexts&) = delete;

    // open()'s top. A new launch of ANY kind supersedes a prior archive extraction — a plain non-archive launch
    // included, since it too must stop a previously started archive from booting on top of it once it unpacks.
    QObject* beginOpen() { return retire(extract_); }

    // openResolved's libretro tail. A newer launch supersedes a still-downloading core/BIOS chain instead of
    // letting both of them boot when their downloads land.
    QObject* beginCoreFetch() { return retire(launch_); }

    // The live core-fetch context, for chaining the BIOS fetch onto the same launch from inside the core-ready
    // continuation. Only ever read from there, where the context is by definition still current — a retired one
    // would have dropped that continuation before it could ask.
    QObject* coreFetchContext() const { return launch_; }

    // Entry of the extraction continuation: it has fired, so its context no longer has queued work to protect.
    // Detach it, so a supersession raised from inside this very call chain finds nothing of ours to retire, and
    // destroy it with deleteLater rather than delete — it still owns the lambda that is running. A ctx that is
    // not the current one was already retired by a newer open() and is not ours to touch.
    void takeExtractContext(QObject* ctx)
    {
        if (!ctx || extract_ != ctx) return;
        extract_ = nullptr;
        ctx->deleteLater();
    }

    // runEmulator's commit point: an external emulator is about to own the screen, so every in-app launch
    // continuation still in flight is dropped. This is the half of supersession that was missing — the in-app
    // launch tails already cancel a pending external launch (GameLauncher::cancelPendingEmulatorLaunch).
    void retireForExternalLaunch() { retire(extract_); retire(launch_); }

private:
    QObject* retire(QObject*& slot)
    {
        delete slot;                     // drops this slot's pending continuations and any queued delivery
        slot = new QObject(owner_);
        return slot;
    }

    QObject* owner_ = nullptr;
    QObject* extract_ = nullptr;   // the archive-extraction worker's continuation
    QObject* launch_ = nullptr;    // the core download + BIOS fetch chain
};
