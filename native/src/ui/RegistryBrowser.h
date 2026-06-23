// An in-app "store": browse one or more public GitHub registries of themes or add-ons (each an index.json
// + raw files) and install entries into the local themes/ or addons/ folder. A built-in default registry
// (github.com/cubman3134) is always present; users can add their own independent registries.
#pragma once
#include <QDialog>
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
