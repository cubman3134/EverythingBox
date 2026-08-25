// THE SUBSONIC CLIENT (issue #193, increment 5) — the one object that talks to a music server, and the
// per-server INDEX CACHE that lets #74's browse builders render it.
//
// Everything that could be decided without a socket is in Subsonic.h (ids, auth, the envelope, the payload
// readers, the conversion onto MusicLibrary::Index). What is left here is the part that genuinely needs the
// world: which server, which request, what to do with the reply, and what the user is told when it fails.
//
// ==================================================================================================
// ONE REQUEST PER BROWSE LEVEL, AND WHY THE INDEX IS FILLED IN RATHER THAN FETCHED WHOLE
// ==================================================================================================
//   the server's artists   getArtists.view              -> Subsonic::indexOfArtists
//   one artist's albums    getArtist.view?id=…          -> Subsonic::fillArtistAlbums
//   one album's tracks     getAlbum.view?id=…           -> Subsonic::fillAlbumTracks
//
// Exactly one request per level the user opens, which is what every real Subsonic client does and the only
// shape that scales: the alternative — walk the whole tree on entry so the Index is complete — is one
// request per artist plus one per album, i.e. several thousand requests against an ordinary library, before
// a single row is drawn. The cost of the lazy shape is that a level's counts are known before its contents
// are, which is precisely what MusicLibrary::Album::trackCount and Artist::albumCount exist for.
//
// The cache is per server and per session. It is NOT persisted: a stale album list is worse than a fetch,
// the fetch is one request, and a persisted cache of somebody's library is a second copy of it on disk.
//
// ==================================================================================================
// WHAT THE INDEX STORES IS AN ID, NEVER A URL
// ==================================================================================================
// IndexTrack::path holds the qualified TRACK id, and MusicSupply::playUrl below turns it into a signed
// stream url at the one moment mpv is handed it. That is not fastidiousness: a stream url contains `t` and
// `s`, the index is copied into queues, and a queue is persisted (the resume position, and — the moment
// #193's "save queue as playlist" lands — a playlist file). An index that stored urls would write the
// user's credential into the ini and into every playlist they saved, and no later change could take it back
// out of the files already written.
//
// ==================================================================================================
// NOTHING BUILDS A MESSAGE OUT OF A REQUEST — AND `errorString()` IS A REQUEST
// ==================================================================================================
// ListenBrainzClient states this rule for an Authorization HEADER. Subsonic puts the credential in the QUERY
// STRING, which makes one specific thing unsafe that was safe there: **QNetworkReply::errorString() embeds
// the url**. Qt's own text for a failed transfer reads "Error transferring https://host/rest/getArtists.view
// ?u=…&t=…&s=… - server replied: Not Found" — so the obvious, idiomatic, everybody-writes-it diagnostic line
// puts the token and the salt into the status bar. There is therefore no call to errorString() anywhere in
// this file; transport failures are rendered from the NetworkError ENUM into fixed sentences of our own, and
// every other message is the server's own `message` field out of the failure envelope.
//
// For the same reason requests use SameOriginRedirectPolicy. A redirect to another host would carry the
// query — and therefore the credential — to a server the user never configured.
#pragma once
#include "MusicLibrary.h"
#include "Subsonic.h"
#include "SubsonicServerStore.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;

class SubsonicClient : public QObject
{
    Q_OBJECT
public:
    // One per process. The browse surface and the player both need it, and two of them would be two caches
    // that disagreed about the same server.
    static SubsonicClient& instance();

    // How a fetch ended. `ok` alone is not enough for a surface to speak: "your password is wrong" and "that
    // box is switched off" want different sentences and only one of them is worth retrying — the same reason
    // Scrobble::Verdict is an enum rather than a bool.
    struct Result
    {
        bool    ok = false;
        bool    auth = false;      // the server refused the CREDENTIAL: retrying changes nothing
        QString message;           // the server's own words, or one of our transport sentences. Never a url.
    };
    using Done = std::function<void(const Result&)>;

    // ---- The three browse fetches ---------------------------------------------------------------------
    // Each merges into that server's cached Index and then calls back. Calling one that is already in flight
    // for the same target coalesces onto the same request rather than issuing a second.
    void fetchArtists(const QString& serverId, Done done);
    void fetchArtistAlbums(const QString& artistKey, Done done);   // key is qualified: it names its server
    void fetchAlbumTracks(const QString& albumKey, Done done);

