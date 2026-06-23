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
#include <QVector>
#include <memory>
#include <vector>

struct LoadedAddon
{
    AddonManifest manifest;
    QString dir;       // the addon's folder
    QString source;    // entry script text (re-evaluated per request on a worker thread)
    bool hasScript = false;
    bool isMediaSource() const { return hasScript && manifest.type == QStringLiteral("media-source"); }
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

    bool installPackage(const QString& addonPackagePath, QString* error = nullptr); // import a .addon (zip)
    bool removeAddon(const QString& id);                                            // delete its folder

    bool isEnabled(const QString& id) const;
    void setEnabled(const QString& id, bool enabled);

    QString addonsRoot() const { return root_; }

signals:
    void catalogReady(int requestId, const MediaCatalog& catalog);
    void metaReady(int requestId, const MediaDetail& detail);

private:
    void loadFolder(const QString& dir);
    AddonRequest buildRequest(LoadedAddon* src, const QString& function, const QString& argJson) const;
    int dispatch(const AddonRequest& req);     // run getCatalog/getDetail off-thread, deliver via catalogReady
    int dispatchMeta(const AddonRequest& req); // run getMeta off-thread, deliver via metaReady

    QString root_;
    std::vector<std::unique_ptr<LoadedAddon>> loaded_;
    QVector<LoadedAddon*> sources_;
    int reqCounter_ = 0;
};
