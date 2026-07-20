// Discovers, loads and manages addons, exposing their media sources to the UI. Addons live in folders
// under <app>/addons/<id>/ (a manifest.json + entry script). Mirrors the Unity AddonManager.
//
// Threading: discovery is plain file I/O on the GUI thread. Each catalog/detail/search INVOCATION runs
// off-thread (QtConcurrent) in its own fresh Duktape context built from the addon's script source - so
// no interpreter state is shared across threads. Results come back to the GUI via catalogReady().
#pragma once
#include "AddonModels.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QHash>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QNetworkAccessManager;

struct LoadedAddon
{
    // How the addon is run: bundled JS in a folder (Duktape), or a remote HTTP service we only reference
    // by URL (the manifest + responses are fetched over the network; nothing is stored but the URL).
    enum Transport { JsLocal, RemoteHttp };
    Transport transport = JsLocal;

    AddonManifest manifest;
    QString dir;       // JsLocal: the addon's folder
    QString source;    // JsLocal: entry script text (re-evaluated per request on a worker thread)
    QString baseUrl;   // RemoteHttp: service base URL (the manifest URL minus "/manifest.json")
    bool hasScript = false;
    // A RemoteHttp addon that speaks the Stremio protocol (catalog/meta/stream resources) instead of ours.
    bool stremio = false;
    QStringList stremioResources; // "catalog" / "meta" / "stream" / "subtitles"
    QStringList stremioTypes;     // "movie" / "series" / ...
    bool isMediaSource() const
    {
        return (manifest.type == QStringLiteral("media-source") || stremio)
               && (transport == RemoteHttp || hasScript);
    }
};

// One self-contained off-thread invocation: everything needed to load + run the addon, copied by value
// so it's safe to execute on a pool thread regardless of what the GUI does afterwards.
struct AddonRequest
{
    QString source;
    AddonManifest manifest;
    QString dir;        // for resolving relative item URLs / thumbnails
    QString storageDir; // addon-private getStorage/setStorage location
    QString function;   // "getCatalog" | "getDetail" | "search"
    QString argJson;
};

class AddonManager : public QObject
{
    Q_OBJECT
public:
    explicit AddonManager(QObject* parent = nullptr);

    void reload();                                  // re-scan the addons root and rebuild the source list
    // Re-fetch each remote source's manifest in the background; if one changed (e.g. the addon added a new
    // catalog), update its cache, rebuild, and emit sourcesChanged so new catalogs appear without re-adding.
    void refreshRemoteManifests();
    // Self-update installed JsLocal addons that declare a manifest "updateUrl": fetch the public package, and
    // if its version is newer than the installed one, reinstall it in place. Runs in the background on startup.
    void checkAddonUpdates();
    const QVector<LoadedAddon*>& sources() const { return sources_; } // media-source addons
    LoadedAddon* sourceById(const QString& manifestId) const          // resolve a source by its manifest id
    { for (LoadedAddon* s : sources_) if (s->manifest.id == manifestId) return s; return nullptr; }
    const std::vector<std::unique_ptr<LoadedAddon>>& all() const { return loaded_; }
    QVector<AddonCatalog> catalogs(LoadedAddon* src) const;

    // ---- synchronous (runs on the calling thread; used by the console probe / tests) ----
    MediaCatalog catalog(LoadedAddon* src, const QString& catalogId = QString(),
                         const QString& query = QString(), int page = 1);
    MediaCatalog detail(LoadedAddon* src, const MediaItem& item, int page = 1);
    MediaCatalog search(LoadedAddon* src, const QString& query);
    MediaDetail meta(LoadedAddon* src, const MediaItem& item);

