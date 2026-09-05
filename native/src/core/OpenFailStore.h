// THE LAST FAILED OPEN, PER ITEM (issue #239) — the state that outlives the toast.
//
// A press that ends without opening anything used to leave a toast and nothing else. The toast fades in a
// few seconds, and a moment later the shelf looks exactly as it did before the press: no message, no mark,
// no trace. That is not a theoretical complaint — it is how #236 came to be filed against the wrong
// subsystem ("the shelf's Play never reaches openGame", with the emulator backend suspected) when the real
// event was a download that came back empty and said so for four seconds.
//
// So a failure that ends a press is written down here, against the ITEM it was about, and stays until the
// user acts on it. MainWindow::reportOpenFailure is the one funnel that writes it; the item's detail page
// reads it (message, when, and the verbs that make sense) and the shelf row reads the predicate.
//
// DEVICE-LOCAL, PER PROFILE, AND DELIBERATELY NOT SYNCED. A failed open is a fact about THIS device's last
// attempt — this network, this source, this moment. The same title on the same account may well open on the
// tablet in the next room, and syncing this would put one device's dead link on another device's shelf with
// a "Try again" that has nothing to retry. CloudSync::isDeviceLocalKey therefore names "openfail/"
// explicitly (with that reason beside it), which is what keeps it out of both the state bundle and the
// CloudMerge progress document. probe_openfail §5 byte-scans the synced categories for the prefix.
//
// SEVEN DAYS. The record answers "did the thing you pressed last do anything?", and after a week it is no
// longer answering that question about anything the user remembers pressing — it is just an old red mark on
// a shelf. Expiry is READ-SIDE (an expired entry is invisible to every accessor even before the file is
// rewritten), so a stale row can never surface through a path that forgot to purge first.
//
// THE CLOCK IS INJECTED. Every accessor takes `now` (unix seconds), and 0 means "ask the wall clock". A test
// that had to sleep seven days would not be written, and a store whose expiry is only ever exercised by the
// wall clock is a store whose expiry is never exercised at all.
//
// NOT THREAD-SAFE: every writer syncs the ini on the calling thread. GUI-thread use only, like RecentStore.
#pragma once
#include <QString>
#include <QVector>

struct OpenFailure
{
    QString id;       // the catalog item's id — the SAME identity the "on disk" badge and FollowStore key on
    QString title;    // what to call it in the report (display only; never the identity)
    QString message;  // the sentence the toast showed, kept verbatim so the page and the toast cannot differ
    qint64  ts = 0;   // when it failed (unix seconds)

    bool isNull() const { return id.isEmpty(); }
};

namespace OpenFailStore
{
    // Seven days. See the header note; changing it changes what §3 of probe_openfail asserts.
    constexpr qint64 kExpirySecs = 7 * 24 * 60 * 60;

    // Cap on stored rows. A failure is per-item and self-clearing, so this is a floor under pathological
    // growth (a source that is down for an evening while somebody walks a 900-row shelf), not a policy.
    constexpr int kMaxEntries = 100;

    // Write down that `id` would not open, replacing any earlier record for the same id (the newest attempt
    // is the news). An EMPTY id is a no-op: without an identity there is no page to carry the state and no
    // row to mark, and writing a title-keyed row instead is exactly the "a failure for one item marks
    // another" bug — two different releases of "Tetris" are two items.
    void record(const QString& id, const QString& title, const QString& message, qint64 now = 0);

    // Forget the record for `id`. The three occasions: the item opened successfully, the user dismissed it,
    // or a caller is standing in for one of those. Unknown ids are a no-op.
    void clear(const QString& id);

    // The record for `id`, or a null OpenFailure when there is none or the one there is has expired.
    OpenFailure lookup(const QString& id, qint64 now = 0);

    // The row marker's predicate. Same answer as `!lookup(id).isNull()`, named for what a shelf asks.
    bool marked(const QString& id, qint64 now = 0);

    // Every live (unexpired) record, newest first.
    QVector<OpenFailure> list(qint64 now = 0);

    // Drop this process's hot copy of the store.
    //
    // THERE IS A CACHE, and it is not an optimisation looking for a problem. The browse model asks marked()
    // once per row, so a 900-row console folder would otherwise re-read and re-parse the whole JSON blob 900
    // times on the GUI thread every time a shelf is drawn — the exact shape of the 2026-08-18 shelf-lag
    // fault. So the parsed rows are held, keyed by the profile-qualified ini key (a profile switch therefore
    // re-reads on its own), and every writer here drops them.
    //
    // The consequence is ItemMarks' consequence, stated in the same words: an EXTERNAL write to the ini is
    // not seen until this is called. Nothing in the app writes these rows except this store; a test that
    // plants one by hand has to say so.
    void invalidate();

    // Drop the expired rows from the file. Purely housekeeping — every reader above already ignores them —
    // so it is safe to never call it, and it returns how many it dropped for the probe to assert on.
    int purgeExpired(qint64 now = 0);
}
