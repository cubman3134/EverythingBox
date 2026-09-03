// The DEVICE-LOCAL half of "Follow a series" (issue #155): what THIS box has already seen of each followed
// series, and which children it has not shown you yet.
//
// IT DOES NOT SYNC, and that is a decision rather than an omission. "Which children existed the last time
// this device asked the source" is a claim about a fetch THIS install performed — the same shape as #23's
// backfill watermark and #148's calendar cache, both device-local for the same reason: synced, one device's
// completed check would suppress another device's first one, and the second device would show an empty New
// shelf having never asked anybody. A peer re-derives its own snapshot on its own first check (which is a
// silent baseline, not an announcement — see FollowPlan::diffChildren's `firstEver`), so nothing is lost by
// keeping it here. CloudSync::isDeviceLocalKey carves out the whole "followsnap/" prefix; the SYNCED half of
// the feature is FollowStore, carved the other way under "follow/". probe_follow pins both classifications.
//
// Layout, per profile, with the item key HASHED before use as an ini leaf — the SyntheticOffsets/ItemMarks
// lesson: a source's item id may be URL-shaped or carry '/', which QSettings would fold into colliding group
// paths. There is no fourth hashing scheme: it is the same MD5-hex-over-UTF8 the other stores use.
//
//   followsnap/<profileId>/<md5(itemId)>  ->  JSON {
//       checked:  <unix seconds of the last completed check; 0/absent = never>,
//       seen:     [<child id>, ...],       // the ids this device has already accounted for (only grows)
//       fp:       "<coarse fingerprint>",  // sources with no stable child ids (the degrade path)
//       pending:  [ {id,title,subtitle,thumbnailUrl,type,url,mime,foundAt,count}, ... ]
//   }
//
// `pending` is the New shelf's backing store: a child moves in when a check finds it, and out when the user
// deals with it (a completion mark, or "Mark all seen"). It carries the child's display fields because the
// shelf has to render a row for a child the catalogue is not currently showing — re-fetching the series to
// draw one home row would be a network request per shelf paint.
//
// QtCore only (a QSettings wrapper), so it links into probe_follow and runs headless.
#pragma once
#include "FollowPlan.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace FollowSnapshot
{
    // A child this device has found and not yet shown as dealt-with. `count` is 1 for a real child and is the
    // number of changes for the degraded "something changed" row a source with no stable child ids produces.
    struct Pending
    {
        QString id;
        QString title;
        QString subtitle;
        QString thumbnailUrl;
        QString type;
        QString url;
        QString mime;
        qint64  foundAt = 0;
        int     count = 1;
    };

    struct Snapshot
    {
        qint64            checked = 0;     // 0 = this device has never completed a check for this series
        QStringList       seen;
        QString           fingerprint;
        QVector<Pending>  pending;
        bool neverChecked() const { return checked <= 0; }
    };

    Snapshot get(const QString& itemId);           // unknown key -> a never-checked snapshot

    // Persist the result of one completed check: the grown seen-set, the fingerprint, and any newly found
    // children APPENDED to pending (de-duplicated by child id, so a child that is still pending from an
    // earlier cycle is not listed twice). Stamps `checked` with `nowSecs`.
    void record(const QString& itemId, const QStringList& seenAfter, const QString& fingerprintAfter,
                const QVector<Pending>& found, qint64 nowSecs);

    // "Mark all seen" on the series: the pending list is emptied. The seen-set is untouched — those children
    // are still accounted for, which is the point.
    void markAllSeen(const QString& itemId);

    // Drop ONE pending child (the user watched/read it, or dismissed it). No-op if it is not pending.
    void clearPending(const QString& itemId, const QString& childId);

    // Forget this series entirely — called when it is unfollowed, so a re-follow later starts from a clean
    // baseline instead of announcing everything published in between as new.
    void forget(const QString& itemId);

    // The stamp of the last completed check for THIS DEVICE across all series, and its setter. Device-local
    // for the same reason the snapshots are; it is what the scheduler's "is a cycle due" question reads.
    qint64 lastCycleAt();
    void   setLastCycleAt(qint64 nowSecs);

    // Convert the pure layer's Child into a Pending stamped at `nowSecs`. One place, so the shelf's row and
    // the store's row cannot drift apart.
    Pending fromChild(const follow::Child& c, qint64 nowSecs);
}
