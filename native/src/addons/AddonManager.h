// Discovers, loads and manages addons, exposing their media sources to the UI. Addons live in folders
// under <app>/addons/<id>/ (a manifest.json + entry script). Mirrors the Unity AddonManager.
//
// Threading: discovery is plain file I/O on the GUI thread. Each catalog/detail/search INVOCATION runs
// off-thread (QtConcurrent) in its own fresh Duktape context built from the addon's script source - so
// no interpreter state is shared across threads. Results come back to the GUI via catalogReady().
#pragma once
#include "AddonModels.h"
#include "StremioTranslate.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QHash>
#include <QSet>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QNetworkAccessManager;

// The outcome of resolving a playable: the url, its mime, and the HTTP request headers that url needs
// (behaviorHints.proxyHeaders.request — empty for every torrent/debrid source, non-empty for the
// direct-HTTP and embed hosts that gate their CDNs on a Referer/User-Agent).
//
// The headers ride the CALLBACK rather than being stashed on the manager and read back afterwards. That is
// the whole point: a member holding "the last stream's headers" is a thing that outlives its stream, and
// the failure it produces — host A's Referer arriving at host B — is silent at both ends.
using StreamCb = std::function<void(const QString& url, const QString& mime,
                                    const StreamHeaders::Headers& headers)>;

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
    // The fully parsed Stremio manifest. stremioResources/stremioTypes above remain as the quick-lookup
    // lists the rest of the class already uses; this carries everything they cannot (extras, idPrefixes,
    // behaviorHints, per-catalog classification).
    StremioTranslate::Manifest stremioManifest;
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
    // The manifest ids that ACTUALLY loaded on this launch — the input BrandMigration's two reconcile passes
    // are driven from, and the only place that answer exists (a remote add-on contributes its id only once its
    // cached manifest has been read). Exposed rather than open-coded because reload() is no longer the sole
    // caller: a cloud merge can land a blob still naming the previous brand's spelling, so MainWindow re-runs
    // the ref reconcile after every merge (#58 review).
    QStringList installedIds() const
    { QStringList ids; ids.reserve(int(loaded_.size())); for (const auto& a : loaded_) ids << a->manifest.id; return ids; }
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
    // Bool presence-peek with the same enabled + TTL semantics as cachedCatalog, but without copying the
    // MediaCatalog out. The prefetcher's freshness check (per catalog, per sweep) only needs "is it warm?",
    // so this avoids copying a full result payload just to discard it.
    bool hasCachedCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page,
                          const QMap<QString, QString>& filters) const;
    // The active catalog-cache TTL in ms (30 min by default; EB_PREFETCH_TTL_S scales it for testability).
    // The prefetcher reads this to size its resweep cadence off the same clock the cache expires on.
    qint64 catalogCacheTtlMs() const { return catalogCacheTtlMs_; }

    // Resolve a playable URL for a remote item via its /stream endpoint (async; the callback fires on the
    // GUI thread with the url+mime, or empty strings if there's no stream). JsLocal items already carry url.
    // attempt (?n=K) selects which source a file provider returns: 0 = best, 1 = next best, … - so the user
    // can reject a release and ask for another. Stremio sources ignore it.
    // preferGroup is the bingeGroup the user already chose for this series (BingeStore::lookup) — passed IN
    // rather than looked up here, so the addon layer keeps no dependency on a UI-owned store. It reaches only
    // the Stremio leg; empty = no memory, take the best candidate. This is the browse Play path for a Stremio
    // catalog leaf, so without it a remembered release would be honoured on the next-episode hand-off and
    // silently ignored the moment the user picked the next episode from the list themselves.
    void resolveStream(LoadedAddon* src, const MediaItem& item,
                       StreamCb cb, int attempt = 0,
                       const QString& preferGroup = QString());
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
    // True if at least one enabled Stremio addon declares the `subtitles` resource for this type — the gate the
    // UI checks before offering the add-on subtitle tier at all.
    bool hasSubtitleProvider(const QString& type) const;

    // Fan out /subtitles/{type}/{id} across every enabled Stremio addon that offers the `subtitles` resource
    // AND claims this id space (routeProviders, same routing + never-cost-a-result fallback as the stream
    // path). `localPath`, when non-empty, adds the OSDb videoHash/videoSize extras so an addon can match THIS
    // exact rip. The callback fires once, on the GUI thread, after every queried provider has answered (or
    // failed), with the aggregated rows in provider order (subtitles have no cross-provider ranking rule the
    // way streams do). Empty vector when nothing was found. Fire this AFTER playback begins — a slow subtitle
    // addon must never delay play start. See listStremioStreams: this is its twin.
    void listStremioSubtitles(const QString& type, const QString& id, const QString& localPath,
                              std::function<void(const QVector<StremioTranslate::SubtitleAddonResult>&)> cb);

    // GET a subtitle file `url` and save it under the shared subs cache dir; `cb` receives the local path
    // (empty on any failure). Addon subtitle results are URLs, but SubtitleCache keys on a LOCAL path (it
    // self-heals a missing file to a miss), and mpv's sub-add wants a real file — so the url is materialised
    // here, exactly as SubtitleFetcher materialises an OpenSubtitles download link. `lang` only names the file.
    void downloadSubtitleFile(const QString& url, const QString& lang,
                              std::function<void(const QString& localPath)> cb);
    // True if any enabled non-Stremio remote media-source (a file provider, e.g. Allarr) is installed - i.e.
    // a source whose /stream supports alternate-source selection (?n=K).
    bool hasFileProvider() const;
    // Resolve a playable source for an IMDB stream id ("tt123" or "ttShow:s:e"): try the file provider(s)
    // (Allarr) first, then the Stremio stream addons.
    // preferGroup is the bingeGroup the user already chose for this series (BingeStore::lookup) — passed IN
    // rather than looked up here, so the addon layer keeps no dependency on a UI-owned store. Empty = no
    // memory, take the best candidate.
    void resolveStreamByImdb(const QString& type, const QString& imdbStreamId,
                             StreamCb cb, int attempt = 0,
                             const QString& preferGroup = QString());

    // Every candidate stream for an item, from every eligible provider — the picker's source of choices.
    // Ordering is the translator's (mergeCandidates); this only aggregates across addons. The callback fires
    // once, after every queried provider has answered (or failed), with an empty vector when nothing is
    // playable.
    //
    // `maxRowsPerAddon` bounds each PROVIDER'S response, and defaults to the picker's bound. The resolution
    // path passes a larger one: those extra rows are never displayed, they only widen the pool of hashes the
    // debrid batch cached-check may hit. See StremioTranslate::parseStreams.
    void listStremioStreams(const MediaItem& item,
                            std::function<void(const QVector<StremioTranslate::StreamCandidate>&)> cb,
                            int maxRowsPerAddon = StremioTranslate::kMaxStreamRows);
    // Find a readable document on a file provider (Allarr) by searching its catalog of `catalogType` for
    // `query` and resolving the first hit's /stream. Used to read a comic/book/audiobook browsed from another
    // addon's catalog. providerError is non-empty when the provider couldn't be reached; noMatches is true when
    // the provider WAS reached but returned zero results — so the UI can distinguish "Allarr is down" from
    // "Allarr has no copy" from "found it, still caching".
    // `wantTitle` is the TITLE being looked for, apart from whatever else the query carries (an author, a
    // console, an issue number). The search needs those extra words to find anything; judging the RESULT needs
    // them gone, or every candidate looks wrong. A result whose title does not match is refused — see the note
    // on the pick.
    // WHAT ONE DOC-BRIDGE SEARCH FOUND. A struct rather than four positional arguments because #214
    // added a fifth and a sixth answer, and "the release is many files" / "the release has no audio at
    // all" are outcomes a caller must be able to see rather than infer from an empty url — an empty url
    // already means three different things here, which is exactly how a release with no audio ended up
    // staging a player that did nothing.
    struct DocFind
    {
        QString url;             // a readable/playable link, empty when there is none
        QString mime;
        QString providerError;   // non-empty: the provider could not be REACHED (not "no copy")
        bool    noMatches = false;   // the provider answered, with nothing

        // #214: the release's audio files, filtered and ordered, when it is a MULTI-FILE audiobook.
        // Empty for every other answer — a document, a comic, a single-file recording — so a caller
        // that only reads `url` behaves exactly as it did before this existed.
        QVector<RemoteAudiobook::Part> parts;
        // #214: a release WAS chosen and it contains no audio whatsoever (an ebook release that won an
        // audiobook search, which is #207's exact case one level deeper). Distinct from `noMatches`,
        // because the two want different sentences: "no copies were found" is wrong and unhelpful when
        // a copy was found and it is an EPUB.
        bool noAudio = false;

        // #216: the release was found AND expanded into its parts, and the first part still could not be
        // linked. The fifth meaning of an empty url, and the one that had no field: it was reported as a
        // plain miss, so the caller said its "isn't ready yet / still caching" sentence — a cause nothing
        // had established, which in the report that opened #216 was false and sent the user to look at
        // their debrid account instead of at the app. A caller that can SEE this says what is known
        // instead: a copy was found, its parts were listed, and the first one did not come back with a
        // link.
        bool noPartLink = false;

        // #216: what the PROVIDER said about this attempt, when it said anything (a /stream "notice" —
        // "42% cached", say). Carried here rather than left in takeStreamNotice() for two reasons: the
        // doc-bridge fans several queries out at once, and a take-once field shared between them belongs
        // to whichever answered last; and a notice that arrives with the answer it belongs to cannot be
        // read against a different one. Empty when the provider offered no words of its own, which is
        // when — and only when — a caller falls back to describing what it knows.
        QString notice;

        bool ok() const { return !url.isEmpty(); }
    };

    void resolveDocumentByQuery(const QString& query, const QString& wantTitle, const QString& catalogType,
                                std::function<void(const DocFind&)> cb);

    // ONE PART of a multi-file audiobook, resolved to a playable link at the moment the app reaches it
    // (#214). `partItemId` is the source's own id for that file — a release plus a file name, never a link,
    // which is what lets it sit in a queue for the fourteen hours a book takes and still mean the same part.
    //
    // The provider is looked up fresh on every call rather than remembered: a reload rebuilds the source
    // list and destroys every LoadedAddon in it, so a pointer held across a book is a dangling one. Empty
    // url through the callback when there is no provider or the file is gone from the release — the caller
    // says a sentence, and never leaves a player sitting on nothing.
    void resolveAudiobookPart(const QString& partItemId, StreamCb cb);

    // #214: THE RELEASE'S PARTS, when a chosen audiobook release is many files. `prov` is the provider
    // that answered the search and `release` the item it picked; this asks that item's /detail — the
    // SAME expansion endpoint a manga series' chapters come down (resolveChapterInSeries) —
    // filters the answer to audio and orders it, and then resolves PART ONE so the caller still gets a
    // url exactly as it always did.
    //
    // Part one and no more. Signing forty links here would be one round trip instead of forty, and
    // thirty-nine of them would have expired before the listener reached them: a queue that plays for
    // an hour and then dies mid-book is a worse failure than the one this fixes, and it looks like the
    // app breaking rather than like a link ageing out. Every later part is minted when the app reaches
    // it, from its id.
    //
    // Terminal: it never re-enters resolveDocumentByQuery, so there is no recursion, and every branch
    // ends in exactly one `cb` — the single-attempt rule RemoteLeafResolve.h states for the same reason
    // (a caller's callback that fires twice corrupts whatever the caller was sequencing).
    void resolveAudiobookRelease(LoadedAddon* prov, const MediaItem& release,
                                 std::function<void(const DocFind&)> cb);

    // ---- emulator BIOS provisioning through the EBS/Allarr file provider --------------------------------
    // The server exposes a `bios:bios` catalog whose items are BIOS/firmware files: id is
    // "bios:bs:{systemId}:{fileName}" (systemId + fileName parse out of it), and the item's subtitle is the
    // expected md5 (lowercase hex; empty => presence-only, no hash check). Bytes are resolved via
    // /stream/game/{id}.json (which returns a url relative to the provider base) and fetched from there.
    // There is NO hardcoded BIOS source any more: with no file provider configured, every call below is a
    // no-op — BIOS now requires a server.
    struct BiosServerFile { QString fileName; QString md5; QString itemId; };
    struct BiosServerSystem { QString systemId; QList<BiosServerFile> files; };

    // True if an enabled EBS/Allarr file provider (the BIOS source) is configured.
    bool hasBiosProvider() const;
    // Blocking enumeration off the file provider, for user-driven flows (Settings ▸ BIOS Check) and the
    // console. `providerErr` (optional) carries a human message when the provider couldn't be reached.
    QList<BiosServerFile>   biosFilesForSystem(const QString& systemId, QString* providerErr = nullptr) const;
    QList<BiosServerSystem> biosCatalog(QString* providerErr = nullptr) const;
    // Blocking fetch of one BIOS item: resolve its /stream, download the bytes, verify the md5 when present
    // (reject + write nothing on mismatch), and write to outPath (creating parent folders). Returns true only
    // on a verified write; `err` (optional) says why on failure.
    bool fetchBiosFile(const BiosServerFile& file, const QString& outPath, QString* err = nullptr) const;
    // Async launch-path fetch (best-effort, never blocks): enumerate `systemId`'s BIOS off the provider, then
    // download every file missing from destDir (or present with the wrong md5), verifying md5, chained on
    // network signals (no nested event loop). onDone always fires — immediately (no provider / nothing to do)
    // or after the chain settles — unless `context` is destroyed first, which cancels it. Callbacks run on
    // `context`'s thread.
    void ensureBiosAsync(const QString& systemId, const QString& destDir, QObject* context,
                         const std::function<void(const QString& text)>& onStatus = {},
                         const std::function<void()>& onDone = {});

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
    // Stremio stream resolution aggregates across every installed Stremio stream addon (à la Stremio):
    // listStremioStreams() for the candidates, pickAuto() for the choice, then play it directly or resolve
    // its infoHash through TorBox.
    void resolveStremioStream(const MediaItem& item,
                              StreamCb cb,
                              const QString& preferGroup = QString());
    // Turn a preference-ordered candidate list into a playable url. `ordered[0]` is the chosen release; a
    // direct http url there plays as-is.
    //
    // The REST of the list still matters for a torrent: TorBox can only stream what it has cached, and the
    // chosen release may not be. So the batch cached-check covers the whole list and the first entry in this
    // order that is PLAYABLE NOW wins — preserving the pre-pick behaviour (play the best release TorBox
    // actually has) instead of failing outright because one preferred hash was cold.
    void playStremioCandidates(std::shared_ptr<QVector<StremioTranslate::StreamCandidate>> ordered,
                               StreamCb cb);

    // Walk `ordered` and start the first entry that can play RIGHT NOW: a direct http url, or a torrent whose
    // hash is in `cached`. Returns false when nothing in the list qualifies.
    //
    // ONE loop, both kinds, in rank order — deliberately. Testing only the torrents here and treating direct
    // urls as a separate last-resort sweep reorders the list behind the sort's back: once the preferred
    // release turns out to be cold, "best remaining" is whatever ranks next, and the sort ranks an instant
    // http url ABOVE every torrent. Splitting the two made a stale binge preference downgrade an instant
    // stream into a debrid round-trip.
    //
    // `from` is the index to resume at: a cached torrent whose resolve chain comes back empty re-enters here
    // at the NEXT candidate rather than ending the attempt, because the batch check already proved the rest of
    // the cached rows are cached. Callers start at 0; only the retry continuation passes a non-zero value.
    bool playFirstPlayable(const QVector<StremioTranslate::StreamCandidate>& ordered,
                           const QSet<QString>& cachedHashes,
                           const StreamCb& cb,
                           int from = 0);
    // The enabled EBS/Allarr file provider that serves BIOS: an enabled non-Stremio remote media-source,
    // preferring one that declares a `bios:` catalog, else the first file provider. Null when none configured.
    LoadedAddon* biosFileProvider() const;
    // The first enabled remote, non-Stremio media source exposing a catalog of `catalogType`, and (out) that
    // catalog's id. ONE copy of the rule, because the doc-bridge search and the later per-part mint have to
    // land on the same provider — a part id means something only to the source that minted it.
    LoadedAddon* fileProviderForCatalogType(const QString& catalogType, QString* catalogIdOut = nullptr) const;
    // The real doc-bridge search. `triedSibling` guards the one comic↔manga retry: the public overload above
    // forwards with false, and the zero-results / no-provider branches recurse once into the sibling catalog
    // with true, so a manga filed as a comic (or vice versa) is still found on whichever shelf holds it.
    void resolveDocumentByQuery(const QString& query, const QString& wantTitle, const QString& catalogType,
                                std::function<void(const DocFind&)> cb,
                                bool triedSibling);
    // Drill a matched manga/comic SERIES container down to the requested chapter and resolve it. Manga is
    // filed as series→chapters, so a doc-bridge search for a chapter returns the series (expandable), not a
    // readable leaf. This fetches the series' chapter list, picks the chapter whose parsed number equals
    // `chapterNumber` (or the lowest-numbered chapter when `chapterNumber` is empty), and resolves it exactly
    // as the leaf path does. Terminal: it never re-enters resolveDocumentByQuery, so no recursion.
    void resolveChapterInSeries(LoadedAddon* prov, const MediaItem& series, const QString& chapterNumber,
                                std::function<void(const DocFind&)> cb);
    // Try each non-Stremio file provider (Allarr) in turn for an IMDB id; fall back to Stremio when none has it.
    void resolveFromFileProviders(std::shared_ptr<QVector<LoadedAddon*>> providers, int idx,
                                  const QString& type, const QString& imdbStreamId,
                                  StreamCb cb, int attempt = 0,
                                  const QString& preferGroup = QString());
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
    // Effective TTL: kCatalogCacheTtlMs, or EB_PREFETCH_TTL_S seconds when that env var is set (>0). The
    // override scales BOTH cache expiry (here) and the prefetcher's resweep cadence so tests can compress time.
    qint64 catalogCacheTtlMs_ = kCatalogCacheTtlMs;
};