    // ---- asynchronous (used by the UI) ----
    // Return a request id; catalogReady(reqId, result) fires later on the GUI thread. The UI ignores
    // results whose id it has superseded.
    // filters maps a CatalogFilter key (genre/year/rating/sort) to the selected value (empty = unfiltered).
    int requestCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                       const QMap<QString, QString>& filters = {});
    int requestDetail(LoadedAddon* src, const MediaItem& item, int page,
                      const QMap<QString, QString>& filters = {},  // filters apply to a container's children (e.g. a console's games)
                      const QString& query = {});                  // search WITHIN the container (scoped search)
    int requestSearch(LoadedAddon* src, const QString& query);
    int requestMeta(LoadedAddon* src, const MediaItem& item); // metaReady(reqId, MediaDetail) fires later

    // Synchronous peek into the catalog result cache (see catalogCache_). Returns the cached MediaCatalog iff
    // an entry exists for this exact request key, it's still within the cache TTL, AND the source is currently
    // enabled — the last clause closes the stale-disabled landmine (a source turned off after its catalog was
    // cached must never be served from that cache). Used by the CatalogPrefetcher to skip still-fresh entries
    // and by the UI to serve a menu instantly without a request round-trip. nullopt on any miss.
    std::optional<MediaCatalog> cachedCatalog(LoadedAddon* src, const QString& catalogId,
                                              const QString& query, int page,
                                              const QMap<QString, QString>& filters) const;
    // The active catalog-cache TTL in ms (30 min by default; MMV_PREFETCH_TTL_S scales it for testability).
    // The prefetcher reads this to size its resweep cadence off the same clock the cache expires on.
    qint64 catalogCacheTtlMs() const { return catalogCacheTtlMs_; }

    // Resolve a playable URL for a remote item via its /stream endpoint (async; the callback fires on the
    // GUI thread with the url+mime, or empty strings if there's no stream). JsLocal items already carry url.
    // attempt (?n=K) selects which source a file provider returns: 0 = best, 1 = next best, … - so the user
    // can reject a release and ask for another. Stremio sources ignore it.
    void resolveStream(LoadedAddon* src, const MediaItem& item,
                       std::function<void(const QString& url, const QString& mime)> cb, int attempt = 0);
    QString resolveStreamSync(LoadedAddon* src, const MediaItem& item); // blocking variant (probe/tests)
    // The most recent /stream "notice" (e.g. a "caching started" message from Allarr), consumed once —
    // empty when there was none. resolveStream sets it just before its callback, so a callback handed an
    // empty url can surface WHY there's no link yet instead of a bare "no source".
    QString takeStreamNotice() { const QString n = lastStreamNotice_; lastStreamNotice_.clear(); return n; }
    // Whether the last resolveStream returned a Cloudflare-gated direct url the client should fetch itself
    // via a browser-UA curl (set only on desktop, which asks with ?dl=curl). Consumed once by the callback.
    bool takeStreamCurl() { const bool c = lastStreamCurl_; lastStreamCurl_ = false; return c; }
    // Resolve a torrent (infoHash) to a streamable http URL via the TorBox debrid API (cached torrents only).
    void resolveTorBoxInfoHash(const QString& infoHash, int fileIdx,
                               std::function<void(const QString& url)> cb);
    // Resolve a manga chapter item (id "mangadexch:{ids}") to its ordered page image URLs via MangaDex (async).
    void resolveMangaChapterPages(const QString& chapterItemId,
                                  std::function<void(const QStringList& pageUrls)> cb);
    // True if at least one enabled stream source serves this type: a Stremio stream addon, or a non-Stremio
    // remote media-source used as a file provider (e.g. Allarr).
    bool hasStreamProvider(const QString& type) const;
    // True if any enabled non-Stremio remote media-source (a file provider, e.g. Allarr) is installed - i.e.
    // a source whose /stream supports alternate-source selection (?n=K).
    bool hasFileProvider() const;
    // Resolve a playable source for an IMDB stream id ("tt123" or "ttShow:s:e"): try the file provider(s)
    // (Allarr) first, then the Stremio stream addons.
    void resolveStreamByImdb(const QString& type, const QString& imdbStreamId,
                             std::function<void(const QString& url, const QString& mime)> cb, int attempt = 0);
    // Find a readable document on a file provider (Allarr) by searching its catalog of `catalogType` for
    // `query` and resolving the first hit's /stream. Used to read a comic/book/audiobook browsed from another
    // addon's catalog. providerError is non-empty when the provider couldn't be reached; noMatches is true when
    // the provider WAS reached but returned zero results — so the UI can distinguish "Allarr is down" from
    // "Allarr has no copy" from "found it, still caching".
    void resolveDocumentByQuery(const QString& query, const QString& catalogType,
                                std::function<void(const QString& url, const QString& mime,
                                                   const QString& providerError, bool noMatches)> cb);

    bool installPackage(const QString& addonPackagePath, QString* error = nullptr); // import a .addon (zip)
    bool removeAddon(const QString& id);                                            // delete its folder

    // ---- remote (HTTP) sources: stored as URLs only, à la a subscribe-by-link model ----
    void addRemoteSource(const QString& url);          // fetch its manifest, persist the URL, reload (async)
    bool removeRemoteSource(const QString& baseUrl);   // drop the URL (and its cached manifest)
    QStringList remoteSourceUrls() const;

    bool isEnabled(const QString& id) const;
    void setEnabled(const QString& id, bool enabled);

    // A local script addon (other than `exclude`) that has a catalog of `type` and can supply metadata for an
    // IMDB id - used to enrich a movie/episode whose own source addon returns no /meta (e.g. Allarr via AIO).
    LoadedAddon* metaProviderFor(LoadedAddon* exclude, const QString& type) const;

    // All enabled script addons that declared `metaFor` includes `type` (the aggregatable metadata providers,
    // e.g. the four game artwork providers). The host fans getMeta() out across these and merges the results.
    QVector<LoadedAddon*> metaProvidersFor(const QString& type) const;

    QString addonsRoot() const { return root_; }

