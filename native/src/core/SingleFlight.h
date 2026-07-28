// One in-flight async operation, many waiters.
//
// Written for TraktClient::ensureValidToken, where a duplicate operation is not merely wasteful but
// DESTRUCTIVE: Trakt ROTATES the refresh token, so two overlapping POSTs to /oauth/token present the
// same refresh token, the second one presents a token the first already consumed, and if the replies
// interleave the later write can put the older token pair over the newer one. That is a permanently
// broken account link, not a failed call. The rule is therefore "exactly one refresh in flight, every
// caller gets the answer", which is what this holds.
//
// Deliberately a plain, Qt-free, header-only value: no signals, no timers, no network. That is what
// makes the queue's awkward cases (a second caller arriving mid-flight, a waiter enqueued from inside
// the fan-out, a settle with nobody waiting) reachable from a headless probe with no socket at all —
// see probe_trakt §12. The queue is single-threaded by construction: every user of it lives on the Qt
// event loop, where join() and settle() can only ever run one at a time.
#pragma once
#include <functional>
#include <utility>
#include <vector>

class SingleFlight
{
public:
    using Waiter = std::function<void(bool ok)>;

    // Enqueue `w` and say who starts the work: TRUE means this caller is the first in and MUST start
    // the operation, FALSE means one is already in flight and `w` simply joined it. A false return is
    // not a failure — the waiter is queued either way and will be called from settle().
    bool join(Waiter w)
    {
        waiters_.push_back(std::move(w));
        if (inFlight_) return false;
        inFlight_ = true;
        return true;
    }

    // Finish the operation and fan `ok` out to everyone waiting on it, each exactly once.
    //
    // The three steps are ORDERED, and each order matters:
    //   1. take the batch, so the set being called is fixed before any callback can run;
    //   2. clear the flag, so a waiter that re-enters join() from inside the fan-out is told to start
    //      a FRESH operation rather than joining one that has already settled — otherwise it queues
    //      behind nothing and is never called at all;
    //   3. only then call them. A waiter enqueued during step 3 lands in the now-empty queue and
    //      belongs to that fresh operation, so it is neither dropped nor called twice by this drain.
    // Draining in place instead (iterate waiters_, clear afterwards) does both wrong at once: it calls
    // the re-entrant waiter here AND leaves it queued for the operation it just started.
    //
    // Calling settle() when nothing is in flight is a no-op with nobody to call — harmless, so a
    // paranoid double-settle on one reply cannot double-call anyone.
    void settle(bool ok)
    {
        std::vector<Waiter> batch;
        batch.swap(waiters_);
        inFlight_ = false;
        for (Waiter& w : batch)
            if (w) w(ok);
    }

    bool inFlight() const { return inFlight_; }
    int  waiting() const { return static_cast<int>(waiters_.size()); }

private:
    std::vector<Waiter> waiters_;
    bool inFlight_ = false;
};
