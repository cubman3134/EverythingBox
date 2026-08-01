// An in-app "store": browse one or more public GitHub registries of themes or add-ons (each an index.json
// + raw files) and install entries into the local themes2/ or addons/ folder. A built-in default registry
// (github.com/cubman3134) is always present; users can add their own independent registries.
//
// The two kinds are shaped differently and only share the browsing chrome: an add-on entry LISTS its files,
// while a themes2 entry names a FOLDER whose contents come from the registry repo's own tree. Everything
// specific to the second lives in ThemeRegistry (core), so this dialog and the themed Appearance surface
// cannot disagree about which paths may become filenames or how a folder lands on disk.
#pragma once
#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QStringList>

class AddonManager;
class QNetworkAccessManager;
class QVBoxLayout;
class QLabel;
class QJsonObject;

class RegistryBrowser : public QDialog
{
    Q_OBJECT
public:
    enum Kind { Themes, Addons };
    RegistryBrowser(Kind kind, AddonManager* addons, QWidget* parent = nullptr);

    bool installedSomething() const { return installed_; }

private:
    QString defaultUrl() const;            // the built-in cubman3134 registry
    QStringList extraRegistries() const;   // user-added registries
    void saveExtras(const QStringList& list);
    QStringList allRegistries() const;     // default + extras

    void renderRegistryRows();             // (re)draw the list of configured registries
    void fetchAll();                       // load every registry and merge the entries
    void fetchOne(const QString& indexUrl);
    void renderEntry(const QJsonObject& entry, const QString& indexUrl);
    void installEntry(const QJsonObject& entry, const QString& indexUrl);
    bool downloadTo(const QString& url, const QString& destPath, QString* error);
    static QString baseUrl(const QString& indexUrl); // the index URL's directory
    QString localDirFor(const QString& id) const;
    bool isInstalled(const QJsonObject& entry) const;

    // Themes: the entry names a FOLDER, so the file list comes from the registry repo's own tree rather
    // than from the index. Cached per registry for this dialog's lifetime — one API call per install, and
    // unauthenticated GitHub allows 60 an hour per IP.
    void installThemeEntry(const QJsonObject& entry, const QString& indexUrl);
    QByteArray treeFor(const QString& indexUrl, QString* error);
    QHash<QString, QByteArray> treeCache_;

    void updateRepoLink();

    Kind kind_;
    AddonManager* addons_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
    QVBoxLayout* registriesLayout_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* repoLink_ = nullptr;
    int pending_ = 0;
    int total_ = 0;
    bool installed_ = false;
};
