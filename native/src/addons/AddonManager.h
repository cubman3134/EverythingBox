// Discovers, loads and manages addons, exposing their media sources to the Library UI. Addons live in
// folders under <app>/addons/<id>/ (a manifest.json + entry script). Mirrors the Unity AddonManager,
// minus the Unity-specific bits; uses Duktape (JsAddon) as the script engine.
#pragma once
#include "AddonModels.h"
#include "JsAddon.h"

#include <QString>
#include <QVector>
#include <memory>
#include <vector>

class AddonContext;

struct LoadedAddon
{
    AddonManifest manifest;
    QString dir;                       // the addon's folder
    std::unique_ptr<JsAddon> addon;    // null if it failed to load / has no script
    bool isMediaSource() const { return addon != nullptr && manifest.type == QStringLiteral("media-source"); }
};

class AddonManager
{
public:
    AddonManager();

    void reload();                                  // re-scan the addons root and rebuild the source list
    const QVector<LoadedAddon*>& sources() const { return sources_; } // media-source addons
    const std::vector<std::unique_ptr<LoadedAddon>>& all() const { return loaded_; }

    MediaCatalog catalog(LoadedAddon* src);                       // getCatalog(), parsed + URLs resolved
    MediaCatalog search(LoadedAddon* src, const QString& query);  // search(), or empty if unsupported

    bool installPackage(const QString& addonPackagePath, QString* error = nullptr); // import a .addon (zip)
    bool removeAddon(const QString& id);                                            // delete its folder

    bool isEnabled(const QString& id) const;
    void setEnabled(const QString& id, bool enabled);

    QString addonsRoot() const { return root_; }

private:
    void loadFolder(const QString& dir);
    MediaCatalog resolved(MediaCatalog cat, const QString& addonDir) const;
    QString resolveUrl(const QString& url, const QString& addonDir) const;

    QString root_;
    std::vector<std::unique_ptr<LoadedAddon>> loaded_;
    QVector<LoadedAddon*> sources_;
};
