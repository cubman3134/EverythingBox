// The landing screen: browse media-source addon catalogs by media type (Movies / TV / Games / Music),
// with poster thumbnails, search, and drill-down (a TV show -> seasons -> episodes; an album -> tracks).
// Leaf items emit openItem() (routed by the main window); file association comes later.
#pragma once
#include <QWidget>
#include <QVariant>
#include <QVector>
#include <QColor>
#include <QMap>
#include <QList>
#include "../addons/AddonModels.h"

class AddonManager;
struct LoadedAddon;
class CarouselView;
class QComboBox;
class QHBoxLayout;
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
    // Re-resolve the last-opened file-provider playable for an ALTERNATE source (?n=K) and re-open it. Backs
    // the player/reader "Issue with Streaming" button. No-op (with a toast) when there's nothing to retry.
    void requestNextSource();

    // For the themed (QML) home: the media-type catalogs as data, and a way to open one by its navKey.
    QVariantList systemItems();
    void activateNav(const QString& navKey); // open a catalog (or Home) by navKey

    // The four inherent top-level categories (video / audio / game / reading) that catalogs classify into.
    // categoryItems() returns the buckets that have at least one catalog (each {title,key,accent,glyph});
    // categoryCatalogs(key) returns the catalogs in that bucket (each {title,navKey,type,accent,subtitle}).
    // mediaCategory() maps a catalog/media type to a bucket key. Used by the themed XMB's two-step nav.
    static QString mediaCategory(const QString& type);  // "video" | "audio" | "game" | "reading"
    QVariantList categoryItems();
    QVariantList categoryCatalogs(const QString& categoryKey);

    // For the themed browse/gamelist: the current level's items as data, open/drill one, and go up a level.
    QVariantList browseItems();              // the loaded items as {title,subtitle,image,type,expandable}
    QString browseTitle() const;             // the current level's title
    void browseActivate(int index);          // open/drill the item at this (filtered) index
    bool browseBack();                       // go up a level; false if already at the catalog root
    bool browseHasMore() const;              // the current level has another page to pull
    void browseLoadMore();                   // pull the next page (no-op if none / already loading)
    int  browseRestoreIndex() const;         // the browse index of the row we last drilled into (for Back), else 0

    // Triple/XMB theme: live metadata beside the selection + an inline Play/Favorite on a leaf, all without
    // leaving the themed view. requestThemedMeta() emits themedMetaReady() (a skeleton at once, then the
    // addon's synopsis/facts). play/favoriteThemedLeaf() act on the browse-item at that (filtered) index.
    void requestThemedMeta(int browseIndex);
    void playThemedLeaf(int browseIndex);
    void favoriteThemedLeaf(int browseIndex);
    bool isThemedLeafFavorite(int browseIndex) const;

    // For themed search: run the existing search machinery with `query` against the current level (scoped to
    // a console, else the base media-type catalog). Empty query restores the full list. Fires browseItemsChanged.
    void searchInBrowse(const QString& query);
    bool atDetailLevel() const;              // the current level is an item's detail/info page
signals:
    // The current level's items changed. appended=true means a page was added to the end (keep the themed
    // selection); false means a fresh set (drill / back / search -> reset to the top).
    void browseItemsChanged(bool appended);
    // An item that opens a full info page (movie/series/book/comic/…) was activated. The themed host surfaces
    // the classic detail page (it renders here on HomeView, which the themed home otherwise hides).
    void infoPageRequested();
    // Triple/XMB theme: metadata for the browse-item at `index` (a skeleton first, then enriched). The host
    // shows it beside the cross. Carries {index,title,subtitle,image,type,overview,facts,favorite,expandable}.
    void themedMetaReady(int index, const QVariantMap& meta);
public:

    // Toast over the view (Play/Read progress + errors). Public so MainWindow can keep the same toast
    // going through the download phase (the "info as we pull the file" feedback the user wanted there).
    void showToast(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide)
    void hideToast();                                   // dismiss it now (e.g. the content view takes over)

