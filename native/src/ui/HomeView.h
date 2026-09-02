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
#include <QSet>
#include <QHash>
#include <QPointer>
#include "../addons/AddonModels.h"
#include "../core/ScrapedSnapshot.h" // the metadata editor's baseline, stamped with the item it is for (#24)
#include "../core/GameFilter.h"   // gamefilter::GameFacts — saved-filter shelf extraction (#63)
#include "../core/TraktRead.h"   // CalendarEntry — the cached Trakt calendar this view draws (#23)
#include "../core/TraktSync.h"   // TraktListEntry — the cached Trakt watchlist/collection (#23)
#include "../core/IptvSourceStore.h" // IptvSource — the Live TV source passed to fetchLiveTvChannels (#75)
#include "../core/XmltvGuide.h"      // xmltv::Guide — the parsed EPG held per open source (#75 inc 3)
#include "../media/StreamResolver.h" // M3uEntry — the in-session channel cache member's element type (#75)
#include "../browse/MusicCatalogs.h" // browse::MusicEmptyNote — the Music category's "nothing here" text (#74)
#include "../browse/AudiobookCatalogs.h" // browse::AudiobookEmptyNote — the same, for the books (#139)
#include "../browse/BookCatalogs.h"      // browse::BookEmptyNote - and the same again, for #134
#include "../browse/LeafRoute.h"     // browse::QueueTarget — what "add this row to the queue" means (#193)
#include "../core/MusicMerge.h"       // MusicMerge::Merged — one library over every supplier (#194)
#include "../core/HomebrewClient.h"  // HomebrewMore — a server's outstanding page, held by the Homebrew folder
#include "../comic/ChapterRun.h"   // ChapterRun — the chapters either side of an opened manga chapter

class AddonManager;
class BingeStore;
class SearchAggregator;
class GameMetaAggregator;
class RaBrowse;
class SteamAchievements;
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
// One sweep of the PC library (the four launcher scans + the downloads list + the TTL-cached Steam owned
// list), defined in HomeView.cpp. Opaque here on purpose: naming its members would drag EpicLibrary.h and
// BattleNetLibrary.h into every translation unit that includes this header.
struct PcLibScan;

class HomeView : public QWidget
{
    Q_OBJECT
public:
    explicit HomeView(AddonManager* mgr, QWidget* parent = nullptr);
    void refresh();      // rebuild the media-type bar from the currently installed addons
    // #193 increment 5: the OSK prompt that adds a Subsonic music server (name / address / username /
    // password, plus the two explicit downgrades). PUBLIC because both settings builders open it too — a
    // capability reachable only from the shelf it manages is one most people never find.
    void addMusicServerInteractive();
    void applyTheme();   // re-read the active theme and recolour the current view
    void focusContent(); // put keyboard focus on the carousel / active tab / grid so arrows work
    // Re-resolve the last-opened file-provider playable for an ALTERNATE source (?n=K) and re-open it. Backs
    // the player/reader "Issue with Streaming" button. No-op (with a toast) when there's nothing to retry.
    void requestNextSource();
    // Seed that same context from a Recents RE-MINT (#224), so "Issue with Streaming" works on a resumed
    // stream. A Recents re-open never populated lastPlay_ — nothing was browsed, so nothing was remembered —
    // and the swap therefore answered "No alternate source to try." on exactly the route whose failure
    // message points at it. `item` is the one MainWindow::remintAndOpen reconstitutes from the row (which is
    // the recipe itself), `route` is the row's sourceRoute and `imdbType` its sourceType — the stremio type
    // resolveStreamByImdb takes, which is why it is passed rather than read off the item's own `type`.
    void seedNextSourceFromRecipe(const MediaItem& item, const QString& route, const QString& imdbType);
    // The window's BingeStore (it owns the store; the browse Play paths that must consult it live here).
    // Borrowed, never owned — null is a valid state and simply means "no remembered release".
    void setBingeStore(BingeStore* store) { bingeStore_ = store; }
    // "Choose source…" on the themed detail row for the browse-item at `browseIndex`: emits
    // chooseSourceRequested with that item, which MainWindow turns into the picker.
    void requestChooseSource(int browseIndex);
    // Resolve a themed browse index to the romhack target NOW, while the index is still valid. The caller
    // then DEFERS the overlay a turn (crash #28): opening one from inside a QML activated handler runs a
    // nested loop under the delegate that is still emitting, and browseRowMap_ can be rebuilt in that window.
    bool romhackTargetAt(int browseIndex, MediaItem* itemOut, QString* systemOut) const;
    // NATIVE PORTS (issue #233). Which recompiled one-game port, if any, runs the game on this row — the
    // answer is empty for every row but the handful the catalog names, which is the whole point of the
    // binding (see core/NativePorts.h).
    //
    // TWO ENTRY POINTS, for the reason browseQueueTarget below has two: the layouts have two cursors.
    // `themedIndex` >= 0 is the themed column's own index (a browseRowMap_ position); -1 means "ask the
    // classic grid where it is standing", which is what the classic Start menu does.
    bool browseNativePort(int themedIndex, MediaItem* itemOut, QString* portIdOut) const;
    // The same question about an item the caller already holds (a Recents/Downloads row). "" = no port.
    QString nativePortIdFor(const MediaItem& it) const;
    // #193 increment 2 — the music row the "Add to queue" / "Play next" verbs act on.
    //
    // TWO ENTRY POINTS BECAUSE THE TWO LAYOUTS HAVE TWO CURSORS, and only one of them is an index this class
    // is handed: the themed column passes its own currentIndex (a browseRowMap_ position), while the classic
    // grid's cursor is grid_->currentRow() and lives in here. Passing -1 for `themedIndex` means "ask the
    // classic grid where it is standing", which is what the browse context menu does on that layout.
    bool browseQueueTarget(int themedIndex, browse::QueueTarget* out) const;
    bool queueTargetForRow(int itemsRow, browse::QueueTarget* out) const;   // an items_ row (right-click)
    // The copy of this item already on disk, or empty — see the note on the definition.
    QString localCopyForItem(const MediaItem& it) const;
    // The console page the current level belongs to, or empty outside one. Walks DOWN from the top so it
    // answers the same at both depths the romhack verb is offered from: a browse row (the platform IS the
    // top level) and a game's detail page (the platform is the level below it).
    QString browseConsoleName() const;
    // Download the base game for the romhack the user just chose, then report whether it started. The romhack
    // flow lives in MainWindow, which cannot do this itself: a catalog leaf carries no url until its source is
    // resolved, and the addon it came from is known only here. Both are captured when the verb is PRESSED,
    // not read when this runs — by then the user may have navigated somewhere else entirely.
    void startRomhackBaseDownload(std::function<void(bool started)> done);
    // Remember what the verb was pressed on. `systemId` is the system it was OFFERED on (retroSystemFor's
    // answer): the offer reads signals the browse stack does not have, and the base-ROM crawl needs the same
    // console the offer was made for — see browse/RomhackTarget.h.
    void noteRomhackTarget(const MediaItem& it, LoadedAddon* addon, const QString& systemId) const;
    // A picker request is in flight (MainWindow owns the round-trip): grey the classic "Choose source…"
    // button, exactly as the Play button greys itself while its own resolve is out, so two presses can't
    // stack two sticky notices and then two menus.
    void setChooseSourceBusy(bool busy);

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
    // Drill the Playlists folder for a category. Reached two ways: nested under a catalogue (asRoot=false, the
    // default — stacks on the catalogue level), or straight from the category-level bucket column (asRoot=true —
    // resets the browse stack so the list is the root, its Back returning to the bucket column). The themed home
    // passes asRoot=true when the "playlistsCategory" row is activated. categoryKey = video|audio|game|reading.
    void openPlaylistsLevel(const QString& categoryKey, bool asRoot = false);

    // The outcome of resolving+opening one item (openResolvedItem / playChannelItem). A channel airing needs to
    // know whether its pick actually PLAYED (chain continues), DETOURED to a detail page / stream-less dead end
    // (the channel must SKIP it, not wedge), or is PENDING an async /stream resolve (the resolveStream callback
    // reports the real outcome later, via channelPickResolved / channelPickDetoured).
    enum class ChannelAir { Played, Detoured, Pending };

    // Channel mode (driven by MainWindow, which owns the bag + the EOF-chain): air one playlist entry through
    // the SAME per-entry open path a row activation / Play-random uses, so a channel pick resolves identically.
    // `gen` tags this airing so a stale async result (superseded by a later pick / a manual play / channel exit)
    // is dropped. Returns the SYNCHRONOUS outcome; Pending means the async signal decides.
    ChannelAir playChannelItem(const QString& playlistId, int index, int gen);

    // Would this entry reach actual playback (vs. detour to a detail page)? Mirrors openResolvedItem's routing:
    // a local file / already-resolved url / an async-resolvable remote leaf plays; an info-page movie/episode,
    // container, or stream-less item detours. Lets the channel skip a detour BEFORE naming it in the countdown
    // (a remote leaf counts as "plays" — its rare async-resolve miss is caught after airing, not predictable here).
    bool channelItemPlaysDirectly(const QString& playlistId, int index);

