// The landing screen: browse media-source addon catalogs by media type (Movies / TV / Games / Music),
// with poster thumbnails, search, and drill-down (a TV show -> seasons -> episodes; an album -> tracks).
// Leaf items emit openItem() (routed by the main window); file association comes later.
#pragma once
#include <QWidget>
#include <QVector>
#include <QColor>
#include "../addons/AddonModels.h"

class AddonManager;
struct LoadedAddon;
class CarouselView;
class XmbView;
class QListWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QHBoxLayout;
class QVBoxLayout;
class QBoxLayout;
class QFrame;
class QTextBrowser;
class QNetworkAccessManager;

class HomeView : public QWidget
{
    Q_OBJECT
public:
    explicit HomeView(AddonManager* mgr, QWidget* parent = nullptr);
    void refresh();      // rebuild the media-type bar from the currently installed addons
    void applyTheme();   // re-read the active theme and recolour the current view
    void focusContent(); // put keyboard focus on the carousel / active tab / grid so arrows work

signals:
    void openItem(const MediaItem& item);
    void requestOpenFile(const QString& kind); // "video" | "audio" | "document" | "game"
    void openRecent(const QString& path, const QString& kind); // re-open a "Recent" tab entry
    void settingsRequested();                  // the "Settings" button in the top bar
    void switchProfileRequested();             // the profile button in the top bar
    void themeChanged(const QColor& background, const QColor& accent); // active tab colour changed

protected:
    bool eventFilter(QObject* obj, QEvent* event) override; // tune the grid's wheel-scroll speed
    void paintEvent(QPaintEvent* event) override;           // draw the theme background image, if any

private slots:
    void onItemActivated();
    void onCatalogReady(int requestId, const MediaCatalog& cat); // async result from AddonManager
    void onMetaReady(int requestId, const MediaDetail& detail);  // async detail-header metadata
    void doSearch();
    void goBack();

private:
    // A navigation level: a top-level catalog (by type), or a drilled-into container's children.
    struct Level
    {
        LoadedAddon* addon = nullptr;
        bool detail = false;     // false = getCatalog(catalogId), true = getDetail(item)
        QString catalogId;
        QString catalogType;     // media type of a top-level catalog (movie/series/game/album/book)
        QString query;
        MediaItem item;          // the container we drilled into (when detail)
        QString title;
        int childRow = -1;       // items_ index last drilled into from this level (restored on Back)
    };

    void selectType(LoadedAddon* addon, const QString& catalogId, const QString& type, const QString& name);
    void showCarousel();             // show the media-type carousel landing (carousel layout)
    void showXmb();                  // show the PS3 XMB layout (categories + item column)
    void activateNav(const QString& navKey); // open a carousel entry (media type, catalog item, or Home)
    void activateItem(int row);      // open/drill a catalog item by row (shared by grid + carousel)
    void fillCarouselFromItems(int from); // (re)build/extend the carousel from items_[from..]
    void fillXmbFromItems(int from);      // (re)build/extend the XMB item column from items_[from..]
    void selectRecent();             // show the local "recently opened" list (not an addon catalog)
    void renderRecents();            // populate the grid from RecentStore + favourites, grouped under headers
    void openFavorite(const MediaItem& favItem); // open a favourited item's detail page from Home
    void applyGridMode(bool recentList); // switch grid_ between the catalog poster grid and the recent list
    void styleTypeButtons(const QString& activeKey); // colour the top tabs + tint the catalogue background
    void applyThemeFont();           // set the app font family/scale from the active theme
    void layoutMetaSections(const QString& itemType); // declarative detail-page arrangement from the theme
    void focusTypeButton(int idx);   // keyboard: move to + activate a top tab (left/right)
    void focusGridTop();             // keyboard: drop focus into the grid (down)
    void focusChromeRow(QWidget* preferred = nullptr); // keyboard/controller: jump up to the top chrome
    void focusChrome(QWidget* from, int dir);          // move Left/Right within the chrome row
    QVector<QWidget*> chromeRow() const;               // the focusable top-bar controls, in order
    void focusUpFromColumn();                          // Up from a content column -> Favorite (if shown) else chrome
    QString openKindForView() const; // file-open kind to offer in the current view, or "" for none
    void loadTop();
    void loadMore();                       // fetch + append the next page (infinite scroll)
    void issueRequest(bool append);        // dispatch an async page request for the current view
    void populate(const MediaCatalog& cat, bool append);
    void loadThumbnails(int fromIndex);    // queue posters for items_[fromIndex..]
    void pumpThumbnails();                 // start queued poster loads up to the concurrency cap
    void requestMeta(const MediaItem& item); // fetch + show the detail-header metadata for item
    void showMeta(const MediaDetail& detail);
    void hideMeta();
    void styleMetaPanel(bool dark);  // theme the detail card: dark+light-text vs light+dark-text
    void updateChrome();
    void updateStatus();