    // The cached index for a server, possibly partial. Empty (and harmless) for an unknown server: a stale
    // route must render an empty level, never crash.
    const MusicLibrary::Index& index(const QString& serverId) const;

    // Has this album's track list been fetched? The album level asks before deciding whether to show the
    // record or a "Loading…" row — Album::trackCount is the server's count and is set before the tracks are.
    bool albumTracksLoaded(const QString& albumKey) const;

    // The same question one and two levels up. They are asked rather than inferred from "is the container
    // empty" because a genuinely EMPTY artist (or server) would otherwise be re-fetched on every Back, for
    // ever, against a server that has already answered.
    bool artistsLoaded(const QString& serverId) const;
    bool artistLoaded(const QString& artistKey) const;

    // ---- Playback and art -----------------------------------------------------------------------------
    // The signed stream url for a qualified TRACK id. Empty for anything else, including a local file path —
    // so a caller can hand it every queue entry and let it decide. A fresh salt per call, per the scheme.
    QString streamUrl(const QString& qualifiedTrackId) const;

    // Fetch this album's cover into MetaCache (keyed on the qualified ALBUM id) if it is not already there.
    // No-op when the album has no cover art, when it is already cached, or when a fetch is in flight.
    // `then` fires once the bytes have landed, so a level can re-render with pictures on it.
    void prefetchAlbumCover(const QString& albumKey, std::function<void()> then = {});

    // The LOCAL FILE MetaCache holds for this album's cover, or an empty string. Deliberately not a fallback
    // to the remote url: a MediaItem's thumbnailUrl is copied into caches and item records, and the remote
    // url carries the credential. A row with no picture yet is the correct thing to draw.
    QString albumCoverPath(const QString& albumKey) const;

signals:
    // A server's cached index changed (a fetch landed). The browse surface repopulates the level it is
    // standing in — the same way onMusicLibraryChanged handles a finished local scan.
    void indexChanged(const QString& serverId);

private:
    explicit SubsonicClient(QObject* parent = nullptr);

    struct Cache
    {
        MusicLibrary::Index    idx;
        QHash<QString, QString> albumCoverId;    // qualified album key -> the server's coverArt id
        QSet<QString>          loadedAlbums;     // qualified album keys whose tracks have been fetched
        QSet<QString>          loadedArtists;    // qualified artist keys whose albums have been fetched
        bool                   artistsLoaded = false;
    };

    Cache& cacheFor(const QString& serverId);
    void   request(const SubsonicServer& srv, const QString& method,
                   const QList<QPair<QString, QString>>& extra,
                   std::function<void(const Subsonic::Node&, const Result&)> then);

    QNetworkAccessManager*    nam_ = nullptr;
    QHash<QString, Cache>     caches_;
    QSet<QString>             inflight_;         // "<method>|<target>" — coalesces duplicate fetches
    QHash<QString, QVector<Done>> waiting_;      // the callbacks a coalesced fetch still owes
};

// ==================================================================================================
// WHICH SUPPLIER OWNS THIS KEY
// ==================================================================================================
// The seam #74's surfaces call so that "the same UI with different suppliers" is literally true. Every music
// key in this app is either a MusicLibrary key or a Subsonic-qualified id, and Subsonic::parse tells them
// apart structurally (see Subsonic.h) — so a browse level, a queue build and a now-playing sleeve all ask
// the same question in the same way and neither surface can answer it differently.
namespace MusicSupply
{
    // The index a key belongs to: the local library's, or the server named INSIDE the key. Never guesses —
    // an unqualified key is local by definition, because a qualified one carries its server.
    const MusicLibrary::Index& indexFor(const QString& key);

    // What to hand the player for one IndexTrack::path. A local path passes straight through; a qualified
    // track id becomes a signed stream url. The ONE place a credential enters a queue.
    QString playUrl(const QString& path);

    // This album's artwork as a local file: the extracted/sibling cover for a local record, MetaCache's
    // fetched cover for a remote one. Empty when there is none (yet).
    QString albumArt(const MusicLibrary::Album& album);
}