    // The LOCAL FILE this entry would play, or empty for a pick that is anything else (issue #141 crossfade).
    // A crossfade has to open the incoming file seconds before the outgoing track ends, which means knowing a
    // path with no round trip and no side effects — so this answers only for the one entry shape that already
    // is a path: a local media entry, which openResolvedItem re-opens by path through openRecent. Every other
    // shape (a remote leaf whose source comes from an addon's /stream endpoint, a detail-page item) returns
    // empty and is aired the ordinary way, at its own boundary. Read-only: no toast, no lastPlay_, no request.
    QString channelItemLocalPath(const QString& playlistId, int index, QString* kind = nullptr);

    // For the themed browse/gamelist: the current level's items as data, open/drill one, and go up a level.
    QVariantList browseItems();              // the loaded items as {title,subtitle,image,type,expandable}
    QString browseTitle() const;             // the current level's title
    void browseActivate(int index);          // open/drill the item at this (filtered) index
    bool browseBack();                       // go up a level; false if already at the catalog root
    int  browseDepth() const { return stack_.size(); } // breadcrumb depth (1 = catalog root, >1 = drilled in)
    void goBack();                           // classic-home Back: pop a drill level, or emit backRequested at root
    bool browseHasMore() const;              // the current level has another page to pull
    void browseLoadMore();                   // pull the next page (no-op if none / already loading)
    int  browseRestoreIndex() const;         // the browse index of the row we last drilled into (for Back), else 0

    // Transient browse-level presentation filter (NOT persisted; cleared on the next real level load). mode:
    // 0 = All, 1 = Favorites, 2 = a completion status (comp = ItemMarks::Completion cast to int), 3 = a tag.
    // browseItems() applies it on top of the hidden filter; the host re-reads browseItems() after setting it.
    void setBrowseFilter(int mode, int comp, const QString& tag);
    void clearBrowseFilter();                // reset to All (called on every fresh level load)
    bool browseFilterActive() const { return browseFilterMode_ != 0; }

    // Triple/XMB theme: live metadata beside the selection + an inline Play/Favorite on a leaf, all without
    // leaving the themed view. requestThemedMeta() emits themedMetaReady() (a skeleton at once, then the
    // addon's synopsis/facts). play/favoriteThemedLeaf() act on the browse-item at that (filtered) index.
    void requestThemedMeta(int browseIndex); // INSTANT: local (session cache / gamelist / MetaCache) art + facts
    void enrichThemedMeta();                  // DEBOUNCED: online scrape + achievements + addon /meta for that row
    // The single emitter of themedMetaReady: stamps the row index and composites the user's correction over
    // the finished map, whichever of the five scraped sources assembled it (issue #24). Nothing else in this
    // class may emit that signal — a raw emit puts the scrape back over the correction a moment after the
    // page opens, which is how the feature came to work only while the network was down.
    void emitThemedMeta(int browseIndex, QVariantMap meta);
    // Dump-verification badge (issue #97). Returns a { label:"Dump", value:"Verified"/"Bad"/"Unknown" } fact
    // for a local-ROM game with a cached stamp; empty for anything else. When the feature is on and the ROM is
    // not yet stamped it kicks off ONE background verification (scheduleRomVerify) and returns empty for now —
    // the pass re-requests the panel when it lands. Appended by emitThemedMeta so every facts source picks it up.
    QVariant dumpStatusFact(const MediaItem& it);
    void scheduleRomVerify(const MediaItem& it, const QString& romPath);
    QSet<QString> romVerifyInFlight_;   // ROM paths whose background verification is running (dedupe)
    // Composite an item's miximage card on the thread pool (compose+PNG-encode is 100-400ms — synchronous on
    // the display path it WAS the themed shelf's per-row scroll hitch). Plans on the GUI thread (MetaCache is
    // not thread-safe), composes on a worker, then records the role + refreshes the panel if the row is still
    // selected. `idx` is the browse row to refresh (-1 = don't refresh, e.g. the detail page's lazy build).
    void ensureMiximageAsync(const QString& metaKey, int idx);
    QSet<QString> miximageInFlight_;    // metaKeys whose composite is being built (dedupe)
    // The themed DETAIL view's own /meta: fetched ONLY for a leaf that could bridge to a Stremio stream id and
    // hasn't yet (the id exists nowhere but /meta). The XMB gets this from its hover debounce; the grid browse
    // has no hover fetch, which is why "Choose source…" was unreachable on the default browse path.
    void requestThemedDetailMeta(int browseIndex);
    // routeHint (0=default, 1=force built-in, 2=force external) is a one-off external-player override from a
    // detail action: for a catalog leaf it is stamped on the resolved MediaItem so it rides the async resolve
    // chain leak-free (a failed resolve never emits the item); local/recents leaves resolve synchronously and
    // are handled by MainWindow's consume-once member instead, so the hint is a no-op for them.
    void playThemedLeaf(int browseIndex, int routeHint = 0);
    void downloadThemedLeaf(int browseIndex);      // resolve + queue the browse-item to download (no play)
    void favoriteThemedLeaf(int browseIndex);
    bool isThemedLeafFavorite(int browseIndex) const;
    void addBrowseItemToPlaylist(int browseIndex); // pick/create a playlist + add the browse-item (themed + key)
    // The themed DETAIL view's data for the browse-item at `browseIndex`: the rich MediaDetail (title/subtitle/
    // overview/facts + art via MediaArt::writeInto) resolved from the same local sources requestThemedMeta uses
    // (session cache / gamelist.xml / MetaCache), plus a joined `factsText`, an `actions` verb list (play/
    // favorite/download/playlist, filtered per-item), a `favorite` flag and a `readable` flag. Empty map for a
    // divider/synthetic row (not a media item). This is what the themed detail view binds through selected.*.
    QVariantMap themedDetailData(int browseIndex);
    bool isThemedInfoLeaf(int browseIndex) const;  // a non-expandable info-page leaf (movie/book/…): opens detail
    // The per-profile marks key (MetaCache::keyFor) for the browse-item at `browseIndex`, or empty for an out-
    // of-range/synthetic row. MainWindow's detail hide/status/tags verbs address ItemMarks through this so they
    // stay correct regardless of any row-index churn a hide causes.
    QString themedLeafKey(int browseIndex) const;
    // The resolved SystemCatalog system id for a game leaf at `browseIndex`, or empty when it isn't an
    // override-capable game (not a game, or no system with candidate cores / a standalone emulator). The
    // "Launch options…" detail action (issue #51) reads this to build its candidate-core / -emulator lists.
    QString themedLeafSystemId(int browseIndex) const;
    // The local file path of a game leaf at `browseIndex` (its MediaItem url), or empty when it isn't a game.
    // The "Other versions" detail action (issue #50) reads this to re-derive the game's region/revision
    // siblings from its own folder.
    QString themedLeafGamePath(int browseIndex) const;
    // True when the focused browse row is a game leaf (item.type == "game"), regardless of whether its OWN system
    // resolved. The Start emulation panel (Task 5) uses this so a game whose system can only be inferred from the
    // console FOLDER it sits in (a catalog/streamed game with no systemHint and an ambiguous/absent extension) is
    // still treated as a Game context, not misfiled as the bare console.
    bool themedLeafIsGame(int browseIndex) const;
    // The SystemCatalog system id of the CURRENT browse LEVEL (not a leaf) when it is a single-console folder,
    // else empty. A drilled-into console/platform level carries the console NAME as its title (resolved via
    // SystemCatalog::forConsoleName, the same rule gameFactsFor uses); a synthetic per-console Favorites /
    // Recent / Downloaded level carries the system in its mime marker. The Start emulation panel (Task 5) reads
    // this for the Console context when no override-capable game leaf is focused.
    QString currentLevelSystemId() const;
    // Drop one item's entry from the per-session resolved-art cache. That cache short-circuits the whole
    // MetaCache read path, so after a metadata correction (issue #24) it would keep serving the artwork the
    // user just replaced — for the rest of the session, on every screen that hovered the item.
    void forgetThemedArt(const QString& metaKey) { themedArtCache_.remove(metaKey); }
    // Re-render the CLASSIC detail card from the cache after a metadata correction (issue #24), so the fix
    // lands on the screen it was made from. Reads MetaCache::cachedDetail, which composites the override —
    // no network, no re-scrape. No-op when no detail card is open.
    void refreshDetailMetaCard();
    // A row from items_ as the PROVIDERS gave it — the pre-correction copy when it carries a correction,
    // else the row itself. Anything that WRITES a row into the scrape cache must use this: the cache is the
    // scraped layer that the correction composites over on every read, so saving the composited row would
    // bake the user's edit in as if the scraper had said it, and "reset to scraped" would restore the edit.
    MediaItem scrapedRow(const MediaItem& shown) const;
    // The run to hand the reader when `currentId` is opened: the remembered chapter list, normalised into
    // reading order. An empty/absent list yields an invalid run, which reads as "no neighbours".
    ChapterRun chapterRunFor(const QString& currentId, bool catalogLane = false) const;
    // What the providers said about the open detail card — the metadata editor's baseline and reset target.
    MediaDetail detailScrapedValues() const;
    // The same for the THEMED detail card at `browseIndex`, assembled from the scraped sources that card is
    // built from (its own /meta reply, the scrape cache, the ROMs gamelist, the pre-correction catalog row).
    MediaDetail themedScrapedValues(int browseIndex) const;
    // Re-apply the hidden filter to the live surface (the Show-hidden toggle / a profile switch changed it):
    // the Home list rebuilds synchronously; a catalogue level re-issues its request so the filter runs as its
    // items land. Cheapest existing refresh path — no bespoke re-filter of items_ in place.
    void reloadForFilterChange();