signals:
    void catalogReady(int requestId, const MediaCatalog& catalog);
    void metaReady(int requestId, const MediaDetail& detail);
    void sourcesChanged();                                  // the source list changed (UI should refresh)
    void sourceEnabledChanged(const QString& id, bool enabled); // a source was enabled/disabled via setEnabled
    void remoteSourceResult(bool ok, const QString& message); // outcome of addRemoteSource()

private:
    void loadFolder(const QString& dir);
    void loadRemoteSources();                   // build RemoteHttp addons from the persisted URL list
    void seedDefaultStremioSources();           // add Cinemeta on first run so movie/series catalogs work
    // Stremio stream resolution aggregates across every installed Stremio stream addon (à la Stremio).
    void resolveStremioStream(const MediaItem& item,
                              std::function<void(const QString& url, const QString& mime)> cb);
    // Try each non-Stremio file provider (Allarr) in turn for an IMDB id; fall back to Stremio when none has it.
    void resolveFromFileProviders(std::shared_ptr<QVector<LoadedAddon*>> providers, int idx,
                                  const QString& type, const QString& imdbStreamId,
                                  std::function<void(const QString& url, const QString& mime)> cb, int attempt = 0);
    AddonRequest buildRequest(LoadedAddon* src, const QString& function, const QString& argJson) const;
    // Key for the catalog-result cache: source + catalog + query + page + filters (QMap iterates sorted).
    QString catalogCacheKey(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                            const QMap<QString, QString>& filters) const;
    int dispatch(const AddonRequest& req);     // run getCatalog/getDetail off-thread, deliver via catalogReady
    int dispatchMeta(const AddonRequest& req); // run getMeta off-thread, deliver via metaReady
    // Remote dispatch: async HTTP on the GUI thread (I/O-bound, so no worker thread), same result signals.
    int dispatchRemoteCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                              const QMap<QString, QString>& filters = {});
    int dispatchRemoteDetail(LoadedAddon* src, const MediaItem& item, int page);
    int dispatchRemoteMeta(LoadedAddon* src, const MediaItem& item);

    QString root_;
    std::vector<std::unique_ptr<LoadedAddon>> loaded_;
    QVector<LoadedAddon*> sources_;
    QNetworkAccessManager* nam_ = nullptr;      // remote-source HTTP (created lazily on the GUI thread)
    int reqCounter_ = 0;
    QString lastStreamNotice_;                  // /stream "notice" from the last resolveStream (see takeStreamNotice)
    bool lastStreamCurl_ = false;               // /stream "curl" flag from the last resolveStream (see takeStreamCurl)

    // Catalog browse/landing results (e.g. the console list) cached so re-opening a tab is instant instead of
    // re-fetching (a blocking HTTP GET or a JS getCatalog run). Populated from catalogReady for requestCatalog
    // calls; served on a hit within the TTL; cleared when addons reload. Search/detail results aren't cached.
    struct CatalogCacheEntry { qint64 atMs = 0; MediaCatalog cat; };
    QHash<QString, CatalogCacheEntry> catalogCache_;
    QHash<int, QString> pendingCatalogKey_;     // in-flight reqId -> cache key, to store the result on arrival
    static constexpr qint64 kCatalogCacheTtlMs = 30 * 60 * 1000; // 30 minutes (default)
    // Effective TTL: kCatalogCacheTtlMs, or MMV_PREFETCH_TTL_S seconds when that env var is set (>0). The
    // override scales BOTH cache expiry (here) and the prefetcher's resweep cadence so tests can compress time.
    qint64 catalogCacheTtlMs_ = kCatalogCacheTtlMs;
};