signals:
    void openItem(const MediaItem& item);
    void downloadItem(const MediaItem& item); // a resolved file to download for keeps (-> Recents)
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls); // manga chapter
    void requestOpenFile(const QString& kind); // "video" | "audio" | "document" | "game"
    void openRecent(const QString& path, const QString& kind, const QString& resumeKey,
                    const QString& title, const QString& thumb); // re-open a "Recent" tab entry
    void settingsRequested();                  // the "Settings" button in the top bar
    void switchProfileRequested();             // the profile button in the top bar
    void themeChanged(const QColor& background, const QColor& accent); // active tab colour changed
    // Outcome of requestNextSource(): ok=true means a new source is being opened (openItem follows); ok=false
    // carries a message to show the user. Surfaced by MainWindow over the player/reader (HomeView is hidden).
    void nextSourceResult(bool ok, const QString& message);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override; // tune the grid's wheel-scroll speed
    void paintEvent(QPaintEvent* event) override;           // draw the theme background image, if any
    void resizeEvent(QResizeEvent* event) override;         // keep the toast centred

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
        QMap<QString, QString> filters; // selected catalog filters (genre/year/rating/sort) for this level
        MediaItem item;          // the container we drilled into (when detail)
        QString title;
        int childRow = -1;       // items_ index last drilled into from this level (restored on Back)
    };

    void selectType(LoadedAddon* addon, const QString& catalogId, const QString& type, const QString& name);
    void showCarousel();             // show the media-type carousel landing (carousel layout)
    void showXmb();                  // show the PS3 XMB layout (categories + item column)
    void activateItem(int row);      // open/drill a catalog item by row (shared by grid + carousel)
    void openDetailLevel(LoadedAddon* addon, const MediaItem& it); // push + show an item's detail page
    void fillCarouselFromItems(int from); // (re)build/extend the carousel from items_[from..]
    void fillXmbFromItems(int from);      // (re)build/extend the XMB item column from items_[from..]
    void selectRecent();             // show the local "recently opened" list (not an addon catalog)
    void openSteamConsole(const MediaItem& consoleItem); // drill the synthetic Steam console -> local games
    void populateSteamGames();                           // (re)build the Steam games list natively
    void requestSteamMeta(const MediaItem& item, int reqId); // native detail fetch for a Steam game
    QWidget* detailActionButton() const; // the focusable action on the detail page (Play for Steam, else Favorite)
    void renderRecents();            // populate the grid from RecentStore + favourites, grouped under headers
    void openFavorite(const MediaItem& favItem); // open a favourited item's detail page from Home
    void showItemContextMenu(int row, const QPoint& globalPos); // Home: remove a Recent/Favorite entry
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
    void maybeRestoreSelection();          // on Back, scroll to the drilled-into item (paging in if needed)
    void issueRequest(bool append);        // dispatch an async page request for the current view
    void populate(const MediaCatalog& cat, bool append);
    void loadThumbnails(int fromIndex);    // queue posters for items_[fromIndex..]
    void pumpThumbnails();                 // start queued poster loads up to the concurrency cap
    void repositionToast();                             // re-centre the toast near the bottom of the view
    void requestMeta(const MediaItem& item); // fetch + show the detail-header metadata for item
    void showMeta(const MediaDetail& detail);
    void hideMeta();
    // Resolve a leaf to a playable/readable source and emit openItem(). Pure (takes all context as args, not
    // detail-page state), so both the classic detail Play button and the themed inline Play reuse it.
    void resolvePlay(LoadedAddon* addon, const MediaItem& it, const QString& parentTitle,
                     const QString& console, const QString& imdbId, const QString& imdbType);
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
    QWidget* filterBar_ = nullptr;       // row of filter dropdowns above the grid (per-catalog, dynamic)
    QHBoxLayout* filterLayout_ = nullptr;
    QList<QComboBox*> filterCombos_;     // current filter dropdowns (each carries its filter key)
    QString filterSig_;                  // signature of the shown filters, to avoid rebuilding on every reload
    void rebuildFilterBar(const QVector<CatalogFilter>& filters); // sync the dropdowns to a catalog's filters
    void onFilterChanged();              // a dropdown changed -> re-run the current catalog with the selection
    QPushButton* back_ = nullptr;
    QPushButton* profileBtn_ = nullptr;  // shows the active profile; click to switch
    QPushButton* settingsBtn_ = nullptr; // the "Settings" button
    QColor themeColor_;                  // the active tab's colour (drives bars/buttons/headers)
    QLabel* status_ = nullptr;
    QLabel* toast_ = nullptr;          // floating notification (Play/Read progress + errors)
    QTimer* toastTimer_ = nullptr;     // auto-hides the toast
    QTimer* searchTimer_ = nullptr;    // debounces live-search as the user types
    QNetworkAccessManager* nam_ = nullptr;

    // Detail-page metadata header (shown when an item is opened; hidden on top-level catalog views).
    QFrame* meta_ = nullptr;
    QBoxLayout* metaLayout_ = nullptr;  // image<->text arrangement (poster/banner/text)
    QVBoxLayout* metaTextCol_ = nullptr; // the reorderable text column (favorite/title/facts/overview)
    QLabel* metaImage_ = nullptr;
    QWidget* actionRow_ = nullptr;    // holds Play + Favorite on the detail header
    QPushButton* favBtn_ = nullptr;   // ★ toggle on the detail header
    QPushButton* playBtn_ = nullptr;  // ▶ launch button shown on a Steam game's info page
    QPushButton* downloadBtn_ = nullptr; // ⬇ download this item (or, for a series/season, all its content)
    // Download crawl: walk a container's children, resolve each leaf's source, and emit downloadItem for it.
    // Runs sequentially (one resolve in flight) so it paces itself and reuses the existing async result signals.
    struct DlNode { LoadedAddon* addon = nullptr; MediaItem item; QString parentTitle; QString parentType; };
    QList<DlNode> dlQueue_;
    DlNode dlDetailNode_, dlMetaNode_; // the node whose detail/meta request is currently in flight
    int dlDetailReq_ = -1, dlMetaReq_ = -1;
    int dlQueued_ = 0;                  // files emitted for download this crawl
    bool dlBusy_ = false;
    void startDownload();              // begin a crawl from the current detail item
    void dlNext();                     // process the next queued node
    void dlResolveLeaf(const DlNode& node); // resolve one leaf's source, then continue
    void dlEmit(const MediaItem& it, const QString& url, const QString& mime); // queue a resolved file
    // TMDB->IMDB bridge: when a non-Stremio catalog item (e.g. AIO Catalog) supplies an IMDB stream id via
    // getMeta, Play resolves it through the installed Stremio stream addons. Set in showMeta for the open item.
    QString playImdbId_;              // "tt123" (movie) or "ttShow:s:e" (episode), else empty
    QString playStremioType_;         // "movie" / "series"
    // The last playable opened from a file provider (Allarr), so "Issue with Streaming" can re-resolve an
    // alternate source. viaImdb -> resolveStreamByImdb(type,imdbId,n); else resolveStream(addon,item,n).
    struct NextSourceCtx {
        LoadedAddon* addon = nullptr; // direct path: the file-provider addon
        MediaItem item;               // the item to re-open (its id/type drive the re-resolve)
        bool viaImdb = false;         // bridged movies/TV
        QString imdbType, imdbId;     // imdb path: resolveStreamByImdb args
        int attempt = 0;              // last ?n= used (0 = best)
    } lastPlay_;
    int steamMetaSeq_ = -1;           // unique (negative) ids for native Steam meta fetches
    // Triple/XMB theme live-meta + inline-play state (see requestThemedMeta()/playThemedLeaf()).
    int themedMetaReq_ = -1;          // in-flight addon /meta id for the live panel beside the cross
    int themedMetaIndex_ = -1;        // the browse index that fetch was for
    int themedPlayReq_ = -1;          // in-flight /meta id for a themed Play that needs the IMDB id first
    MediaItem themedPlayItem_;        // the item that deferred Play is resolving
    QString themedPlayConsole_;       // its console (ROM core hint), if any
    LoadedAddon* themedPlayAddon_ = nullptr;
    QLabel* metaTitle_ = nullptr;
    QLabel* metaFacts_ = nullptr;
    QTextBrowser* metaOverview_ = nullptr;
    int pendingMetaReqId_ = -1;
    bool metaFallbackTried_ = false; // tried enriching this item's empty /meta from a provider (AIO) already
    MediaItem metaItem_;             // the item whose detail header is showing (for the meta fallback)

    QVector<Level> stack_;       // navigation breadcrumb (top = current view)
    QVector<MediaItem> items_;   // items in the current view (parallel to grid_ rows)
    QVector<int> browseRowMap_;  // themed-browse index -> items_ row (skips synthetic _open/info rows)
    bool recentView_ = false;    // true = showing the local "Recent" list (not an addon catalog)
    bool searchEditing_ = false; // search box: false = highlighted (arrows navigate), true = typing
    QVector<int> thumbQueue_;    // item rows awaiting a remote poster load (throttled)
    int thumbActive_ = 0;        // remote poster loads currently in flight
    int generation_ = 0;         // bumped on each fresh load so stale async thumbnails are ignored
    int currentPage_ = 1;        // last page loaded for the current view
    int pendingRestoreRow_ = -1; // on Back: keep paging until this row is loaded, then scroll to it
    bool hasMore_ = false;       // the addon says another page exists
    bool loading_ = false;       // a page fetch is in progress (guards re-entrant scroll triggers)
    int pendingReqId_ = -1;      // in-flight async request; results with a different id are stale
    int pendingPage_ = 1;        // page number of the in-flight request
    bool pendingAppend_ = false; // whether the in-flight request appends or replaces
};