    // For themed search: run the existing search machinery with `query` against the current level (scoped to
    // a console, else the base media-type catalog). Empty query restores the full list. Fires browseItemsChanged.
    void searchInBrowse(const QString& query);
    // Cross-addon search: query every enabled source's catalogs for `query` at once and merge the results into
    // one grid (deduped by title+type; each result remembers its source so it re-opens through the right addon).
    void searchEverything(const QString& query);
    bool atDetailLevel() const;              // the current level is an item's detail/info page
    bool atRecentsLevel() const;             // the current level is a catalogue's synthetic Recent folder
    bool atDownloadsLevel() const;           // the current level is a catalogue's synthetic Downloaded folder
    bool atFavoritesLevel() const;           // the current level is a console's synthetic Favorites folder
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
    // These no longer draw locally - they emit toastRequested/toastHideRequested so MainWindow can render
    // the message as a window-level overlay that stays visible over ANY theme (a themed home is a native
    // QQuickView our own child widgets can't paint over).
    void showToast(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide)
    void hideToast();                                   // dismiss it now (e.g. the content view takes over)

    // The async local-library scan (MainWindow, at startup) installed a fresh index: refresh the current
    // level so the "Local Library" folder appears / updates. Cheap no-op if that root isn't showing.
    void onLocalLibraryChanged();

    // The index a multi-album MUSIC queue should be built from (issue #194, increment 2): the MERGED one
    // while the merge is active — so "Play all" queues the discography the artist page is showing, across
    // suppliers — otherwise the one supplier that owns the key, which is what MusicSupply::indexFor answers
    // and what every single-source install gets. Public because MainWindow::openMusicQueue builds the queue
    // and this view is the only thing that knows whether a merge is in play.
    const MusicLibrary::Index& musicIndexForArtist(const QString& artistKey);

    // The async MUSIC scan (MainWindow::rescanMusicLibrary) installed a fresh index (#74): refresh whichever
    // of the three Music levels is showing. Cheap no-op anywhere else — see the definition for why this one
    // does NOT fall back to a loadTop() the way its video twin above does.
    void onMusicLibraryChanged();

    // The async AUDIOBOOK scan (MainWindow::rescanAudiobookLibrary) installed a fresh index (#139): refresh
    // whichever Audiobooks level is showing. Cheap no-op anywhere else, by the same rule as the music twin.
    void onAudiobookLibraryChanged();

    // ...and the async READING scan (MainWindow::rescanBookLibrary) for #134. Same rule again, so a Books
    // level the user is standing in picks up a finished scan at once and every other level pays nothing.
    void onBookLibraryChanged();

    // The music SOURCE PREFERENCE or the manual match overrides changed (issue #194, Settings). Both decide
    // which copy a merged row is keyed and rendered from, so the cached merge is dropped and whichever music
    // level the user is standing in is rebuilt. Cheap no-op anywhere else.
    void refreshMusicLevels();

    // The Trakt calendar cache changed — a fetch landed, or the account was connected/disconnected. Re-reads
    // TraktClient::cachedCalendar() and refreshes whichever surface is showing. HomeView never touches the
    // network: TraktClient owns the fetch and WRITES the cache, and both statics this reads (cachedCalendar /
    // calendarAvailable) are the only Trakt API this class needs, so no client pointer is lent here.
    //
    // Also the DISCONNECT path: after it, calendarAvailable() is false, traktCal_ is emptied, and the shelf
    // and folder disappear on the same refresh — there is no state left behind to be shown by a later render.
    void onTraktCalendarChanged();

    // The same, for the watchlist/collection caches. Kept SEPARATE from the calendar signal rather than
    // folded into one "Trakt changed": the two are written by different fetches on different cadences, and
    // a single handler would rebuild the level the user is standing in every time the OTHER one landed.
    void onTraktListsChanged();

