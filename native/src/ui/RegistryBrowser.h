// An in-app "store": browse one or more public GitHub registries of themes or add-ons (each an index.json
// + raw files) and install entries into the local themes2/ or addons/ folder. A built-in default registry
// (github.com/cubman3134) is always present; users can add their own independent registries.
//
// The two kinds are shaped differently and only share the browsing chrome: an add-on entry LISTS its files,
// while a themes2 entry names a FOLDER whose contents come from the registry repo's own tree. Everything
// specific to the second lives in ThemeRegistry (core), so this dialog and the themed Appearance surface
// cannot disagree about which paths may become filenames or how a folder lands on disk.
#pragma once
#include "../core/ThemeRegistry.h"   // ThemeRegistry::Entry — the parsed shape the themes path works in

#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QStringList>

#include <functional>

class AddonManager;
class QNetworkAccessManager;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QJsonObject;

class RegistryBrowser : public QDialog
{
    Q_OBJECT
public:
    enum Kind { Themes, Addons };
    RegistryBrowser(Kind kind, AddonManager* addons, QWidget* parent = nullptr);

    bool installedSomething() const { return installed_; }

    // An install is SYNCHRONOUS: it downloads one file per nested event loop (downloadTo), each behind a
    // 20 s wall, and the surfaces that host this dialog inline keep their own Back live while it runs.
    // Their Back replaces the panel content, which DELETES this dialog — under its own stack frame, with
    // the download code still to return into it. deleteLater() at the host cannot fix that: a deferred
    // delete posted from inside a nested loop is delivered by that same loop. So a host must ask before
    // navigating away, and hand the exit to closeWhenIdle() instead.
    bool isInstalling() const { return installing_; }

    // Leave when it is safe to: now if nothing is in flight, otherwise once the install finishes, via the
    // usual accept() -> finished -> host handler (queued, so it lands with no dialog frame on the stack).
    void closeWhenIdle();

private:
    void finishInstall();                  // end of an install: clear the flag, honour a deferred exit

    // Bracket around an install so EVERY exit from it — including the early error returns — clears the
    // flag and settles a deferred close.
    struct InstallScope
    {
        explicit InstallScope(RegistryBrowser* b) : b_(b) { b_->installing_ = true; }
        ~InstallScope() { b_->finishInstall(); }
        InstallScope(const InstallScope&) = delete;
        InstallScope& operator=(const InstallScope&) = delete;
        RegistryBrowser* b_;
    };

    QString defaultUrl() const;            // the built-in cubman3134 registry
    QStringList extraRegistries() const;   // user-added registries
    void saveExtras(const QStringList& list);
    QStringList allRegistries() const;     // default + extras

    void renderRegistryRows();             // (re)draw the list of configured registries
    void fetchAll();                       // load every registry and merge the entries
    void fetchOne(const QString& indexUrl);

    // The card chrome both kinds share. Everything kind-specific — what "installed" means and what the
    // button does — arrives as a value or a callable, so neither kind can fall into the other's branch by
    // omission. The callable owns the button because both kinds retitle it from their own result.
    void addCard(const QString& name, const QString& author, const QString& description,
                 const QStringList& formFactors, const QString& indexUrl, bool installed,
                 const std::function<void(QPushButton*)>& onInstall);

    // Add-ons. An add-on entry LISTS its files (or names a remote URL to subscribe to), so it stays on the
    // raw QJsonObject; nothing in ThemeRegistry describes that shape.
    void renderEntry(const QJsonObject& entry, const QString& indexUrl);
    void installEntry(const QJsonObject& entry, const QString& indexUrl);
    bool isInstalled(const QJsonObject& entry) const;

    bool downloadTo(const QString& url, const QString& destPath, QString* error);
    static QString baseUrl(const QString& indexUrl); // the index URL's directory
    QString localDirFor(const QString& id) const;

    // Themes: the entry names a FOLDER, so the file list comes from the registry repo's own tree rather
    // than from the index. Cached per registry for this dialog's lifetime — one API call per install, and
    // unauthenticated GitHub allows 60 an hour per IP.
    //
    // These take a PARSED ThemeRegistry::Entry, not a QJsonObject: parseIndex is what decides which index
    // key is authoritative and which entries are offerable at all, and re-deriving either here is how this
    // surface ends up listing a theme the themed Appearance surface refuses to show.
    void renderThemeEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl);
    bool isThemeInstalled(const ThemeRegistry::Entry& entry) const;
    void installThemeEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl);
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
    bool installing_ = false;      // an install's nested event loops are on the stack
    bool closeWhenIdle_ = false;   // an exit was asked for while they were, and is owed once they unwind
};
