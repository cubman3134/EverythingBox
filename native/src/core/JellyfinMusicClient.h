// THE JELLYFIN MUSIC CLIENT (issue #194, increment 3) — the socket half of the Jellyfin supplier: which
// server, which request, what to do with the reply, and what the user is told when it fails. Everything
// that could be decided without a socket is in JellyfinMusic.h.
//
// ==================================================================================================
// WHY THIS IS NOT A METHOD ON JellyfinClient
// ==================================================================================================
// JellyfinClient (#160) owns the VIDEO library: one fan-out across every enabled server, one callback when
// every leg has settled, and a union that deliberately does not dedupe. Music is a different shape at every
// level — one request per browse level rather than one fan-out per shelf, a per-server INDEX that is filled
// in as levels are opened rather than a list that is rebuilt, and a merge that very much does dedupe. Bolting
// that onto the same class would have given it two caches with two lifetimes and one name.
//
// They share what should be shared and nothing else: the same server list (JellyfinServerStore), the same
// identity for every id (Jellyfin::qualify), the same auth header, and the same rule about diagnostics.
//
// ==================================================================================================
// ONE REQUEST PER BROWSE LEVEL, AND A CACHE THAT IS NOT PERSISTED
// ==================================================================================================
//   the server's artists   /Artists                  -> JellyfinMusic::indexOfArtists
//   one artist's albums    /Users/<uid>/Items        -> JellyfinMusic::fillArtistAlbums
//   one album's tracks     /Users/<uid>/Items        -> JellyfinMusic::fillAlbumTracks
//
// SubsonicClient.h argues this at length and the argument is identical: walking the whole tree on entry is
// one request per artist plus one per album before a single row is drawn. The cache is per server and per
// session, and it is NOT persisted — a stale album list is worse than a fetch, and a persisted cache of
// somebody's library is a second copy of it on disk.
//
// A SERVER THAT DOES NOT ANSWER COSTS ONE BUDGET AND NOTHING ELSE. Each fetch is under its own deadline, and
// a fetch that fails leaves the cache exactly as it was — so the merged library is whatever the suppliers
// that did answer say it is, and the friend's box being switched off does not hold up your own shelf. That
// is #160's failure-isolation rule, restated one level up.
//
// ==================================================================================================
// NOTHING BUILDS A MESSAGE OUT OF A REQUEST
// ==================================================================================================
// There is no call to QNetworkReply::errorString() in this file. Qt's text embeds the url, every request
// here carries a token in a header, and a stream url carries one in its query — so transport failures are
// rendered from the NetworkError enum into fixed sentences of our own, exactly as JellyfinClient.cpp does.
// streamUrlFor() returns a string containing the token and is called at the one moment the player is handed
// it; nothing stores it.
#pragma once
#include "JellyfinMusic.h"
#include "MusicLibrary.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

class QNetworkAccessManager;

class JellyfinMusicClient : public QObject
{
    Q_OBJECT
public:
    // One per process. The browse surface and the player both need it, and two of them would be two caches
    // that disagreed about the same server.
    static JellyfinMusicClient& instance();

    // How a fetch ended. `ok` alone is not enough for a surface to speak: "that box is switched off" and
    // "that server refused the sign-in" want different sentences and only one of them is worth retrying.
    struct Result
    {
        bool    ok = false;
        bool    auth = false;      // the server refused the CREDENTIAL: retrying changes nothing
        QString message;           // one of our own sentences. Never a url.
    };
    using Done = std::function<void(const Result&)>;

    // ---- The three browse fetches ----------------------------------------------------------------------
    // Each merges into that server's cached Index and then calls back. Calling one that is already in flight
    // for the same target coalesces onto the same request rather than issuing a second.
    void fetchArtists(const QString& serverId, Done done);
    void fetchArtistAlbums(const QString& artistKey, Done done);   // key is qualified: it names its server
    void fetchAlbumTracks(const QString& albumKey, Done done);

    // The cached index for a server, possibly partial. Empty (and harmless) for an unknown server: a stale
    // route must render an empty level, never crash.
    const MusicLibrary::Index& index(const QString& serverId) const;

    // Asked rather than inferred from "is the container empty", because a genuinely empty artist (or server)
    // would otherwise be re-fetched on every Back, for ever, against a server that has already answered.
    bool artistsLoaded(const QString& serverId) const;
    bool artistLoaded(const QString& artistKey) const;
    bool albumTracksLoaded(const QString& albumKey) const;

    // ---- Playback and art ------------------------------------------------------------------------------

    // The stream url for a qualified TRACK id, through the server that owns it. Empty when the id is not
    // qualified, when its server is not configured any more, or when that server is switched off — three
    // situations with one honest answer, which the caller renders as "unavailable" rather than erroring at
    // play. THE RETURN VALUE CARRIES THE TOKEN: hand it to the player and drop it.
    QString streamUrl(const QString& qualifiedTrackId) const;

    // Fetch this album's cover into MetaCache (keyed on the qualified ALBUM id) if it is not already there.
    // No-op when it is already cached or a fetch is in flight.
    void prefetchAlbumCover(const QString& albumKey, std::function<void()> then = {});

    // The LOCAL FILE MetaCache holds for this album's cover, or an empty string. Deliberately not a fallback
    // to the remote url: a MediaItem's thumbnailUrl is copied into caches and item records, and the remote
    // url carries the token.
    QString albumCoverPath(const QString& albumKey) const;

signals:
    // A server's cached index changed (a fetch landed). The browse surface repopulates the level it is
    // standing in — the same way onMusicLibraryChanged handles a finished local scan.
    void indexChanged(const QString& serverId);

private:
    explicit JellyfinMusicClient(QObject* parent = nullptr);

    struct Cache
    {
        MusicLibrary::Index idx;
        QSet<QString>       loadedArtists;   // qualified artist keys whose albums have been fetched
        QSet<QString>       loadedAlbums;    // qualified album keys whose tracks have been fetched
        bool                artistsLoaded = false;
    };

    Cache& cacheFor(const QString& serverId);
    // One GET against one server, under one deadline. `then` gets the body and the outcome; a failed leg
    // hands an empty body and a Result whose message is one of our own sentences.
    void request(const QString& serverId, const QString& pathAndQuery, int budgetMs,
                 std::function<void(const QByteArray&, const Result&)> then);

    QNetworkAccessManager*        nam_ = nullptr;
    QHash<QString, Cache>         caches_;
    QSet<QString>                 inflight_;    // "<what>|<target>" — coalesces duplicate fetches
    QHash<QString, QVector<Done>> waiting_;     // the callbacks a coalesced fetch still owes
};
