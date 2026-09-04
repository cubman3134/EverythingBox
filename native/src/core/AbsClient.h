// THE AUDIOBOOKSHELF CLIENT (issue #197, increment 1) — the one object that talks to an Audiobookshelf
// server, and the per-server cache the browse levels render.
//
// Everything that could be decided without a socket is in Audiobookshelf.h (the id scheme, the URL rules,
// the request bodies, the payload readers, the chapter arithmetic, the reporting throttle). What is left
// here is the part that genuinely needs the world: which server, which request, what to do with the reply,
// and what the user is told when it fails. SubsonicClient.h makes the same split for the same reason.
//
// ==================================================================================================
// ONE REQUEST PER BROWSE LEVEL
// ==================================================================================================
//   the server's libraries    GET  /api/libraries                    -> Abs::readLibraries
//   one library's books       GET  /api/libraries/<id>/items          -> Abs::readLibraryItems
//   its series / its authors  GET  /api/libraries/<id>/series|authors -> Abs::readSeries / readAuthors
//   one item                  GET  /api/items/<id>?expanded=1         -> Abs::readItem
//   open a book               POST /api/items/<id>/play               -> Abs::readPlaySession
//   the user's position       GET  /api/me/progress/<id>              -> Abs::readProgress
//   ...and report it        PATCH /api/me/progress/<id>
//
// The cache is per server and per session, and is NOT persisted: a stale library list is worse than a
// fetch, the fetch is one request, and a persisted copy of somebody's library is a second copy of it on
// disk. A series' books come out of the SERIES reply (the server sends them inline), which is why opening
// a series costs no request of its own.
//
// ==================================================================================================
// THE TOKEN IS A HEADER, AND THE TWO PLACES IT IS NOT
// ==================================================================================================
// Every API request carries `Authorization: Bearer <token>`. That is not a stylistic preference: Subsonic
// puts its credential in the QUERY STRING, and SubsonicClient.h documents at length what that costs —
// QNetworkReply::errorString() embeds the URL, so the obvious diagnostic line puts the credential in the
// status bar and in stream_debug.log. A header cannot end up there.
//
// The two exceptions are forced and are both handled the way #200 settled: the STREAM url and the COVER
// url take `?token=`, because mpv is handed a url and MetaCache fetches one. Neither is ever stored — the
// stream url is minted at the moment a part is reached (RemoteAudiobook.h is the whole argument) and the
// cover's BYTES are stored while its url is not.
//
// Even so there is no call to errorString() anywhere in this file. Transport failures are rendered from
// the NetworkError enum into fixed sentences of our own, and everything else is the server's own words.
// Requests use SameOriginRedirectPolicy: a redirect to another host would carry the Authorization header —
// and, for a stream, the query — to a server the user never configured.
#pragma once
#include "AbsServerStore.h"
#include "Audiobookshelf.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;

class AbsClient : public QObject
{
    Q_OBJECT
public:
    // One per process. The browse surface and the player both need it, and two of them would be two caches
    // that disagreed about the same server — and two progress throttles reporting the same book twice.
    static AbsClient& instance();

    // How a fetch ended. `ok` alone is not enough for a surface to speak: "your sign-in expired" and "that
    // box is switched off" want different sentences and only one of them is worth retrying.
    struct Result
    {
        bool    ok = false;
        bool    auth = false;   // the server refused the TOKEN: retrying changes nothing, re-adding does
        QString message;        // our transport sentence, or the server's own. Never a url.
    };
    using Done = std::function<void(const Result&)>;

    // ---- Signing in ------------------------------------------------------------------------------------
    // POST /login with the password the user just typed, then GET the server's own identity. The password
    // is a PARAMETER and is not retained: on success `out` carries a token and no password, which is all
    // AbsServer has room for.
    //
    // The caller stores the result; this does not, so a failed sign-in cannot leave a half-configured
    // server behind.
    using LoggedIn = std::function<void(const Result&, const AbsServer&)>;
    void login(const QString& url, const QString& username, const QString& password, bool allowPlainHttp,
               const QString& displayName, LoggedIn done);

    // ---- Browse ----------------------------------------------------------------------------------------
    // Each merges into that server's cache and then calls back. A fetch already in flight for the same
    // target coalesces onto it rather than issuing a second request.
    void fetchLibraries(const QString& serverId, Done done);
    void fetchLibrary(const QString& qualifiedLibraryId, Done done);   // items + series + authors
    void fetchItem(const QString& qualifiedItemId, Done done);         // expanded: tracks, chapters, episodes

    // The cached answers, possibly absent. Empty (and harmless) for anything not fetched: a stale route
    // must render an empty level, never crash.
    QVector<Abs::Library>  libraries(const QString& serverId) const;
    bool                   librariesLoaded(const QString& serverId) const;
    QVector<Abs::Item>     libraryItems(const QString& qualifiedLibraryId) const;
    QVector<Abs::SeriesRow> series(const QString& qualifiedLibraryId) const;
    QVector<Abs::AuthorRow> authors(const QString& qualifiedLibraryId) const;
    bool                   libraryLoaded(const QString& qualifiedLibraryId) const;
    // The books of one series / one author, out of the library listing that is already cached. No request:
    // the listing carries each book's series and author names, so the filter is local.
    QVector<Abs::Item>     seriesBooks(const QString& qualifiedLibraryId, const QString& seriesId) const;
    QVector<Abs::Item>     authorBooks(const QString& qualifiedLibraryId, const QString& authorId) const;
    // The expanded item, or an ItemDetail whose `ok` is false.
    Abs::ItemDetail        item(const QString& qualifiedItemId) const;
    bool                   itemLoaded(const QString& qualifiedItemId) const;
    // The library a fetched item belongs to, for the title a level shows. Empty when unknown.
    QString                libraryNameOf(const QString& qualifiedLibraryId) const;

