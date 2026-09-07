// STORE-AND-FORWARD PLAYBACK PROGRESS FOR A JELLYFIN SERVER (issue #110, increment 1).
//
// Watching a downloaded episode on a plane produces exactly the progress reports watching it at home would,
// and there is nobody to send them to. So they are QUEUED, per server, and flushed to the owning server on
// reconnect through the ordinary JellyfinClient::reportProgress path — the same endpoint, the same body, the
// same fire-and-forget. Nothing about the server side of this is new; what is new is the buffer and its four
// rules.
//
// ==========================================================================================================
// 1. PER SERVER
// ==========================================================================================================
// The queue is keyed by server id because that is the granularity that can actually be flushed: one server
// comes back and the other stays off, and a single flat queue would either hold up the reachable server's
// reports behind the unreachable one's or would have to re-derive the split on every pass. It also means
// removing a server (JellyfinServerStore::remove) can drop its buffer whole.
//
// ==========================================================================================================
// 2. BOUNDED
// ==========================================================================================================
// kMaxPerServer reports per server, oldest dropped first. Unbounded, a fortnight off the network with a
// ten-second report interval is 120 000 rows in an ini file that is read on every settings write. The bound
// is on the queue, not on the truth: collapse() means the rows that matter — the LATEST position for each
// item — are the last ones in, so the bound sheds history rather than conclusions.
//
// ==========================================================================================================
// 3. ORDER AND IDEMPOTENCE
// ==========================================================================================================
// Reports are flushed in ascending `whenMs`, and collapse() first reduces the queue to ONE report per item:
// the newest. Ten reports for one episode are ten statements of the same fact at ten moments, and the server
// only wants the last one — sending all ten works but writes nine positions the user was at and has left,
// each of which is visible on every other device for the moment it is current.
//
// A report is dropped from the queue once it has been HANDED OVER, whether or not the server liked it. This
// is the same fire-and-forget reportProgress has always been (JellyfinClient.h says why a failed progress
// report is not worth a word) — retrying a position report forever is how a queue becomes permanent.
//
// ==========================================================================================================
// 4. A STALE REPORT MUST NEVER MOVE THE SERVER BACKWARDS
// ==========================================================================================================
// THE CASE THIS FEATURE IS JUDGED ON. You watch twenty minutes of an episode on the plane; you land; but
// meanwhile you also watched the whole thing on the television at home, and the server knows it. Flushing
// the queued twenty-minute report unread would rewind the server — and every other device — to a position
// the user left behind hours ago. The download feature would have DESTROYED progress, which is worse than
// not having synced any.
//
// So before a queued report is sent, the server is asked what it already knows about that item
// (JellyfinClient::fetchUserState -> Jellyfin::UserState), and shouldApply() decides:
//
//   * the server does not know this item at all      -> APPLY (nothing to lose)
//   * the server says the item is PLAYED             -> DROP (a position report un-marks it: strictly worse)
//   * the server's position is later than the queued one, by more than kStaleSlackSeconds
//                                                    -> DROP (the stale-report rule itself)
//   * otherwise                                      -> APPLY
//
// The slack exists because the two numbers are measurements of the same viewing taken up to a report
// interval apart: without it, a report queued a moment before the connection came back would be judged
// stale against the position IT had just caused. kProgressIntervalS is 10 s; half of it is the slack.
//
// A DROPPED REPORT IS STILL REMOVED FROM THE QUEUE. It has been decided, not deferred — leaving it in would
// re-ask the server about it forever.
//
// ==========================================================================================================
// 5. WHERE IT LIVES
// ==========================================================================================================
// Under "jellyfin/<profile>/offlineprogress/<serverId>", inside the prefix CloudSync::isDeviceLocalKey
// already carves out — see CloudSync.cpp on why "jellyfin/" is device-local. Two independent reasons apply
// here as well as the token one: a queue of reports THIS device owes a server is not a fact another device
// can act on, and syncing it would have two installs flush the same rows.
//
// NO CREDENTIAL IS STORED. A report holds a qualified id, a position, an event and the two session ids the
// server's own API requires; the token is read from JellyfinServerStore at flush time, exactly as every
// other Jellyfin request reads it.
#pragma once
#include "Jellyfin.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace OfflineProgress
{
    struct Report
    {
        QString qualifiedId;
        double  positionSeconds = 0.0;
        int     ev = int(Jellyfin::ProgressEvent::Progress);   // the enum, stored as its int
        QString playSessionId;
        QString mediaSourceId;
        qint64  whenMs = 0;                                    // UTC ms; the flush order and the collapse key
    };

    // See section 2. Chosen so a month of offline viewing still fits in a few tens of kilobytes.
    constexpr int    kMaxPerServer      = 400;
    constexpr double kStaleSlackSeconds = Jellyfin::kProgressIntervalS / 2.0;

    // ---- The pure rules (no store, no socket) --------------------------------------------------------
    // `existing` + `in`, bounded to `cap` by dropping the OLDEST. cap <= 0 gives an empty queue rather than
    // an unbounded one: "no bound" is not a state this queue is allowed to be in.
    QVector<Report> boundedAppend(const QVector<Report>& existing, const Report& in, int cap);

    // One report per item — the newest by whenMs — in ascending whenMs order. See section 3.
    QVector<Report> collapse(const QVector<Report>& in);

    // Section 4. `serverKnows` is false when the server did not answer with a UserData block at all.
    bool shouldApply(const Report& r, const Jellyfin::UserState& server);

    // ---- The store (per profile, device-local) -------------------------------------------------------
    QString queueKey(const QString& serverId);   // jellyfin/<profile>/offlineprogress/<serverId>

    void            enqueue(const Report& r);            // routes by the report's own server id
    QVector<Report> pending(const QString& serverId);    // as stored, oldest first
    void            replace(const QString& serverId, const QVector<Report>& rows);
    void            clearServer(const QString& serverId);
    QStringList     serversWithPending();                // which servers owe a flush at all

#ifdef EB_JELLYFIN_TEST_SEAM
    void setIniPathForTesting(const QString& path);
#endif
}