    // "Something is playing", the classic surface's half (#193 increment 4). The host hands this the track
    // that is playing with its now-playing page closed, or "" when nothing is — one string carrying both the
    // text and the visibility, exactly as the themed root's `backgroundTrack` does. The chip is a free-floating
    // overlay child (never in a layout), so it cannot reflow the grid, and it takes NO focus, so the D-pad ring
    // stays the browse ring. See the definition.
    void setNowPlayingTrack(const QString& track);
    // The chip AS RENDERED — its text when it is actually on screen, "" when it is not. For the uitest
    // snapshot: asking the WIDGET rather than the host is the whole point, because the claim being checked is
    // that the sign appeared, not that a string was handed to something.
    QString nowPlayingChipText() const;

signals:
    // The now-playing chip was clicked: take the user back to what they are listening to.
    void nowPlayingActivated();
    void toastRequested(const QString& text, int ms); // ask MainWindow to show a window-level notice
    void toastHideRequested();                        // ask MainWindow to dismiss it
    void openItem(const MediaItem& item);
    void downloadItem(const MediaItem& item); // a resolved file to download for keeps (-> Recents)
    // A chapter's pages, from whichever addon declared the `pages` resource (#188), plus the chapters either
    // side of it in the list it came from. AddonPage rather than a bare url list because a page may need
    // request headers to fetch at all — many image CDNs gate on a Referer, and there was nowhere to put one.
    void openImagePages(const QString& title, const QString& key, const QVector<AddonPage>& pages,
                        const ChapterRun& run);
    // "Read online" was chosen on an OPDS book whose server offers OPDS-PSE (#153). The item carries its
    // page template (MediaItem::pse) and this catalog's device-local Authorization header, and MainWindow
    // turns both into the page list the seam above already knows how to open. A SEPARATE signal from
    // openItem because the two verbs are genuinely different endings of the same row: this one streams
    // pages, that one downloads the volume — and the download must stay reachable, which is the point.
    void readOpdsPseRequested(const MediaItem& item);
    void requestOpenFile(const QString& kind); // "video" | "audio" | "document" | "game"
    void openRecent(const QString& path, const QString& kind, const QString& resumeKey,
                    const QString& title, const QString& thumb); // re-open a "Recent" tab entry
    // Start a channel over this (video/audio) playlist: MainWindow owns the shuffle bag + the EOF-chain, and
    // calls back into playChannelItem() to air each pick through the same per-entry open path a row uses.
    void startChannelRequested(const QString& playlistId);
    // Async outcome of a channel pick's /stream resolve (see openResolvedItem). `gen` lets MainWindow drop a
    // result whose airing was superseded (a later pick, a manual play, or channel exit). Resolved carries the
    // now-playable item; Detoured means it produced no stream (skip it).
    void channelPickResolved(int gen, const MediaItem& item);
    void channelPickDetoured(int gen);
    // At the home root there's nowhere further back -> the host opens the app pause menu (one Back rule).
    void backRequested();
    void settingsRequested();                  // the "Settings" button in the top bar
    void switchProfileRequested();             // the profile button in the top bar
    void themeChanged(const QColor& background, const QColor& accent); // active tab colour changed
    // Outcome of requestNextSource(): ok=true means a new source is being opened (openItem follows); ok=false
    // carries a message to show the user. Surfaced by MainWindow over the player/reader (HomeView is hidden).
    void nextSourceResult(bool ok, const QString& message);
    // "Choose source…" was activated on this catalog item (themed action row or the classic detail button).
    // MainWindow owns the picker: it also owns the BingeStore the choice is remembered in.
    void chooseSourceRequested(const MediaItem& item);
    // A retro game leaf asking "what hacks exist for this?". MainWindow turns it into the list, the
    // confirm and the install — the same shape as chooseSourceRequested above.
    void romhacksRequested(const MediaItem& item, const QString& systemId);
    // A game leaf that a NATIVE PORT is bound to, asking to run on it (issue #233). MainWindow owns the
    // confirm and the install-and-launch, the same shape as romhacksRequested above. `portId` is the
    // NativePorts catalog id, resolved while the row index was still valid.
    void nativePortRequested(const MediaItem& item, const QString& portId);
    // "Fix info…" was activated on the classic detail card (issue #24). Carries the item's MetaCache key (the
    // same identity the override store files against) AND what the providers said about it, because the
    // editor shows each correction over the value it replaces — and the live reply is richer than the cache.
    // MainWindow owns the nav-kit editor loop.
    void editMetadataRequested(const QString& metaKey, const MediaDetail& scraped);
    // A browse/detail level was POPPED (classic Back, or the themed column's Back). MainWindow uses this to
    // invalidate a "Choose source…" fan-out started from the page being left: the themed detail pop bumps the
    // generation itself, but the classic stack is invisible to MainWindow, so without this a slow reply from
    // Show A's page pops its picker over Show B — and the busy latch keeps Show B answering "Still finding
    // sources…" until it lands.
    void browseLevelPopped();
    // Play a local ALBUM (#74). `albumKey` is a MusicLibrary album key; `startPath` is the track to begin on,
    // or empty for the top. MainWindow turns it into ONE PlaybackSession queue — the same queue folder
    // playback, shuffle, channel mode and playlists already run through — so nothing about the player has to
    // learn what an album is. Carries the KEY rather than the track list because the list belongs to the
    // index, and rebuilding it at the play site is what keeps disc/track order stated in exactly one place.
    void playMusicAlbumRequested(const QString& albumKey, const QString& startPath);
    // Play a MULTI-ALBUM music queue: one artist's whole discography (`artistKey` set) or the whole library
    // (`artistKey` empty), in index order or shuffled across the lot. Same contract as the album signal above
    // and for the same reason — the key travels, not the track list, so the order stays stated once in
    // MusicLibrary's index and the queue is built at the play site into the ONE PlaybackSession.
    void playMusicQueueRequested(const QString& artistKey, bool shuffle);
    // Play a local AUDIOBOOK (#139). `bookKey` is an AudiobookLibrary book key; `startPath` is the part to
    // begin on, or empty for the top. Same contract as the album signal above and for the same reason: the
    // key travels rather than the file list, so a book's order stays stated once — in the index — and the
    // queue is built at the play site into the ONE PlaybackSession. A multi-file book is therefore an
    // ordinary queue, which is what makes it resume across a file boundary without a player that knows what
    // a book is.
    // `startSec` < 0 is "wherever the marks say" and is what every route but one passes; the chapter list
    // (#139 increment 2) passes a real offset into `startPath`, and 0 there means the top of that part.
    void playAudiobookRequested(const QString& bookKey, const QString& startPath, int startSec);
    // #193 increment 2: the MOUSE route to the queue verbs — a right-click on a music row in the classic
    // grid. Carries the items_ row rather than the target, because the menu it opens is a nav-kit NavMenu
    // (a nested event loop) that MainWindow owns, and MainWindow re-asks for the target on the far side.
    void browseQueueMenuRequested(int itemsRow);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override; // tune the grid's wheel-scroll speed
    void paintEvent(QPaintEvent* event) override;           // draw the theme background image, if any
    void resizeEvent(QResizeEvent* event) override;         // keep the toast centred

private slots:
    void onItemActivated();
    void onCatalogReady(int requestId, const MediaCatalog& cat); // async result from AddonManager
    void prefetchThemedGames(); // entering a game console -> background-scrape + cache all its games
    void onMetaReady(int requestId, const MediaDetail& detail);  // async detail-header metadata
    void doSearch();

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
        QVector<MediaItem> synthItems; // a shelf level's resolved intersection (favorites/tag/hidden), re-shown on Back
    };

    // The classic detail page's action gates for an item at the CURRENT drill level (stack_.last().addon is
    // the resolving addon): which of Play(/Read) and Download the item offers, and whether "play" means Read.
    // ONE definition — requestMeta (the classic page's buttons) and themedDetailData (the themed action row's
    // verbs) both call this, so the themed row can never drift from the classic visibility rules.
    struct ActionGates { bool play = false; bool download = false; bool readable = false; };
    ActionGates classicActionGates(const MediaItem& item) const;

    // True when playing this leaf goes through the Stremio stream add-ons — i.e. there is a LIST of releases
    // to choose between, which is the only case "Choose source…" means anything. A local-library item plays
    // its own file (prefer-local short-circuits every resolve), and a direct file has nothing to choose.
    // Shared by the classic detail button and the themed action row, like classicActionGates.
    bool canChooseStreamSource(const MediaItem& item) const;
    // Record the bridged Stremio stream id /meta produced onto the stored browse row, so the gates AND the
    // item requestChooseSource emits both carry it. True when it newly landed (i.e. the verbs may have changed).
    bool bridgeStreamId(int browseIndex, const QString& streamId);
    // (The bingeGroup already chosen for a stream id's series is BingeStore::preferredGroup — one definition
    // shared with MainWindow's next-episode hand-off, which had a verbatim copy of it.)

    void selectType(LoadedAddon* addon, const QString& catalogId, const QString& type, const QString& name);
    void showCarousel();             // show the media-type carousel landing (carousel layout)
    void showXmb();                  // show the PS3 XMB layout (categories + item column)
    void activateItem(int row);      // open/drill a catalog item by row (shared by grid + carousel)
    void openDetailLevel(LoadedAddon* addon, const MediaItem& it); // push + show an item's detail page
    void fillCarouselFromItems(int from); // (re)build/extend the carousel from items_[from..]
    void fillXmbFromItems(int from);      // (re)build/extend the XMB item column from items_[from..]
    void selectRecent();             // show the local "recently opened" list (not an addon catalog)
    // The synthetic Photos category (#102): a top-level browser over the configured photo library. Offered
    // only when PhotoLibrary::hasImages(root); folders drill into their image grid, a photo opens the viewer.
    void selectPhotos();             // enter the Photos category (a synthetic top-level, no addon)
    void populatePhotos();           // (re)build its top level (folder rows, or a flat grid) from a fresh scan
    void openPhotoFolderLevel(const QString& folder); // drill a folder row -> its image grid
    void populatePhotoFolder(const QString& folder);  // (re)build that folder's grid from a fresh scan
    // The synthetic Music category (#74): Artists -> that artist's Albums -> that album's Tracks, over
    // MusicLibrary's installed index. Offered whenever MusicLibrary::hasLibrary() — i.e. as soon as the
    // configured root exists, not only once tracks were found, so the empty and still-scanning cases have
    // somewhere to explain themselves (musicEmptyReason). Nothing here rescans; MainWindow owns the scan.
    // The synthetic AUDIOBOOKS category (#139): Authors (plus Narrators and Series doors) -> that bucket's
    // books -> one book's parts, over AudiobookLibrary's installed index. Offered whenever
    // AudiobookLibrary::hasLibrary(), the same rule the Music tab follows. Nothing here rescans.
    void selectAudiobooks();
    void populateAudiobooks();                              // (re)build the root from the installed index
    void openAudiobookAuthorLevel(const QString& authorKey);
    void populateAudiobookAuthor(const QString& authorKey);
    void openAudiobookNarratorsLevel();
    void populateAudiobookNarrators();
    void openAudiobookNarratorLevel(const QString& narratorKey);
    void populateAudiobookNarrator(const QString& narratorKey);
    void openAudiobookSeriesListLevel();
    void populateAudiobookSeriesList();
    void openAudiobookSeriesLevel(const QString& seriesKey);
    void populateAudiobookSeries(const QString& seriesKey);
    void openAudiobookBookLevel(const QString& bookKey);
    // The book's CHAPTERS as a NavMenu over the current screen (#139 increment 2) — an .m4b's atoms or a
    // folder's parts, whichever the book is, with the row the listener is standing in marked and preselected.
    // Not a level: it is a jump you make and leave. See the definition.
    void openAudiobookChapters(const QString& bookKey);
    void populateAudiobookBook(const QString& bookKey);

    // The synthetic BOOKS category (#134): Authors (plus a Series door) -> that bucket's books, over
    // BookLibrary's installed index. Offered whenever BookLibrary::hasLibrary(), the same rule the Music and
    // Audiobooks tabs follow. ONE LEVEL SHORTER than the audiobook family on purpose - one file is one book,
    // so a book row is a leaf that opens its reader rather than a container. Nothing here rescans.
    void selectBooks();
    void populateBooks();                              // (re)build the root from the installed index
    void openBookAuthorLevel(const QString& authorKey);
    void populateBookAuthor(const QString& authorKey);
    void openBookSeriesListLevel();
    void populateBookSeriesList();
    void openBookSeriesLevel(const QString& seriesKey);
    void populateBookSeries(const QString& seriesKey);

    void selectMusic();                                // enter the Music category (synthetic, no addon)
    void populateMusicArtists();                       // (re)build the artist list from the installed index
    void openMusicArtistLevel(const QString& artistKey); // drill an artist row -> their albums
    void populateMusicArtist(const QString& artistKey);
    void openMusicAlbumLevel(const QString& albumKey);   // drill an album row -> Play album + its tracks
    void populateMusicAlbum(const QString& albumKey);
    // The classical view (#196, part 2): the "Composers" entry row -> a composer -> one of their works.
    // Three more levels in exactly the shape of the three above, so Back and a finished rescan handle them
    // by the same rules; a work's tracks are ordinary track rows and route through the same player.
    // Subsonic music servers (#193, increment 5): the Music root's "Music Servers" door -> the saved-server
    // shelf -> one server's ARTISTS, and from there the SAME artist/album/track levels the local library
    // uses. render* are split out of populate* because a remote level renders twice - once as "Loading..."
    // and once when its one request lands - and a fetch that lands after the user has navigated away must
    // not overwrite the level they are now standing in (musicFetchGen_).
    void removeMusicServerInteractive(const QString& serverId, const QString& name); // long-press a server row
    void openMusicServersLevel();
    void populateMusicServers();
    void openMusicServerLevel(const QString& serverId);
    void populateMusicServer(const QString& serverId);
    void renderMusicServer(const QString& serverId);
    void renderMusicArtist(const QString& artistKey);
    void renderMusicAlbum(const QString& albumKey);
    void showMusicServerError(const QString& title, const QString& why);
    void showMusicLoading(const QString& title);
    void scheduleMusicArtRefresh();
    void prefetchAlbumCovers(const QVector<MusicLibrary::Album>& albums);
    // ---- ONE LIBRARY ACROSS SOURCES (issue #194, increment 1) ------------------------------------------
    // The Music ROOT unifies the local library and every configured Subsonic server into one artist list, so
    // somebody who owns the same records in both places sees one row rather than two. Everything below it is
    // rendered by the SAME three builders — there is still no second artist list, album row or track row.
    //
    // TWO GATES, and both matter:
    //   musicMergePossible()  there are at least two suppliers AT ALL. False => every level below is built
    //                         exactly as it was before this feature existed, which is what makes "a
    //                         single-source install is unchanged" a structural claim rather than a hope.
    //   musicMergeActive()    ...and we are not standing INSIDE a particular server's shelf. Someone who
    //                         walked in through the "Music Servers" door asked for THAT server, and answering
    //                         with a merged view would be answering a different question.
    bool musicMergePossible() const;
    bool musicMergeActive() const;
    bool insideMusicServerLevel() const;
    void rebuildMergedMusic();                          // recompute mergedMusic_ from the live suppliers
    void applyMusicRemap();                             // ...and move what was banked onto the new pick
    void applyMusicStreamRekey(const QString& albumKey); // #204: off the signed url, onto the track's own name
    void fetchMergeSources();                           // one getArtists per server, at most once per session
    // An instance key -> the key its merged row is actually rendered under. Identity when nothing merged.
    QString mergedArtistPrimary(const QString& key) const;
    QString mergedAlbumPrimary(const QString& key) const;
    QString musicSourceLabel(const QString& sourceId) const;
    browse::MusicAlbumSources musicAlbumSourcesFor(const QString& albumKey) const;
    void playMusicAlbumFromSource(const QString& albumKey);   // a "Play from ..." row: fetch first if remote
    // "Play all" / "Shuffle all" on an artist (issue #194, increment 2). Same shape as the row above and for
    // the same reason: an artist whose records live on a server has no track lists until somebody asks for
    // them, and a queue built from those albums would be empty — the row would look like it did nothing.
    void playMusicArtistQueue(const QString& artistKey, bool shuffle);
    void unmergeAlbumInteractive(const QString& albumKey);    // "these are NOT the same album"
    void mergeAlbumInteractive(const QString& albumKey);      // "this IS the same album as..."

    void openMusicComposersLevel();
    void populateMusicComposers();
    void openMusicComposerLevel(const QString& composerKey);
    void populateMusicComposer(const QString& composerKey);
    void openMusicWorkLevel(const QString& workKey);
    void populateMusicWork(const QString& workKey);
    // Empty text when the index has content; else the sentence + the folder it is about.
    browse::MusicEmptyNote musicEmptyNote() const;
    // Why the Audiobooks category is empty, in the user's terms — the twin of musicEmptyNote, and separate
    // for the same reason the roots are: the sentence has to name the audiobook folder.
    browse::AudiobookEmptyNote audiobookEmptyNote() const;
    // ...and the same for the reading library, separate for the same reason: the sentence has to name the
    // books folder.
    browse::BookEmptyNote bookEmptyNote() const;
    // ---- The ONE PC Games folder (it replaced the Steam / Epic / GOG / Battle.net folders) ---------------
    //
    // Those four showed the same game up to five times under unrelated ids, so a favourite or 40 hours of
    // play time attached to whichever copy you happened to launch it from. There is now one folder built by
    // browse::pcGamesCatalog, one item per game, and every way to launch it carried as a source on that item.
    void openPcGamesConsole(const MediaItem& consoleItem); // drill the synthetic PC Games console
    // (Re)build it natively. `runRemap` is what a REFRESH does and a mere QUERY CHANGE does not: the
    // in-folder search box is debounced at 300 ms and repopulates on every keystroke, and the remap's input
    // is the LIBRARY, which cannot have changed because the user typed a letter. Running it there was an ini
    // pass per keystroke, unbounded in the size of the library, for a table identical to the one the last
    // pass applied. It still runs on every genuine refresh (entering the folder, Back into it, the owned-list
    // re-present), which is what makes a reinstalled game migrate the moment it reappears.
    void populatePcGames(bool runRemap = true);
    bool atPcGamesConsole() const;                         // the top level is still that console

    // The library the folder is built from, gathered in ONE place: the four launcher scans, the downloaded
    // copies and the TTL-cached Steam owned list. Both the folder and the re-derivation below call this, so
    // a tile's sources and a re-derived game's sources cannot come from different ingredients.
    //
    // `pre` hands in a scan the caller ALREADY did (populatePcGames needs the same four launcher scans to
    // build the remap's candidate ids) instead of making this repeat it — that was two full scans of every
    // launcher per refresh. Null means "scan here", which is what the re-derivation path passes.
    MediaCatalog pcLibraryCatalog(const QString& query, const QString& launcherFilter,
                                  const PcLibScan* pre = nullptr) const;

    // THE RE-DERIVATION PATH. MediaItem::pcSources does not survive persistence and it.url is empty by
    // design, so a stored favourite/recent/search row for a merged PC game carries its ID and nothing else —
    // and unlike "steam:<appid>" that id encodes no launch. Favouriting a merged game, restarting and
    // pressing Play would therefore do NOTHING. Every such surface instead rebuilds the sources from the
    // CURRENT library and runs the same picker.
    //
    // Re-deriving is not a fallback for a missing persist — it is the correct behaviour. Sources are machine
    // state: the Steam copy is uninstalled, the GOG one moves, the download is deleted. A persisted list
    // would launch a game that is no longer there; a re-derived one is true at the moment Play is pressed.
    QVector<pcgame::PcGameSource> pcSourcesForId(const QString& itemId) const;   // rebuild from the library
    QVector<pcgame::PcGameSource> pcSourcesFor(const MediaItem& it) const;       // live field, else re-derive
    // Play a merged PC game: pick a source automatically, or offer a NavMenu of them when the pick is
    // ambiguous (several ready) or unsafe (none ready). Never launches a not-ready source by itself.
    void playPcGame(const MediaItem& it);
    // Launch ONE chosen source, routed by kind through the launcher path that already exists for it.
    void launchPcSource(const MediaItem& it, const pcgame::PcGameSource& s);
    // The launcher filter's menu (issue #44), opened from the folder's own control row. A NavMenu, because
    // three of the four layouts render no widget chrome for a dropdown to live in.
    void showPcLauncherFilterMenu();

