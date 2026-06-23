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
#include <functional>
#include <memory>
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
    bool isMediaSource() const
    {
        return manifest.type == QStringLiteral("media-source")
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
    const QVector<LoadedAddon*>& sources() const { return sources_; } // media-source addons
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
    int requestCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page);
    int requestDetail(LoadedAddon* src, const MediaItem& item, int page);
    int requestSearch(LoadedAddon* src, const QString& query);
    int requestMeta(LoadedAddon* src, const MediaItem& item); // metaReady(reqId, MediaDetail) fires later

    // Resolve a playable URL for a remote item via its /stream endpoint (async; the callback fires on the
    // GUI thread with the url+mime, or empty strings if there's no stream). JsLocal items already carry url.
    void resolveStream(LoadedAddon* src, const MediaItem& item,
                       std::function<void(const QString& url, const QString& mime)> cb);
    QString resolveStreamSync(LoadedAddon* src, const MediaItem& item); // blocking variant (probe/tests)

    bool installPackage(const QString& addonPackagePath, QString* error = nullptr); // import a .addon (zip)
    bool removeAddon(const QString& id);                                            // delete its folder

    // ---- remote (HTTP) sources: stored as URLs only, à la a subscribe-by-link model ----
    void addRemoteSource(const QString& url);          // fetch its manifest, persist the URL, reload (async)
    bool removeRemoteSource(const QString& baseUrl);   // drop the URL (and its cached manifest)
    QStringList remoteSourceUrls() const;

    bool isEnabled(const QString& id) const;
    void setEnabled(const QString& id, bool enabled);

    QString addonsRoot() const { return root_; }

signals:
    void catalogReady(int requestId, const MediaCatalog& catalog);
    void metaReady(int requestId, const MediaDetail& detail);
    void sourcesChanged();                                  // the source list changed (UI should refresh)
    void remoteSourceResult(bool ok, const QString& message); // outcome of addRemoteSource()

private:
    void loadFolder(const QString& dir);
    void loadRemoteSources();                   // build RemoteHttp addons from the persisted URL list
    AddonRequest buildRequest(LoadedAddon* src, const QString& function, const QString& argJson) const;
    int dispatch(const AddonRequest& req);     // run getCatalog/getDetail off-thread, deliver via catalogReady
    int dispatchMeta(const AddonRequest& req); // run getMeta off-thread, deliver via metaReady
    // Remote dispatch: async HTTP on the GUI thread (I/O-bound, so no worker thread), same result signals.
    int dispatchRemoteCatalog(LoadedAddon* src, const QString& catalogId, const QString& query, int page);
    int dispatchRemoteDetail(LoadedAddon* src, const MediaItem& item, int page);
    int dispatchRemoteMeta(LoadedAddon* src, const MediaItem& item);

    QString root_;
    std::vector<std::unique_ptr<LoadedAddon>> loaded_;
    QVector<LoadedAddon*> sources_;
    QNetworkAccessManager* nam_ = nullptr;      // remote-source HTTP (created lazily on the GUI thread)
    int reqCounter_ = 0;
};
