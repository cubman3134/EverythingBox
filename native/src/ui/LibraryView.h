// The Library: browse media-source addons and their catalogs. Selecting a source runs its getCatalog()
// (or search()); activating an item asks the main window to open it (routed by type/URL). Addons can be
// installed from .addon packages and reloaded here.
#pragma once
#include <QWidget>
#include <QVector>
#include <functional>
#include "../addons/AddonModels.h"

class AddonManager;
struct LoadedAddon;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QLabel;
class QStackedWidget;
class QDialog;

class LibraryView : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryView(AddonManager* mgr, QWidget* parent = nullptr);
    void refreshSources();

signals:
    void openItem(const MediaItem& item);
    void homeRequested();

private slots:
    void onSourceChanged();
    void onSourceCheckChanged(QListWidgetItem* item); // enable/disable toggled
    void onItemActivated();
    void onCatalogReady(int requestId, const MediaCatalog& cat); // async result
    void doSearch();
    void installAddon();
    void addByUrl();       // add a remote (HTTP) addon by its URL
    void removeSelected(); // remove the selected source (URL for remote, files for local)
    void browseAddons();   // open the add-on registry browser
    void configureAddon();
    void reloadAddons();

private:
    void showCatalog(const MediaCatalog& cat);
    // Host a sub-page inline (no popup): push it onto the internal stack; the helpers return to the main
    // list when done. showDialogPage embeds an existing QDialog; pushPage/popPage handle ad-hoc forms.
    void showDialogPage(QDialog* dlg, const std::function<void(int result)>& onFinished);
    void pushPage(QWidget* page);
    void popPage(QWidget* page);

    AddonManager* mgr_ = nullptr;
    QStackedWidget* stack_ = nullptr;  // page 0 = the library UI; sub-pages (browse/configure/confirm) on top
    QListWidget* sourceList_ = nullptr;
    QListWidget* itemList_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* status_ = nullptr;
    QVector<MediaItem> currentItems_;
    QVector<LoadedAddon*> sourceRefs_; // parallel to sourceList_ rows
    bool populating_ = false;          // suppress check-change handling while (re)building the list
    int pendingReqId_ = -1;            // in-flight async catalog/search request
};