public:
    // THE MERGE OVERRIDE, offered from the entry it is about (issue #44) — split a tile that is two games,
    // fuse two tiles that are one, or undo a previous verdict. Synchronous (NavMenu::pick / NavConfirm::ask,
    // the addGameToPlaylistInteractive idiom) so the caller learns whether anything changed; returns true
    // only when a verdict was written. Nothing is repopulated here — see refreshAfterPcMergeFix.
    bool fixPcGameEntry(const MediaItem& it);
    // Addressed by ID, never by a row index: the themed action row has to DEFER this verb by an event-loop
    // turn (it rebuilds the browse model under the delegate that is still emitting), and browseRowMap_ can
    // be rebuilt in that window. Resolve the index with pcGameIdAt while it is still valid, then fix by id.
    QString pcGameIdAt(int browseIndex) const;
    bool fixPcGameEntryById(const QString& itemId);
    // Show the result. Separate replaces the entry with one per copy, so a page showing that entry has to be
    // LEFT rather than refreshed in place.
    void refreshAfterPcMergeFix();
    // Re-scan after a ROM lands in the library (a romhack install). refreshAfterPcMergeFix is the PC-GAMES
    // merge refresh and does nothing for a console game — using it meant an installed hack stayed invisible
    // until the app was restarted, which reads as "nothing happened".
    void refreshAfterRomInstall();
    // #248: re-derive the Recomps section's row states, but only while it is the level on screen. PUBLIC
    // because the verbs that change those states (Remove, and an install that completes) live in MainWindow,
    // and a row still reading "installed" after the folder was deleted is indistinguishable from a Remove
    // that silently did nothing. A no-op anywhere else, so the caller never has to ask where it is.
    void refreshRecompsIfShown();
    // Prompt for a name + playlist URL and save the source; true if one was added. PUBLIC because the Live TV
    // shelf hides itself until a source exists, which would otherwise leave no way to add the first one —
    // Settings calls this, and the shelf appears on the next home rebuild.
    bool promptForLiveTvSource();