    // ---- Play ------------------------------------------------------------------------------------------
    // POST /api/items/<id>/play. The session is REMEMBERED here — its tracks, its chapters and the
    // position the server had for it — because everything downstream (minting a part's link, rebasing the
    // chapter list onto the part that is open, turning a position in a part into a position in the book)
    // is a question about that session and the alternative is passing four parallel vectors through the
    // player.
    using Opened = std::function<void(const Result&, const Abs::Session&)>;
    void openSession(const QString& qualifiedId, Opened done);

    // The session that openSession last landed for this id, or a Session whose `ok` is false.
    Abs::Session session(const QString& qualifiedId) const;

    // The stream url for ONE PART of an open session, minted now. Empty when there is no such session or
    // no such part. CREDENTIAL-BEARING: hand it straight to the player and write it down nowhere.
    QString partStreamUrl(const QString& qualifiedId, int partIndex) const;

    // ---- Progress --------------------------------------------------------------------------------------
    // Tell the server where the listener is. Throttled through Abs::shouldReport — the hook this rides
    // (PlaybackSession::persistResume) fires on a 5-second position change, which is right for an ini write
    // and far too chatty for a round trip to somebody's Raspberry Pi.
    //
    // `currentTime` is in BOOK time, not part time. The caller converts (Abs::absoluteTime); it is not done
    // here because only the caller knows which part is playing.
    //
    // `force` bypasses the throttle for the one moment it must not apply: leaving the book. A listener who
    // closes the app nine seconds into an interval would otherwise lose those nine seconds — and, worse,
    // the server would hold a position from before the last thing they heard.
    void reportProgress(const QString& qualifiedId, double currentTime, double duration, bool force = false);

    // The server's position for an item, fetched. Used where a session is not being opened (a Recents row
    // deciding whether to show a progress bar); the ordinary play path takes it off the play session, which
    // carries it, so opening a book costs ONE request rather than two.
    using GotProgress = std::function<void(const Result&, const Abs::Progress&)>;
    void fetchProgress(const QString& qualifiedId, GotProgress done);

    // ---- Art -------------------------------------------------------------------------------------------
    // Fetch this item's cover into MetaCache (keyed on the qualified id) if it is not already there. No-op
    // when it is cached or a fetch is in flight. `then` fires once the bytes have landed, so a level can
    // re-render with pictures on it.
    void prefetchCover(const QString& qualifiedId, std::function<void()> then = {});

    // The LOCAL FILE MetaCache holds for this item's cover, or empty. Deliberately NOT a fallback to the
    // remote url: a MediaItem's thumbnailUrl is copied into caches and item records, and the remote url
    // carries the token. A row with no picture yet is the correct thing to draw.
    QString coverPath(const QString& qualifiedId) const;

signals:
    // A server's cache changed (a fetch landed). The browse surface repopulates the level it is standing
    // in — the same way onAudiobookLibraryChanged handles a finished local scan.
    void cacheChanged(const QString& serverId);

private:
    explicit AbsClient(QObject* parent = nullptr);

    struct LibraryCache
    {
        QVector<Abs::Item>      items;
        QVector<Abs::SeriesRow> series;
        QVector<Abs::AuthorRow> authors;
        QString                 name;
        bool                    loaded = false;
    };
    struct ServerCache
    {
        QVector<Abs::Library>          libs;
        bool                           libsLoaded = false;
        QHash<QString, LibraryCache>   libraries;   // qualified library id -> its listing
        QHash<QString, Abs::ItemDetail> items;      // qualified item id -> its expanded detail
    };
    // What has been reported for one item this session, so the throttle has something to compare against.
    struct Reported
    {
        bool   ever = false;
        double pos = 0.0;
        qint64 atMs = 0;
    };

    ServerCache& cacheFor(const QString& serverId);

    // ONE request builder. `body` empty means GET; otherwise the verb is `verb` with that JSON body.
    void request(const AbsServer& srv, const QString& path, const QByteArray& verb, const QByteArray& body,
                 std::function<void(const QByteArray&, const Result&)> then);

    QNetworkAccessManager*          nam_ = nullptr;
    QHash<QString, ServerCache>     caches_;
    QHash<QString, Abs::Session>    sessions_;    // qualified id -> its open play session
    QHash<QString, Reported>        reported_;    // qualified id -> what the server was last told
    QSet<QString>                   inflight_;    // "<what>|<target>" — coalesces duplicate fetches
    // Items whose cover the server had nothing usable for. Remembered so a level does not re-ask on every
    // repaint — see prefetchCover, and the loop the first live drive of this feature walked into.
    QSet<QString>                   coverMissing_;
    QHash<QString, QVector<Done>>   waiting_;     // the callbacks a coalesced fetch still owes
};

// ==================================================================================================
// WHICH SUPPLIER OWNS THIS AUDIOBOOK KEY
// ==================================================================================================
// The seam the play path calls so that "the same player with different suppliers" is literally true.
namespace AbsSupply
{
    // Is this queue entry / resume identity an Audiobookshelf part? A part token names its BOOK by key
    // (RemoteAudiobook::bookKeyOfToken), and for one of ours that key is a qualified id — so this is one
    // question asked one way, and neither surface can answer it differently.
    bool isAbsEntry(const QString& queueEntryOrToken);

    // The qualified BOOK id a queue entry belongs to, or empty. Handles both shapes an entry can take: a
    // part token for a multi-file book, and the bare qualified id for a single-file book or an episode.
    QString bookIdOf(const QString& queueEntryOrToken);
}
