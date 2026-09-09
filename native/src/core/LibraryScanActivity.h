// "Is a library scan running right now?" — one process-wide counter, for the things that must not fight a
// scan for the disk (issue #302's idle preview walk is the first).
//
// It is a COUNTER and not a flag because there are four scans (video, music, audiobooks, books), they are
// started independently, and a rescan can be superseded by a newer one while the older future is still
// running — so two of the same kind can genuinely be in flight at once. A flag would be cleared by whichever
// finished first and would then claim the machine was quiet with a directory walk still going.
//
// Atomic because the counter is incremented on the GUI thread and read from wherever the interested party
// happens to be; the scans themselves run on the global thread pool.
//
// It is deliberately NOT "is any background work running". That would be QThreadPool::activeThreadCount(),
// which is a wider and vaguer fact — it counts one-off tasks nobody would call a scan — and a predicate whose
// input is named `scanning` should mean scanning.
#pragma once
#include <QAtomicInt>

namespace LibraryScanActivity
{
inline QAtomicInt& counter()
{
    static QAtomicInt n{ 0 };
    return n;
}

// Call at the point a scan's future is handed to the pool, and exactly once again from its finished handler —
// including the handler of a scan that was superseded, which still finishes.
inline void begin() { counter().fetchAndAddOrdered(1); }
inline void end()   { if (counter().loadAcquire() > 0) counter().fetchAndAddOrdered(-1); }

inline bool running() { return counter().loadAcquire() > 0; }
} // namespace LibraryScanActivity