private:

    // Playlists: category-scoped (video/audio/game/reading). A "Playlists" folder shows at the category level
    // and at every catalogue root of that category; these drive its synthetic (addon-less) levels. catalogKey
    // identifies a catalogue ("addonId|catalogId|catalogType"); currentCategoryKey() maps the current
    // catalogue's type to its bucket (the key playlists actually filter/create on).
    QString currentCatalogKey() const;                   // key for the catalogue at the root of the browse stack
    QString currentCategoryKey() const;                  // the bucket the current catalogue classifies into
    LoadedAddon* addonForKey(const QString& catalogKey) const; // the catalogue's source addon (null if native)
    void populatePlaylists(const QString& categoryKey);  // (re)build that list (each playlist + a New entry)
    void openPlaylistLevel(const QString& playlistId);   // drill a playlist -> its items
    void populatePlaylistItems(const QString& playlistId); // (re)build a playlist's items as openable rows
    void createPlaylistInteractive(const QString& categoryKey); // prompt for a name + create, refresh the list
    void addItemToPlaylistInteractive(const MediaItem& it);    // pick/create a playlist, add this item to it
    // ---- Live TV (#75 inc 2): saved IPTV sources -> a browsable channel shelf ----------------------------
    void openLiveTvSourcesLevel();                             // drill Home's "Live TV" folder -> the sources shelf
    void populateLiveTvSources();                              // (re)build it: one row per source + an "add" row
    void openLiveTvChannelsLevel(const QString& sourceId);     // drill a source -> its channels (fetched fresh)
    void populateLiveTvChannels(const QString& sourceId);      // re-show a source's channels (cache, else fetch)
    void fetchLiveTvChannels(const IptvSource& src);           // GET/read the playlist -> parse -> cache -> show
    void showLiveTvError(const QString& name);                 // a readable one-row failure, never a crash
    // ---- Live TV EPG (#75 inc 3) -------------------------------------------------------------------------
    void showLiveTvChannels(const IptvSource& src);            // render liveTvEntries_ with now/next + a Guide row
    void fetchLiveTvEpg(const IptvSource& src, const QString& headerTvgUrl); // resolve+fetch(daily-cache)+parse EPG
    void openLiveTvGuideLevel(const QString& sourceId);        // drill the "Guide" row -> the channels×today grid
    void populateLiveTvGuide(const QString& sourceId);         // (re)build the grid without pushing a level (Back)
    void addIptvSourceInteractive();                           // OSK name + URL -> save the source, refresh
    void removeIptvSourceInteractive(const QString& sourceId, const QString& name); // confirm -> remove, refresh
    void toggleLiveTvChannelFavorite(const MediaItem& it);     // star/unstar a channel (FavoritesStore "livetv")
    // ---- Recomps (#248 inc a): the browse surface over the native-port catalogue #233 ships ----
    void openRecompsLevel();                                   // drill Games' "Recomps" folder -> the section
    void populateRecomps();                                    // (re)build it: a header per system + its ports
    // ---- OPDS book catalogs (#146): saved book servers -> a browsable feed shelf -> download+open a book ----
    void openOpdsCatalogsLevel();                              // drill Reading's "Book Servers" folder -> the shelf
    void populateOpdsCatalogs();                               // (re)build it: one row per catalog + an "add" row
    void openOpdsCatalog(const QString& catalogId);            // open a saved catalog -> fetch its ROOT feed
    void openOpdsFeedLevel(const QString& feedUrl, const QString& title); // drill a sub-feed row (same auth)
    void fetchOpdsFeed(const QString& catalogId, const QString& feedUrl, const QString& title); // GET(+auth)->render
    void populateOpdsFeed(const QString& catalogId, const QString& feedUrl, const QString& title); // Back: re-fetch
    void showOpdsError(const QString& title);                  // a readable one-row failure, never a crash
    void addOpdsCatalogInteractive();                          // OSK name+URL+optional user/pass -> save, refresh
    void openOpdsBook(const MediaItem& it);                    // attach device-local auth, then download+open
    void chooseOpdsBookAction(const MediaItem& book);          // #153: "Read online" beside "Download"
    // A playlist row's action menu (Open / Play random / Rename / Delete) — the game-item-menu NavMenu precedent.
    void showPlaylistMenu(const QString& playlistId);
    void playRandomFromPlaylist(const QString& playlistId);    // uniform pick -> the shared per-entry open path
    void renamePlaylistInteractive(const QString& playlistId); // OSK prefilled with the current name -> rename
    void deletePlaylistInteractive(const QString& playlistId); // Cancel-focused confirm -> remove
    // Saved filter presets (#63): extract one game's facts for the pure evaluator, and the "＋ New filter…"
    // manager (create via a sequence of nav-kit picks + OSK name; rename/delete an existing preset).
    gamefilter::GameFacts gameFactsFor(const MediaItem& it) const;
    void createFilterPresetInteractive();                      // the manager NavMenu (create / rename / delete)
    void buildFilterPreset();                                  // walk the dimension picks + OSK name -> save
    void renameFilterPresetInteractive(const QString& name);
    void deleteFilterPresetInteractive(const QString& name);
    // Open one item through the per-entry resolution path (shared by activateItem's tail + Play-random). When
    // forChannel, a would-be detail-page open is SUPPRESSED and reported (Detoured / channelPickDetoured) so the
    // channel skips the pick instead of dumping the viewer on an info page; channelGen tags the async result.
    ChannelAir openResolvedItem(const MediaItem& it, LoadedAddon* levelAddon,
                                bool forChannel = false, int channelGen = 0);

    // Recents: every catalogue that has any matching recents shows a "Recent" folder at the top; opening a
    // row re-opens it at its saved position. The kind is the catalogue's bucket mapped to a RecentItem kind.
    QString catalogRecentKind() const;                   // "video"|"audio"|"document"|"game" for this catalogue
    QString catalogReadingForm() const;                  // "book"|"comic"|"manga" for a reading catalogue, else ""
    void openRecentsLevel(const QString& kind);          // drill the Recent folder -> the matching recents
    void populateRecents(const QString& kind);           // (re)build that list as re-openable rows
    // The synthetic "Downloaded" folder: fully-downloaded items of this catalogue (per-console for games).
    // marker = "downloads:<kind>|<system>" (system empty for non-games, a SystemCatalog id / "pc" for games).
    void openDownloadsLevel(const QString& marker);      // drill it -> the matching downloads
    void populateDownloads(const QString& marker);       // (re)build that list as re-openable rows
    // The synthetic "Local Library" folder (video category only): this machine's scanned local videos.
    void openLocalLibraryLevel(const QString& marker);   // drill it -> the scanned local videos
    void populateLocalLibrary(const QString& marker);    // (re)build that list from the cached index
    // The synthetic "Airing Soon" folder + Home shelf (video category only): the connected Trakt account's
    // episodes airing in the next week. EVERY one of these is gated on traktCalendarItems() being non-empty,
    // which is itself false whenever TraktClient::calendarAvailable() is false — so an install that never
    // heard of Trakt gets no folder, no shelf, no header and no placeholder anywhere.
    MediaCatalog traktCalendarItems() const;             // the built catalog, or an EMPTY one when Trakt is off
    void openTraktCalendarLevel();                       // drill it -> the episodes airing soon
    void populateTraktCalendar();                        // (re)build that list from the cached calendar
    // The synthetic "You Missed" folder + Home shelf (issue #25): the complement of the two above, over the
    // SAME cached calendar — episodes of your followed shows that already aired, are still inside the
    // lookback window, are unwatched and are not dismissed. One row per show.
    //
    // Gated identically, and that is the whole answer to "what does a user who never linked Trakt see":
    // nothing. traktMissedItems() is an empty catalog whenever calendarAvailable() is false, an empty
    // catalog draws no shelf and no folder row, so the surface does not exist rather than existing and
    // being permanently, unexplainedly empty. It is also absent when Trakt IS linked and there is simply
    // nothing missed, which is the same rule and the right one: this shelf is a to-do list, and an empty
    // to-do list is not a thing to draw.
    //
    // `maxRows` <= 0 is uncapped (the folder); the shelf passes trakt::kMissedShelfMax.
    MediaCatalog traktMissedItems(int maxRows) const;
    void openTraktMissedLevel();                         // drill it -> every missed show
    void populateTraktMissed();                          // (re)build that list from the cached calendar
    // Activating a "You Missed" row opens a small menu rather than playing straight away — Play is row 0,
    // so the couch gesture is still "press twice". The second row is the dismissal, and a menu is how it
    // becomes reachable: the app has four layouts and a D-pad has no second button to bind, whereas a
    // NavMenu from the nav kit is controller/keyboard/mouse navigable in all of them. Same pattern, and
    // the same reasoning, as the Recent/Downloads game menu below.
    void showTraktMissedMenu(MediaItem it);
    // The synthetic "Trakt Watchlist" / "Trakt Collection" folders (video category only). Gated exactly as
    // the calendar is: the builder returns an EMPTY catalog whenever Trakt is not configured+connected, and
    // an empty catalog means no folder at all — no row, no placeholder, no "connect Trakt" hint.
    //
    // `which` is "watchlist" or "collection" and rides the level marker, so Back repopulates the right one.
    MediaCatalog traktListItems(const QString& which) const;
    // The same gate + admissibility rule, answering only "is there anything to draw" — which is all the
    // folder list asks, on every navigation into the video root. Building the catalog to test emptiness
    // sorted the whole watchlist (thousands of rows) to compare a size against zero.
    bool traktListHasRows(const QString& which) const;
    void openTraktListLevel(const QString& which);
    void populateTraktList(const QString& which);
    void openFavoritesLevel(const QString& system);      // drill a console's Favorites folder -> its favourited games
    void populateFavorites(const QString& system);       // (re)build that list of favourited games for the console
    // A console's Homebrew folder: the same synthetic-level shape as Favorites above, but fetched from every
    // configured server rather than read from a store, so the level is built asynchronously and one page at a
    // time. `more` empty means "the first page from every server"; otherwise it is the per-server
    // continuations the "More…" row carried.
    void openHomebrewLevel(const QString& system);       // drill the folder -> that console's homebrew
    void populateHomebrew(const QString& system);        // Back: re-fetch page one (the level keeps its marker)
    void fetchHomebrew(const QString& system, const QVector<HomebrewMore>& more, bool append);
    void showHomebrewPage(const QString& system);        // render homebrewRows_ (+ a trailing "More…" row)
    // Marks shelves (Favorites / pinned-tag / Hidden): each drills into a synthetic catalog of the CURRENT
    // level's items that match, snapshotted into the pushed Level (re-shown on Back, no re-fetch).
    void openShelfLevel(const MediaItem& folder);        // drill a shelf folder -> its matching items
    QVector<MediaItem> shelfMatches(const MediaItem& folder) const; // current-level items matching the shelf
    bool passesBrowseFilter(const MediaItem& it) const;  // the transient browse filter membership test
    void requestSteamMeta(const MediaItem& item, int reqId); // native detail fetch for a Steam game
    QWidget* detailActionButton() const; // the focusable action on the detail page (Play for Steam, else Favorite)
    void renderRecents();            // populate the grid from RecentStore + favourites, grouped under headers
    void openFavorite(const MediaItem& favItem); // open a favourited item's detail page from Home
    void showItemContextMenu(int row, const QPoint& globalPos); // Home: remove a Recent/Favorite entry
    // The Recent/Downloads game action menu (Play / Favorite / Add to playlist / Uninstall) — a NavMenu
    // overlay from the nav kit; `isDownloads` = the item lives in the Downloaded store.
    void showGameItemMenu(MediaItem it, bool isDownloads);
    void toggleGameFavorite(const MediaItem& it);        // star/unstar a local game (re-opens by path)
    void addGameToPlaylistInteractive(const MediaItem& it); // add a local game to a playlist (path-based entry)
    void uninstallGameItem(const MediaItem& it, bool isDownloads); // delete the file (if ours) + drop from stores
    void applyGridMode(bool recentList); // switch grid_ between the catalog poster grid and the recent list
    void styleTypeButtons(const QString& activeKey); // colour the top tabs + tint the catalogue background
    void applyThemeFont();           // set the app font family/scale from the active theme
    void layoutMetaSections(const QString& itemType); // declarative detail-page arrangement from the theme
    // The ONE seam every internal focus assignment goes through. The classic HomeView stays LIVE while a
    // themed (QML) page is on screen — it is the data engine behind it — so its own focus calls must never
    // reach across and take the keyboard off the visible page. See the definition for why "hidden" is the
    // right gate and what a leaked focus does to a themed page.
    void takeFocus(QWidget* w);
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
    // The ONE ingress every row of items_ passes through: composites the user's correction (issue #24) onto
    // the row and keeps the pre-correction copy, which scrapedRow() hands back to the metadata editor as its
    // baseline. Used by populate() (catalogs + search) and renderRecents() (recents, favourites, Trakt).
    MediaItem correctedRow(const MediaItem& src);
    // key -> the row as the providers gave it, for rows that carry a correction. Bounded by the corrections
    // on screen: an uncorrected row is not stashed, because the composite left it untouched.
    QHash<QString, MediaItem> preCorrection_;
    // Show a locally built (addon-less) catalog level: reset paging state and hand it to the grid. Shared
    // boilerplate for the three synthetic levels below (Recent/Downloaded/Favorites).
    void showSyntheticCatalog(const MediaCatalog& cat);
    void loadThumbnails(int fromIndex);    // queue posters for items_[fromIndex..]
    void pumpThumbnails();                 // start queued poster loads up to the concurrency cap
    void requestMeta(const MediaItem& item); // fetch + show the detail-header metadata for item
    // Manual action (issue #89): show/hide the "📖 Manual" button for the open detail item (a game whose
    // scraped bundle carries a manual URL, or that already has one on disk), and — on click — fetch it on
    // demand (progress in the button label) then hand the local file to the shared reader-open path.
    void refreshManualButton(const MediaItem& item);
    void openManualFor(const QString& key, const QString& title);
    // Paint the classic detail card. `fromProvider` = this is the source's own answer (so it becomes the
    // baseline the metadata editor corrects); false for a re-render or the offline cached fallback. Either
    // way the user's correction is composited on top before anything is drawn.
    void showMeta(const MediaDetail& scraped, bool fromProvider = true);
    void showMetaComposited(const MediaDetail& detail);   // the painter; `detail` is already composited
    // The open classic card as the PROVIDERS gave it (issue #24: the editor's baseline and the reset target),
    // stamped with the item it is for. KEYED, not bare: the reply is written only when one ARRIVES, so an
    // item whose addon returns nothing would otherwise be edited against the PREVIOUS item's card — see
    // ScrapedSnapshot.h for the whole failure and why the key lives beside the value.
    MetaEdit::ScrapedSnapshot scrapedDetail_;
    // The same, for the THEMED detail card: the /meta reply that card was enriched from, stamped with its
    // row's key. themedScrapedValues() reads it; MainWindow's "Fix info…" verb passes the result in.
    MetaEdit::ScrapedSnapshot themedScraped_;
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
                       QString catalogId, type, name; bool photos = false;    // the synthetic Photos category (#102)
                       bool music = false;                                    // the synthetic Music category (#74)
                       bool audiobooks = false;                               // the synthetic Audiobooks one (#139)
                       bool books = false; };                                 // the synthetic My Books one (#134)
    QVector<NavTarget> navTargets_;
    CarouselView* carousel_ = nullptr;
    bool carouselMode_ = false;
    bool atCarouselLanding_ = false; // showing the media-type carousel (the root)
    XmbView* xmb_ = nullptr;
    bool xmbMode_ = false;           // active theme layout is "xmb" (PS3 XrossMediaBar)
    bool atXmbRoot_ = true;          // at a category's top level (Left/Right switch categories)
    QString lastMediaKey_;           // last media type entered (to re-highlight on return to the carousel)
    QListWidget* grid_ = nullptr;
    QString browseSelectKey_; // url/id to re-select after the next themed re-sync (keeps the spot after fav/uninstall)
    int     browseFilterMode_ = 0;   // transient browse filter: 0 All, 1 Favorites, 2 Status, 3 Tag (see setBrowseFilter)
    int     browseFilterComp_ = 0;   // ItemMarks::Completion (cast to int) when browseFilterMode_ == 2
    QString browseFilterTag_;        // the tag when browseFilterMode_ == 3
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
    // #193 increment 4: the "something is playing" chip, bottom-left. Built on first use (there is nothing to
    // say until an album is backgrounded) and never added to a layout — see setNowPlayingTrack.
    QPushButton* nowPlayingChip_ = nullptr;
    void positionNowPlayingChip();   // re-anchor it (first show / resize)
    QTimer* searchTimer_ = nullptr;    // debounces live-search as the user types
    QNetworkAccessManager* nam_ = nullptr;
    RaBrowse* raBrowse_ = nullptr;     // RetroAchievements web-API lookup for the themed metadata panel (lazy)
    SteamAchievements* steamAch_ = nullptr; // Steam achievements for installed PC games (Hydra-style, lazy)
    GameMetaAggregator* gameAgg_ = nullptr; // fans out SteamGridDB/IGDB/ScreenScraper/TheGamesDB on hover (lazy)
    QHash<QString, QVariantMap> themedArtCache_; // per-session page cache of resolved panel art/facts by item key
    bool themedResolvedRich_ = false;            // did requestThemedMeta resolve locally? (enrich skips scraping if so)

    // Detail-page metadata header (shown when an item is opened; hidden on top-level catalog views).
    QFrame* meta_ = nullptr;
    QBoxLayout* metaLayout_ = nullptr;  // image<->text arrangement (poster/banner/text)
    QVBoxLayout* metaTextCol_ = nullptr; // the reorderable text column (favorite/title/facts/overview)
    QLabel* metaImage_ = nullptr;
    QWidget* actionRow_ = nullptr;    // holds Play + Favorite on the detail header
    QPushButton* favBtn_ = nullptr;   // ★ toggle on the detail header
    QPushButton* playBtn_ = nullptr;  // ▶ launch button shown on a Steam game's info page
    QPushButton* downloadBtn_ = nullptr; // ⬇ download this item (or, for a series/season, all its content)
    QPushButton* sourceBtn_ = nullptr;
    QPushButton* romhackBtn_ = nullptr;   // "Romhacks…" — retro game leaves only   // 🔀 "Choose source…" — shown only for a Stremio-resolved leaf
    // ⚙ "Fix this entry…" — the PC-game merge override (issue #44), shown only on a merged PC game's page.
    QPushButton* pcFixBtn_ = nullptr;
    QPushButton* editMetaBtn_ = nullptr; // ✎ "Fix info…" — the per-item metadata editor (issue #24)
    QPushButton* manualBtn_ = nullptr;   // 📖 "Manual" — open the scraped game manual (issue #89), on demand
    BingeStore* bingeStore_ = nullptr;   // borrowed from MainWindow (see setBingeStore); may be null
    // Download crawl: walk a container's children, resolve each leaf's source, and emit downloadItem for it.
    // Runs sequentially (one resolve in flight) so it paces itself and reuses the existing async result signals.
    struct DlNode { LoadedAddon* addon = nullptr; MediaItem item; QString parentTitle; QString parentType; };
    QList<DlNode> dlQueue_;
    DlNode dlDetailNode_, dlMetaNode_; // the node whose detail/meta request is currently in flight
    int dlDetailReq_ = -1, dlMetaReq_ = -1;
    int dlQueued_ = 0;                  // files emitted for download this crawl
    bool dlBusy_ = false;
    // Fired once when a crawl drains, with whether it queued anything. The romhack flow needs to know a base
    // ROM download actually STARTED before it arms an install to run when that download lands.
    std::function<void(bool anyQueued)> dlDone_;
    // The romhack verb's leaf, captured when it was PRESSED and shaped as a crawl node — a game leaf resolves
    // by QUERY (its title plus the console), not by its own id, so reaching the base ROM means running the
    // ordinary download crawl rather than resolving a stream directly. Mutable because the verb is also
    // offered from a const query (romhackTargetAt).
    mutable DlNode romhackNode_;
    void startDownload();              // begin a crawl from the current detail item
    void dlNext();                     // process the next queued node
    void dlResolveLeaf(const DlNode& node); // resolve one leaf's source, then continue
    // Queue a resolved file. `headers` is the source's proxyHeaders for `url` — a download is an ordinary
    // HTTP fetch of the stream URL, so a gated source needs them here too (#59).
    void dlEmit(const MediaItem& it, const QString& url, const QString& mime,
                const StreamHeaders::Headers& headers = {});
    // TMDB->IMDB bridge: when a non-Stremio catalog item (e.g. AIO Catalog) supplies an IMDB stream id via
    // getMeta, Play resolves it through the installed Stremio stream addons. Set in showMeta for the open item.
    QString playImdbId_;              // "tt123" (movie) or "ttShow:s:e" (episode), else empty
    QString playStremioType_;         // "movie" / "series"
    // The last playable opened from a file provider (Allarr), so "Issue with Streaming" can re-resolve an
    // alternate source. viaImdb -> resolveStreamByImdb(type,imdbId,n); else resolveStream(addon,item,n).
    struct NextSourceCtx {
        // The file-provider addon on the direct path, held as its MANIFEST ID and never as a LoadedAddon*.
        // This context outlives the open that set it — it is read when the user presses "Issue with
        // Streaming", which may be an hour later — and AddonManager::reload() clears the
        // std::vector<std::unique_ptr<LoadedAddon>> that owns every source, so a pointer parked here is one
        // add-on install/remove away from dangling. sourceById() at the point of use answers null instead,
        // and resolveStream refuses a null src politely on its first line (AddonManager.cpp:2433).
        QString addonId;
        MediaItem item;               // the item to re-open (its id/type drive the re-resolve)
        bool viaImdb = false;         // bridged movies/TV
        QString imdbType, imdbId;     // imdb path: resolveStreamByImdb args
        int attempt = 0;              // last ?n= used (0 = best)
    } lastPlay_;
    int steamMetaSeq_ = -1;           // unique (negative) ids for native Steam meta fetches
    int ownedFetchGen_ = 0;           // in-flight dedup for the async owned-games re-present (only the latest wins)
    // The PC Games folder's launcher filter, and the launchers it can offer. FOLDER STATE, deliberately not
    // persisted: it belongs to this level the way the in-folder search query does, and a filter restored on
    // the next launch would hide most of the library with nothing on screen explaining why. `available` is
    // recomputed from the same scan the folder is built from, so the menu can never offer a launcher this
    // machine has no games in.
    QString     pcLauncherFilter_;
    QStringList pcLaunchersAvailable_;
    // Triple/XMB theme live-meta + inline-play state (see requestThemedMeta()/playThemedLeaf()).
    int themedMetaReq_ = -1;          // in-flight addon /meta id for the live panel beside the cross
    int themedMetaIndex_ = -1;        // the currently-selected browse index (updated on every hover)
    int themedMetaReqIndex_ = -1;     // the index themedMetaReq_ was issued FOR (J09: response must bind to THIS
                                      // row, not the live selection, or a slow /meta paints onto the next row)
    // Live TV (#75 inc 2): the in-session channel cache for the currently open source (so Back and a favourite
    // re-render never re-hit the network), and a generation counter that drops a superseded async fetch.
    QVector<M3uEntry> liveTvEntries_;
    QString           liveTvCacheSourceId_;
    int               liveTvFetchGen_ = 0;
    // Live TV EPG (#75 inc 3): the parsed XMLTV guide for the currently open source (the now/next on the channel
    // list and the guide grid read it), which source it belongs to, and a generation counter dropping a
    // superseded async EPG fetch. The guide is fetched (daily-cached on disk) after the channel list loads.
    xmltv::Guide      liveTvGuide_;
    QString           liveTvGuideSourceId_;
    int               liveTvEpgFetchGen_ = 0;
    // OPDS (#146): the catalog whose feeds are currently being browsed. It holds the device-local auth context
    // across a drill-in — a sub-feed row and a book item carry only a url, so the catalog id (and the creds it
    // resolves to at fetch time) is remembered here, mirroring liveTvCacheSourceId_. Reset from the level on
    // Back. A generation counter drops a superseded async feed fetch.
    QString           currentOpdsCatalogId_;
    int               opdsFetchGen_ = 0;
    // A console's Homebrew folder: the rows gathered so far across every configured server and every page
    // fetched, plus the continuations still outstanding. Accumulated rather than appended to the grid because
    // each page arrives as a whole new render — the trailing "More…" row has to be replaced, not grown past.
    // A generation counter drops a superseded fetch (a Back, or a second console opened mid-flight).
    QVector<MediaItem> homebrewRows_;
    QVector<HomebrewMore> homebrewMore_;
    int               homebrewFetchGen_ = 0;
    // #193: the same supersede-an-in-flight-fetch counter for the music-server levels, and the one-shot
    // flag that keeps a level from being rebuilt once per cover that lands.
    int               musicFetchGen_ = 0;
    bool              musicArtRefreshPending_ = false;
    // #194: the merged view over every supplier, and the two "we have already asked" sets that keep a
    // FAILED fetch from re-arming itself. artistsLoaded() stays false when a server refuses, so a gate on it
    // alone would re-request on every repopulate — and every repopulate is triggered by the last reply.
    MusicMerge::Merged mergedMusic_;
    bool               mergedMusicValid_ = false;
    QSet<QString>      musicMergeFetched_;        // server ids whose artist list we have asked for
    QSet<QString>      musicMergeArtistFetched_;  // qualified artist keys whose albums we have asked for
    // #194 increment 2: the supersede counter for the track-list fetches a "Play all"/"Shuffle all" press
    // fires. Its OWN counter, not musicFetchGen_ — see playMusicArtistQueue for why the two must not share.
    int                musicQueueFetchGen_ = 0;
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
    MediaDetail lastMeta_;           // the last VALID detail card shown, so a Download from the info page
    QString lastMetaKey_;            // can save it for offline use (MetaCache); keyed like the cache

    QVector<Level> stack_;       // navigation breadcrumb (top = current view)
    QVector<MediaItem> items_;   // items in the current view (parallel to grid_ rows)
    // The manga chapters of the level last populated, in the order the provider listed them. Kept beside
    // items_ rather than derived from it at read time because drilling into a chapter's DETAIL page clears
    // items_, and that is one of the two places a chapter is opened from.
    QVector<ChapterRun::Entry> chapterList_;
    // The media type of those entries ("manga_chapter"), carried onto the run so a crossing can ask the
    // addon for the NEXT chapter's pages without knowing what kind of serial it is (#188).
    QString chapterEntryType_;
    QString chapterSeriesTitle_;   // the container chapterList_ was drilled from — the three facts about
    QString chapterSeriesThumb_;   // it that a run carries (see ChapterRun::seriesTitle): what the Catalog
    QString chapterSeriesAddonId_; // lane searches by, and what a chapter's Recents row is titled/drawn from
    QVector<int> browseRowMap_;  // themed-browse index -> items_ row (skips synthetic _open/info rows)
    // The Trakt calendar as last read from TraktClient's on-disk cache. Loaded in the ctor so an OFFLINE
    // launch already has last week's calendar to draw, and replaced by onTraktCalendarChanged() when a
    // fetch lands. EMPTY whenever Trakt is not configured/connected — that emptiness is what makes the
    // shelf and the folder vanish, on top of the calendarAvailable() gate itself.
    QVector<CalendarEntry> traktCal_;
    // The watchlist and collection, from the same on-disk caches and for the same offline-launch reason.
    // Both EMPTY whenever Trakt is not configured/connected, which is what makes their folders vanish.
    QVector<TraktListEntry> traktWatchlist_;
    QVector<TraktListEntry> traktCollection_;
    bool recentView_ = false;    // true = showing the local "Recent" list (not an addon catalog)
    bool searchEditing_ = false; // search box: false = highlighted (arrows navigate), true = typing
    QVector<int> thumbQueue_;    // item rows awaiting a remote poster load (throttled)
    int thumbActive_ = 0;        // remote poster loads currently in flight
    int perfThumbCount_ = 0;     // thumbs.page span: remote posters queued this generation (span detail only)
    bool perfSearchFirstSeen_ = false; // search.first span: end it once, on the first streamed batch
    int generation_ = 0;         // bumped on each fresh load so stale async thumbnails are ignored
    int currentPage_ = 1;        // last page loaded for the current view
    int pendingRestoreRow_ = -1; // on Back: keep paging until this row is loaded, then scroll to it
    bool hasMore_ = false;       // the addon says another page exists
    bool loading_ = false;       // a page fetch is in progress (guards re-entrant scroll triggers)
    int pendingReqId_ = -1;      // in-flight async request; results with a different id are stale

    // ---- cross-addon search (searchEverything): the request fan-out + merge lives in SearchAggregator; HomeView
    // keeps only the UI residue (the "_search" level, grid population, loading_ mirroring, the toasts).
    void startSearch(const QString& query); // reset grid state, hand the fan-out to agg_, mirror its in-flight state
    SearchAggregator* agg_ = nullptr;       // owns the cross-addon fan-out; streams results back via its signals
    int pendingPage_ = 1;        // page number of the in-flight request
    bool pendingAppend_ = false; // whether the in-flight request appends or replaces
};
