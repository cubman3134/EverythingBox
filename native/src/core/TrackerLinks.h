// WHICH TRACKER ENTRY AN ITEM IS (issue #156) — the per-item link store, plus the "stop asking me about this
// one" memory that lives beside it.
//
// CLOUD SYNC. The "trackerlink/" prefix is in CloudSync::isPerItemStoreKey, NOT isDeviceLocalKey — the
// inverse of the credentials it sits next to, and deliberately. A link is a statement about the CONTENT
// ("this shelf row is AniList media 30002"), it costs the user a prompt per item to establish, and it is not
// a secret. It rides the CloudMerge document in the speed/lyricoffset shape: a flat { "<hash>": <blob> },
// newest-updatedAt wins per item, NO TOMBSTONES — unlinking writes a HUSK (an empty mediaId) rather than
// deleting the row, so a peer still holding the old link is beaten by a newer record instead of needing a
// deletion to be represented. probe_cloudmerge pins both the classification and the round trip.
//
// WHY THE "DON'T ASK" FLAG IS IN THE SAME BLOB. It is an answer to the SAME question ("what is this item on
// the tracker?"), just the negative one, and the issue requires that a refusal is not re-asked. In its own
// store it would be a second thing to sync, a second thing to carve out, and a second thing that could be
// merged out of step with the link it contradicts — one blob makes "linked" and "asked and declined"
// mutually exclusive by construction.
//
// PER ITEM, NOT PER PROFILE. A link is a fact about the media; two people sharing the box are still watching
// the same show. What is per-profile is the ACCOUNT, and that lives in the device-local credentials family.
#pragma once
#include "Tracker.h"

#include <QString>
#include <functional>

namespace TrackerLinks
{
    // What the store holds for one (tracker, item) pair.
    struct Link
    {
        QString      mediaId;                    // "" = not linked (never linked, or unlinked)
        tracker::Kind kind = tracker::Kind::Anime;
        QString      title;                      // the tracker's title for it, so the detail verb can say
                                                 // WHAT this is linked to without a network round trip
        int          totalUnits = 0;             // as Match::totalUnits — the COMPLETED rule needs it offline
        // HOW FAR THE APP ITSELF HAS GOT — the furthest episode/chapter this library has seen finished for
        // this item. It lives here rather than being derived at reconcile time because the app has no
        // per-unit local counter to derive it FROM: ItemMarks::Completion is a five-state mark on the
        // series, not a count, so "we are three chapters ahead of the tracker" is not a question the marks
        // store can answer. This is the app's side of tracker::reconcile, and like everything else in this
        // blob it syncs — a device that read three chapters offline still knows it did after the merge.
        int          localUnits = 0;
        bool         declined = false;           // the user said "don't ask about this item"
        qint64       updatedAt = 0;              // unix seconds; the merge's tie-break

        bool linked() const { return !mediaId.isEmpty(); }
    };

    // The ini group the per-item blobs live under ("trackerlink/items"), named here so the store and
    // CloudMerge's serializer/merger cannot drift on the spelling.
    QString itemsGroup();

    // The 10-hex-char MD5 leaf for one (tracker, item) pair — the SpeedStore/LyricOffsetStore hashing. The
    // tracker token is hashed IN, not appended after, so no item key containing the separator can be made to
    // read another tracker's link.
    QString hashFor(tracker::Id id, const QString& itemKey);

    // The link for `itemKey` on `id`, or a default-constructed Link when there is none. An empty itemKey, a
    // malformed blob and a missing row all read back the same way — never a wild media id.
    Link get(tracker::Id id, const QString& itemKey);

    // Remember that `itemKey` IS `mediaId` on this tracker. Clears `declined`: choosing a match is the user
    // answering the question they previously refused, and leaving the refusal set would make a later unlink
    // silently un-promptable.
    void set(tracker::Id id, const QString& itemKey, const QString& mediaId, tracker::Kind kind,
             const QString& title, int totalUnits);

    // Unlink. Writes the HUSK described above rather than removing the row, and deliberately KEEPS
    // `declined` false — the user unlinking is not the user refusing, and the next progress event should
    // offer the prompt again. (Someone who unlinks because the match was wrong wants to pick the right one.)
    void clear(tracker::Id id, const QString& itemKey);

    // Remember that the user does not want to be asked about this item. Stored, not held in memory, because
    // "the prompt is not repeated" has to survive a restart or it is not a promise.
    void decline(tracker::Id id, const QString& itemKey);

    // Raise this item's local progress to `unit`. MONOTONIC BY CONSTRUCTION: a lower value is ignored, so
    // re-reading chapter 3 of a series you have finished cannot regress the app's own side of the
    // reconciliation and cannot make the next pull push a lower number at the tracker. Returns true when it
    // really moved (the caller uses that to decide whether a sync push is owed). No-op when not linked:
    // there is nothing for the number to be the progress OF.
    bool noteLocalProgress(tracker::Id id, const QString& itemKey, int unit);

    // Should a progress event for `itemKey` open the match prompt? True only when there is no link AND the
    // user has not declined. This is the ONE predicate the push path gates the prompt on.
    bool shouldPrompt(tracker::Id id, const QString& itemKey);

    // Multi-device sync trigger, matching BookmarkStore/MetaOverrides: a std::function, not a Qt signal, so
    // this stays QtCore-clean and probes can leave it unset. Fired only when something actually changed.
    void setChangeHook(std::function<void()> hook);

    // Serialisation, exposed so the round trip is assertable without touching an ini.
    QString encode(const Link& l);
    Link    decode(const QString& blob);
}
