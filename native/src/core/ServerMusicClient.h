// THE EVERYTHINGBOX-SERVER MUSIC CLIENT (issue #194, increment 3) — the socket half of the server-shelf
// supplier. Everything that could be decided without a socket is in ServerMusic.h.
//
// ==================================================================================================
// THE SHELF LIST IS HANDED IN, NOT DISCOVERED HERE
// ==================================================================================================
// Which connected servers serve music is an ADDON question — it needs the loaded-source list, each source's
// base url and its per-source config header — and AddonManager is a large object that lives on the UI side.
// So this class holds a list of shelves it was GIVEN (setShelves), exactly the way MusicRemap takes its
// groups and Subsonic::trackIdFromStreamUrl takes its server roots: the decision is made where the
// information is, and this file stays a fetcher.
//
// It also keeps this unit out of the browse surface's way in the one place that matters: MusicSupply::playUrl
// is a free function with no UI in reach, and it still has to be able to resolve a shelf track.
//
// ==================================================================================================
// ONE REQUEST PER BROWSE LEVEL, AND A URL THAT LIVES FOR ONE SESSION
// ==================================================================================================
//   the shelf's artists    /catalog/<catalogId>.json      -> ServerMusic::indexOfArtists
//   one artist's albums    /detail/artist/<id>.json       -> ServerMusic::fillArtistAlbums
//   one album's tracks     /detail/album/<id>.json        -> ServerMusic::fillAlbumTracks
//
// The track rows come back carrying playable urls, and those urls may be signed. THE INDEX NEVER HOLDS ONE
// (ServerMusic.h says why); this class holds them in a per-session map keyed by the qualified track id, and
// MusicSupply::playUrl reads that map at the one moment the player is handed a url. Nothing persists it, so
// a saved queue, a playlist and a recents row all carry the ID and re-resolve — which is also what makes a
// re-signed url work tomorrow instead of a dead one.
//
// A shelf that does not answer costs one budget and nothing else: the cache is left exactly as it was, so
// the merged library is whatever the suppliers that DID answer say it is.
#pragma once
#include "MusicLibrary.h"
#include "ServerMusic.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;

class ServerMusicClient : public QObject
{
    Q_OBJECT
public:
    static ServerMusicClient& instance();

    // One connected server that serves a music shelf. `id` is the source's addon id and is the second field
    // of every key this supplier mints; `name` is what the picker calls it; `configHeader` is the per-source
    // blob AddonManager already sends with our own servers' requests (empty when the source has no settings).
    struct Shelf
    {
        QString    id;
        QString    name;
        QString    baseUrl;      // no trailing slash
        QString    catalogId;    // the music catalogue's own id
        QByteArray configHeader;
    };

    // Replace the list. Called whenever the addon sources change. A shelf that disappears keeps its cached
    // index (harmless: nothing routes to it any more) and loses its session urls, which is the same shape
    // JellyfinServerStore::remove documents for a removed server's stored rows.
    void setShelves(const QVector<Shelf>& shelves);
    QVector<Shelf> shelves() const { return shelves_; }
    bool has(const QString& sourceId) const;
    QString nameOf(const QString& sourceId) const;

    struct Result
    {
        bool    ok = false;
        QString message;   // one of our own sentences. Never a url.
    };
    using Done = std::function<void(const Result&)>;

    void fetchArtists(const QString& sourceId, Done done);
    void fetchArtistAlbums(const QString& artistKey, Done done);
    void fetchAlbumTracks(const QString& albumKey, Done done);

    const MusicLibrary::Index& index(const QString& sourceId) const;
    bool artistsLoaded(const QString& sourceId) const;
    bool artistLoaded(const QString& artistKey) const;
    bool albumTracksLoaded(const QString& albumKey) const;

    // What to hand the player for a qualified TRACK id: the url the shelf sent with that row, this session.
    // Empty when the album's tracks have not been fetched, or the shelf is gone — which the caller renders
    // as "unavailable" rather than erroring at play.
    QString streamUrl(const QString& qualifiedTrackId) const;

    // The album's cover, fetched into MetaCache under the qualified album key. The shelf sends an ordinary
    // image url on the album row; it is fetched rather than rendered from, because a MediaItem's thumbnail
    // url is copied into caches and item records and a shelf's url may be signed.
    void prefetchAlbumCover(const QString& albumKey, std::function<void()> then = {});
    QString albumCoverPath(const QString& albumKey) const;

signals:
    void indexChanged(const QString& sourceId);

private:
    explicit ServerMusicClient(QObject* parent = nullptr);

    struct Cache
    {
        MusicLibrary::Index idx;
        QSet<QString>       loadedArtists;
        QSet<QString>       loadedAlbums;
        QHash<QString, QString> albumCoverUrl;   // qualified album key -> the row's image url
        bool                artistsLoaded = false;
    };

    bool shelfFor(const QString& sourceId, Shelf& out) const;
    void request(const Shelf& shelf, const QString& path, int budgetMs,
                 std::function<void(const QByteArray&, const Result&)> then);

    QNetworkAccessManager*        nam_ = nullptr;
    QVector<Shelf>                shelves_;
    QHash<QString, Cache>         caches_;
    // THE SESSION-ONLY URL MAP. Qualified track id -> the url that row came with. Never written to disk.
    QHash<QString, QString>       trackUrls_;
    QSet<QString>                 inflight_;
    QHash<QString, QVector<Done>> waiting_;
};