    AddonManager* mgr_ = nullptr;
    QWidget* topBar_ = nullptr;               // backing behind the whole top row (themed, fills any seams)
    QWidget* typeHost_ = nullptr;             // holds the tabs; its empty stretch area is themed
    QHBoxLayout* typeBar_ = nullptr;
    QVector<QPushButton*> typeButtons_;       // the top media-type tabs (arrow-key navigation)
    QPushButton* activeTypeButton_ = nullptr; // the currently selected tab
    // A navigable destination (Home or a catalog), shared by the tabs and the carousel.
    struct NavTarget { QString navKey; bool isHome = false; LoadedAddon* addon = nullptr;
                       QString catalogId, type, name; };
    QVector<NavTarget> navTargets_;
    CarouselView* carousel_ = nullptr;
    bool carouselMode_ = false;
    bool atCarouselLanding_ = false; // showing the media-type carousel (the root)
    XmbView* xmb_ = nullptr;
    bool xmbMode_ = false;           // active theme layout is "xmb" (PS3 XrossMediaBar)
    bool atXmbRoot_ = true;          // at a category's top level (Left/Right switch categories)
    QString lastMediaKey_;           // last media type entered (to re-highlight on return to the carousel)
    QListWidget* grid_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* profileBtn_ = nullptr;  // shows the active profile; click to switch
    QPushButton* settingsBtn_ = nullptr; // the "Settings" button
    QColor themeColor_;                  // the active tab's colour (drives bars/buttons/headers)
    QLabel* status_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;

    // Detail-page metadata header (shown when an item is opened; hidden on top-level catalog views).
    QFrame* meta_ = nullptr;
    QBoxLayout* metaLayout_ = nullptr;  // image<->text arrangement (poster/banner/text)
    QVBoxLayout* metaTextCol_ = nullptr; // the reorderable text column (favorite/title/facts/overview)
    QLabel* metaImage_ = nullptr;
    QPushButton* favBtn_ = nullptr;   // ★ toggle on the detail header
    QLabel* metaTitle_ = nullptr;
    QLabel* metaFacts_ = nullptr;
    QTextBrowser* metaOverview_ = nullptr;
    int pendingMetaReqId_ = -1;

    QVector<Level> stack_;       // navigation breadcrumb (top = current view)
    QVector<MediaItem> items_;   // items in the current view (parallel to grid_ rows)
    bool recentView_ = false;    // true = showing the local "Recent" list (not an addon catalog)
    bool searchEditing_ = false; // search box: false = highlighted (arrows navigate), true = typing
    QVector<int> thumbQueue_;    // item rows awaiting a remote poster load (throttled)
    int thumbActive_ = 0;        // remote poster loads currently in flight
    int generation_ = 0;         // bumped on each fresh load so stale async thumbnails are ignored
    int currentPage_ = 1;        // last page loaded for the current view
    bool hasMore_ = false;       // the addon says another page exists
    bool loading_ = false;       // a page fetch is in progress (guards re-entrant scroll triggers)
    int pendingReqId_ = -1;      // in-flight async request; results with a different id are stale
    int pendingPage_ = 1;        // page number of the in-flight request
    bool pendingAppend_ = false; // whether the in-flight request appends or replaces
};
