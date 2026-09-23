// STORE-AND-FORWARD PROGRESS FOR A DOWNLOADED AUDIOBOOKSHELF BOOK (issue #197, offline listening).
//
// An Audiobookshelf server owns where its listener is (#197 increment 1, one resume owner: positions are
// whole-book seconds, reported throttled, and the server wins). A book downloaded to this device plays with
// the server switched off — on a plane, in a car park — and every position report it makes then has nobody
// to go to. So they are KEPT, per server, and sent when the server can be reached again. Jellyfin's
// OfflineProgress (#110) is the precedent; this is the same idea with Audiobookshelf's own vocabulary, and
// the four places it differs are the four places Audiobookshelf differs.
//
// ==========================================================================================================
// 1. ONE ENTRY PER BOOK — THE LATEST, NOT EVERY TICK
// ==========================================================================================================
// A position report is a statement of one fact ("the listener is HERE in this book") and every later one
// supersedes it. Jellyfin's queue keeps every report and collapses at flush time; Audiobookshelf has no event
// kinds to keep apart (no start/progress/stop — just a position), so there is nothing to collapse later and
// the queue holds exactly one entry per book, replaced as it goes. latestPerBook() is that rule, and an entry
// only ever replaces one that is OLDER (by whenMs): a report that arrives late cannot rewind the queue.
//
// ==========================================================================================================
// 2. SENT OR NOT
// ==========================================================================================================
// An entry is kept after it has been delivered, marked `sent`. It is then this device's LAST KNOWN POSITION
// for the book, which is what an offline open has to start from — the server cannot be asked, and without it
// a book listened to at home would open at the top of chapter one on the plane. Only an UNSENT entry is
// owed to the server and only an unsent one is ever flushed.
//
// ==========================================================================================================
// 3. THE SERVER STILL WINS ON OPEN — UNLESS THIS DEVICE'S POSITION IS NEWER
// ==========================================================================================================
// THE RULE, stated once and probed both ways (pickOnOpen):
//
//   When a downloaded book is opened and the server ANSWERS, the server's position is used — unless this
//   device holds an UNSENT position for the book whose local report time (`whenMs`) is LATER than the
//   server's `lastUpdate`. Then the local position is flushed to the server FIRST, and then used.
//
//   When the server does not answer, this device's last known position is used (sent or not), and failing
//   that the book opens at its top.
//
// "Later" is the comparison Audiobookshelf itself makes for its own mobile apps (POST /api/me/sync-local-
// progress: "for any local media progress with a greater lastUpdate time than the server's, the server's is
// updated"). The two clocks are this device's and the server's; that is the same trade Audiobookshelf's own
// clients make, and the one it documents.
//
// The FLUSH (AbsClient::flushOfflineProgress) applies the identical comparison before it sends anything, for
// OfflineProgress's section-4 reason: you listened for twenty minutes on the plane, and meanwhile finished
// the book on the kitchen speaker. Sending the plane's position unread would rewind the server — and every
// other device — to a place the listener left hours ago. localIsNewer() is the one spelling of "newer".
//
// ==========================================================================================================
// 4. WHERE IT LIVES
// ==========================================================================================================
// Under "audiobookshelf/<profile>/offlineprogress/<serverId>", inside the prefix CloudSync::isDeviceLocalKey
// already carves out of the synced bundle for the saved servers' tokens. DEVICE-LOCAL AND NOT SYNCED, for two
// independent reasons: a queue of reports THIS device owes a server is not a fact another device can act on,
// and synced, two installs would flush the same rows. probe_cloudmerge pins the carve-out through queueKey().
//
// NO CREDENTIAL IS STORED. An entry is a qualified id, two numbers, a time and a flag. The token is read from
// AbsServerStore at the moment of a request, exactly as every other Audiobookshelf request reads it.
#pragma once
#include "Audiobookshelf.h"   // Abs::Progress — what the server answers with

#include <QString>
#include <QStringList>
#include <QVector>

namespace AbsProgressQueue
{
    struct Entry
    {
        QString qualifiedId;     // abs:<serverId>:<itemId> (or an episode) — the book this position is in
        double  position = 0.0;  // WHOLE-BOOK seconds, the only time base the server speaks
        double  duration = 0.0;  // the book's length, for the report's fraction; 0 when unknown
        qint64  whenMs = 0;      // this device's clock, UTC ms, at the moment of the report
        bool    sent = false;    // delivered: kept only as the last known position (section 2)
    };

    // ---- The pure rules (no store, no socket) --------------------------------------------------------
    // Section 1: `existing` with `in` folded in — one entry per book, and a book's entry is replaced only by
    // a report at least as new as it. Order is by book as first seen; nothing here reorders a queue.
    QVector<Entry> latestPerBook(const QVector<Entry>& existing, const Entry& in);

    // Section 3's one comparison: is this device's report newer than what the server holds? True when the
    // server has never heard of the book (`!server.found`) — there is nothing to rewind — and otherwise only
    // when `local.whenMs` is strictly later than the server's `lastUpdate`.
    bool localIsNewer(const Entry& local, const Abs::Progress& server);

    // What a downloaded book opens at.
    enum class From { Server, Local, Start };
    struct OpenPick
    {
        From   from = From::Start;
        double position = 0.0;     // whole-book seconds
        bool   flushFirst = false; // the local entry is owed to the server and must be sent before it is used
    };
    // Section 3. `answered` is false when the server could not be reached at all (no HTTP answer); a 404 is
    // an answer ("never heard of it") and arrives as answered with `server.found == false`. `local` is the
    // entry this device holds for the book, or null.
    OpenPick pickOnOpen(bool answered, const Abs::Progress& server, const Entry* local);

    // ---- The store (per profile, device-local) -------------------------------------------------------
    QString queueKey(const QString& serverId);   // audiobookshelf/<profile>/offlineprogress/<serverId>

    void           put(const Entry& e);                    // routes by the entry's own server; latestPerBook
    bool           entryFor(const QString& qualifiedId, Entry* out);
    QVector<Entry> pending(const QString& serverId);        // as stored, sent and unsent
    QVector<Entry> unsent(const QString& serverId);         // what a flush owes, oldest report first
    // The entry was delivered: mark it sent — but only if it is still the report that was delivered (a newer
    // one that arrived while the request was in flight stays owed).
    void           markSent(const QString& qualifiedId, qint64 whenMs);
    // The server's answer superseded this device's entry (section 3): the entry becomes the server's
    // position, as sent, so an offline open later starts where the server said.
    void           adoptServer(const QString& qualifiedId, const Abs::Progress& server);
    void           remove(const QString& qualifiedId);      // the download went: its entry goes with it
    QStringList    serversWithUnsent();                     // which servers are owed a flush at all
}
