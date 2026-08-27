#pragma once
#include <QMainWindow>

#include <QStringList>
#include <QVector>
#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <QColor>
#include <QPointer>
#include <QPointF>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include <vector>
#include "../addons/AddonModels.h"
#include "../core/EmulationScope.h"   // emuscope::Scope — scope-aware editCoreOptions (Task 3)
#include "../core/LifecyclePolicy.h"
#include "../core/MediaSegments.h"
#include "../media/LrcLyrics.h"   // trackLyrics_ is a value member (issue #142)
#include "../media/LyricSources.h" // LyricSources::Choice is a by-value parameter (issue #142)
#include "../media/BackgroundAudio.h" // BackgroundAudio::Session is returned by value (issue #193 inc 3)
#include "../core/SegmentStore.h"
#include "../core/ShuffleBag.h"
#include "../core/ThemeRegistry.h"   // installThemeRegistryEntry names ThemeRegistry::Entry (QtCore-only)
#include "../core/RomhackClient.h"   // PendingRomhack holds a chosen hack + its stated target by value
#include "../core/MusicQueue.h"      // MusicQueue::Entry — startMusicEntries takes the built queue by value
#include "../core/Scrobble.h"        // Scrobble::Track is a value member (issue #192)
#include "../browse/LeafRoute.h"     // browse::QueueTarget — the browse row the #193 reach verbs act on

class MpvWidget;
class QQuickItem;           // the themed (QML) scene root — only ever held as a pointer here
class RetroView;
class RetroParkView;   // Slice 2a: the RetroPark backend's play surface, a sibling content page beside retro_
class EbookView;
class ReaderChromeHost;
class ThemedPanelHost;
class ThemePickerHost;
class PdfView;
class ComicView;
class LibraryView;
class BackgroundMusic;
class HomeView;
class AddonManager;
class CatalogPrefetcher;
class CloudSync;
class SaveSync;             // per-file save/state sync — see core/SaveSync.h
class LocalResolveCache;
class CatalogResolver;
class SubtitleCache;
class BingeStore;           // remembered release (bingeGroup) per series — see core/BingeStore.h
struct SubtitleCandidate;   // one OpenSubtitles search row (see core/SubtitleFetcher.h) — the picker's choices
// One candidate stream for the "Choose source…" picker (see addons/StremioTranslate.h). Declared, not
// included: only references to QVector<StreamCandidate> appear here, so the definition is not needed.
namespace StremioTranslate { struct StreamCandidate; struct SubtitleAddonResult; }
class QStackedWidget;
class QSlider;
class QLabel;
class QListWidget;
class QFrame;
class QPushButton;
class QTimer;
class QScrollArea;
class QVBoxLayout;
class QNetworkAccessManager;
class QLabel;
class EmulatorManager;
class GameLauncher;
class QJsonObject;
struct GameSystem;
struct ExternalEmulator;

// Minimal media-hub window: a stacked surface holding the libmpv video view and the libretro game view,
// with Open Video / Open Game and a transport bar. The shell the rest of the hub grows from.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // chooseProfileAtStart: show the "Who's using…" picker inline after the window opens (0 or >1 profiles).
    explicit MainWindow(bool chooseProfileAtStart = false, QWidget* parent = nullptr);
    ~MainWindow() override; // out-of-line so unique_ptr<AddonManager> is destroyed where it's complete

    // What panelReturnTo_ WAS, classified at the moment the settings exit gate closed — see leaveSettingsArea,
    // which is where this is produced and consumed. It lives up here only because moc will not parse a type
    // declaration inside the `private slots:` block below.
    enum class SettingsReturn { Home, Page, None };

private slots:
    void openFile();
    void openAudio();
    void openGame();
    void openDocument(); // ebooks (.epub) + PDFs (.pdf), dispatched by extension
    void openHome();
    void enterSplitScreen();   // open the two-pane split screen (both panes empty)
    void exitSplitScreen();    // leave split mode, stop both panes
    void finishSplitOpen();    // after an item loads into splitTarget_, return to the split view
    void onRequestOpenFile(const QString& kind); // from Home's "open a file" item
    void openRecent(const QString& path, const QString& kind, const QString& resumeKey = QString(),
                    const QString& title = QString(), const QString& thumb = QString()); // re-open a Home "Recent" entry
    void onSwitchProfile();                      // pick/create a profile from the Home profile button
    void onThemeChanged(const QColor& background, const QColor& accent); // match the home view's theme
    void openLibrary();
    void openLibraryItem(const MediaItem& item); // route an addon catalog item to the right view
    // Local video library: read the configured root on the MAIN thread, then scan off-thread and install
    // the rebuilt index + refresh the home on completion. Single async-scan site (startup + settings picker).
    void rescanLocalLibrary();
    // Local MUSIC library (issue #74): the same shape one layer down — read the root and the index-file path
    // on the MAIN thread, then load / walk / re-tag / persist / group entirely in the worker. A tag scan opens
    // files, which is exactly the disk work this app has a documented history of stalling the GUI on.
    void rescanMusicLibrary();
    // Local AUDIOBOOK library (issue #139): the same shape again, over its own root and its own persisted
    // index. A separate scan rather than a mode of the music one, because the two roots are two different
    // statements by the user and a shared walk would have to be told which of them it was doing.
    void rescanAudiobookLibrary();
    // Local READING library (issue #134): the same shape a third time, over its own root and its own
    // persisted index. A separate scan rather than a mode of either audio one, because three roots are
    // three different statements by the user and a shared walk would have to be told which it was doing.
    void rescanBookLibrary();
    // Trakt calendar (#23): refresh the cached "my shows" calendar and tell the home to redraw. DEBOUNCED —
    // stamped before the request, not in the callback, so it rate-limits even though fetchMyShowsCalendar's
    // callback may never arrive (see TraktClient.h). Called from startup, from a fresh account link, and on
    // the PERIODIC tick below; never per navigation — the surfaces read TraktClient's on-disk cache, which
    // does not need a live fetch to be drawn. No-op when Trakt is not configured/connected.
    void refreshTraktCalendar();
    // The watchlist/collection top-up (#23). Deliberately NOT folded into refreshTraktCalendar: the two
    // have different costs (the lists page, the calendar is one request) and different reasons to run —
    // a calendar goes stale by the day, a watchlist only when the user edits it somewhere else.
    void refreshTraktLists();
    // Import the Trakt watched history into ItemMarks. Additive and incremental; see TraktSync.h.
    void runTraktBackfill();
    // The same import with THIS profile's watermark cleared first, behind a confirmation — the only
    // thing that reaches watches Trakt gained after a complete run with an older date, and the only
    // thing that can re-mark something the user has unmarked. Both settings builders offer it.
    void reimportTraktHistory();
    // The Trakt "what have I got and how old is it" line, shared by both settings builders so they
    // cannot drift; and the hook that re-reads it into whichever one is on screen (a no-op when
    // neither is). The line is the only place the import's watermark is visible to the user at all.
    static QString traktStatusLine();
    // The music-scrobbling confidence line (#192), built by Scrobbler and shown by BOTH settings builders so
    // the two can never tell the user different things about the same state. Not static — unlike the Trakt
    // line it reads a live object, because the queue depth and the counter are its whole content.
    QString scrobbleStatusLine() const;
    // #193: "No music servers yet." / "2 music servers: Navidrome, Basement. They appear under Music."
    // One builder, shown by both settings surfaces.
    QString musicServerStatusLine() const;
    void refreshTraktSettingsStatus();
    // Start/stop the periodic top-up to match the link state. Separate from the fetch so the two reasons the
    // timer exists (a box left running for days; an account linked mid-session) share one definition.
    void updateTraktCalendarTimer();
    // Documents (CBZ/EPUB/PDF) open through file-based readers, so a remote http(s) url must be
    // fetched to a local cache file first; this downloads then re-enters openLibraryItem locally.
    void fetchRemoteDocumentThenOpen(const MediaItem& item, const QString& ext);
    // Download (for keeps) a resolved item to <app>/downloads and record it. Fed by HomeView's downloadItem
    // signal (single item or a crawl); handed to the persistent DownloadManager which runs + tracks them.
    void enqueueDownload(const MediaItem& item);
    void openDownloadManager();          // Settings ▸ Downloads: the download-manager panel
    void updateDownloadRow(const QString& id); // refresh one job's progress bar/label in place
    // Themed Downloads: a job's Progress row is activated to open a NavMenu action chooser (Pause/Resume/Retry/
    // Cancel/Remove per the SAME state logic classic uses for its per-job buttons), mirrored on the panel graph.
    // Themed-only (its body uses the QML panel host); guarded so moc emits no metacall for it in a no-QML build.
#ifdef EB_HAVE_QML
    void showDownloadActionMenu(const QString& id);
#endif
    // Window-level notification overlay for download/resolve progress + errors. A child-widget overlay owned by
    // Notifier, floating over the central area and raised above the current page so it shows over ANY view
    // (the QQuickWidget themed home and the libmpv QOpenGLWidget both composite with sibling widgets). Driven by
    // HomeView's toastRequested/toastHideRequested and by the library download queue, so the "info while pulling
    // a file" feedback shows regardless of the active theme.
    void notify(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide); delegates to notifier_
    void hideNotice();                               // delegates to notifier_
    // A manga chapter resolves to a list of page image URLs; download them, pack into a cached CBZ,
    // then hand it to the comic reader (which gives natural page order + resume for free).
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls);
    void openSettingsHub();   // centralized "Settings" area (emulator + input)
    // The hub's rendering, WITHOUT the parental gate. Split out of openSettingsHub so the "Keep editing"
    // branch of the exit gate can put the popped hub root back without re-prompting for the PIN
    // (parentalUnlock does not cache — it asks every call).
    void presentSettingsHub();

    // ---- Settings save/discard transaction (issue #26) -------------------------------------------------
    // Entering the settings area. begin()s the transaction; begin() is a no-op while one is active, so every
    // entry point calls this unconditionally and hub -> panel -> picker all share ONE transaction. Also
    // CLOSES a transaction left open by a previous visit that escaped without passing the gate (see the
    // definition) — cheap, no dirtyCount scan.
    void enterSettingsArea();
    // True when the current stack page is part of the settings area. Used only to tell "entering from
    // outside" from "navigating within"; never a dirtiness query.
    bool inSettingsArea() const;
    // The ONE settings-area exit gate (issue #26). If the transaction is dirty, ask Save / Discard / Keep
    // editing and act on it; otherwise run `proceed` straight through. Both hub builders route their root
    // onBack through this, so all thirteen screens are covered in both modes at once.
    //
    // Returns TRUE when the settings area was actually left (`proceed` ran), FALSE for "Keep editing" — the
    // transaction is then still open and the caller must restore whatever its Back already tore down. The
    // brief specified void; the bool is needed because ThemedPanelHost pops the panel level BEFORE invoking
    // onBack, so the themed hub has to re-present itself when the user stays.
    //
    // COST: this is the ONLY caller of SettingsTxn::dirtyCount()/isDirty() in the UI. That is deliberate —
    // dirtyCount() is O(all keys in the ini), so it may only ever run on a discrete navigation event.
    //
    // What panelReturnTo_ WAS, classified at the moment the gate closed — handed to `proceed` so nothing
    // downstream has to re-read the pointer. Discard runs SettingsTxn::rollback(), whose hook calls
    // showHomeScreen() -> showThemedHome(), which reassigns themedHome_ and deleteLater()s the previous widget.
    // A `proceed` that then compared panelReturnTo_ against themedHome_ would be testing the STALE pointer
    // against the NEW home: the equality fails, so it falls into the "some other page" branch and calls
    // setCurrentWidget() on a widget already removed from the stack. Classify FIRST, act on the classification.
    // (SettingsReturn itself is declared at the top of the class — moc rejects a type inside `private slots:`.)
    bool leaveSettingsArea(std::function<void(SettingsReturn)> proceed);
    // The destination the classification names: the home screen (rebuilt, so an Appearance change applies), or
    // the remembered page — which is re-checked for liveness, so a return page destroyed after the classification
    // was taken degrades to the home screen instead of a dangling setCurrentWidget().
    void returnFromSettings(SettingsReturn where);

    QVariantMap settingsPanelStyle() const; // the active theme's `settingsPanel` block (themed panels; B2)
    void openGeneralSettings(); // general playback options (subtitle defaults)
    void openStats();           // per-profile consumption stats (Watched/Listened/Read/Played + top titles)
    void openCloudSync();     // Google Drive sign-in + sync panel
    void openCloudClientSetup(); // inline form to paste the Google OAuth client id/secret
    void cloudSyncNow();      // pull (if newer) then push the current state
    // Cloud Sync backend (Increment C): switch between Google Drive and a self-hosted server as a MIGRATION —
    // rebuild cloud_ so the ctor picks the newly-selected backend, re-arm its listeners, and (if the new backend
    // is configured) force a full push so it adopts the local state as its baseline.
    void switchSyncBackend(const QString& newBackend); // "drive" | "server"; writes cloud/backend + rebuilds cloud_
    void wireCloudSignals();  // the window-scoped CloudSync sign-in listeners (#34), re-armed after a rebuild
    void openDebug();         // diagnostic log viewer (refresh / clear / open file location)
    void confirmUninstall();  // Settings ▸ Uninstall: warn, then performUninstall() on confirm
    void performUninstall();  // remove the app folder + cache/registry/dumps via a detached post-exit script
    void openRetroAchievements(); // RetroAchievements sign-in panel
    void openBiosCheck();         // per-system BIOS presence check + download-missing (RetroBat-style)
    void openEmulatorSettings();
    void openInputMapping();
    void onTrackEnded();
    void onDuration(double seconds);
    void onPosition(double seconds);
    void onSeekReleased();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override; // reveal media controls on mouse move
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;             // keep the notification overlay anchored while dragging
    void keyPressEvent(QKeyEvent* event) override;          // Esc leaves full screen
    bool focusNextPrevChild(bool next) override;            // clamp Tab / D-pad focus nav in settings panels
    void showEvent(QShowEvent* event) override;             // grab keyboard focus on first show
    void changeEvent(QEvent* event) override;               // re-focus the themed view when the window reactivates
    void closeEvent(QCloseEvent* event) override;           // push state to Drive on exit

    // Android OS lifecycle: on backgrounding, freeze a running core / playing video; on foregrounding, resume
    // ONLY what we froze (LifecyclePolicy). Left unguarded (a probe/test can call it directly); the connect
    // in the ctor is gated on Q_OS_ANDROID so desktop app-state churn (alt-tab) never touches playback.
    void onApplicationStateChanged(Qt::ApplicationState state);
    eb::LifecyclePolicy lifecycle_;    // sticky pause/resume decision core

private:
    class DownloadManager* dm_ = nullptr;
    // Live widgets in the open Downloads panel, keyed by job id, so progress ticks update in place without
    // rebuilding (which would steal keyboard/controller focus). Repopulated each time the panel is built.
    QHash<QString, class QProgressBar*> dlBars_;
    QHash<QString, class QLabel*> dlStatus_;
    bool dlPanelOpen_ = false;           // the Downloads panel is the current view (rebuild it on state changes)
    // Themed Emulators: the emulator whose install we kicked from Settings ▸ Emulators, so GameLauncher's install
    // status stream patches the RIGHT status row (EmulatorManager::status carries no id; only one installs at a
    // time). Cleared on completion/failure.
    QString emInstallId_;

    static QString fmt(double seconds);
    // External-player handoff decision for a VIDEO play. Called at the top of each video entry point
    // (openVideoPath / playStream / openLibraryItem's video branch). Returns true when the media was handed
    // off to an external player (the caller then only records Recent and returns); false to fall through to the
    // built-in libmpv player. An external launch that fails notifies + returns false (built-in fallback).
    // Restricted profiles never leave the app.
    //
    // The route is resolved from `explicitRoute` if given, else the consume-once member `playRouteOverride_`.
    // TWO leak-free channels feed a detail-view one-off: (1) SYNCHRONOUS local/recents leaves go through the
    // member — armed just before playThemedLeaf and cleared right after it returns, so it lives only for the
    // synchronous call and can't survive to a later play; (2) ASYNC catalog leaves ride MediaItem::playRouteHint
    // through the resolve chain and arrive as `explicitRoute` — a failed/abandoned resolve never emits the item,
    // so nothing leaks. openHome()/goBack() also clear the member defensively.
    enum class PlayRoute { Default, ForceBuiltin, ForceExternal };
    static PlayRoute routeFromHint(int hint) {           // MediaItem::playRouteHint (0/1/2) -> PlayRoute
        return hint == 2 ? PlayRoute::ForceExternal : hint == 1 ? PlayRoute::ForceBuiltin : PlayRoute::Default; }
    static int hintFromRoute(PlayRoute r) {               // PlayRoute -> hint for playThemedLeaf/MediaItem
        return r == PlayRoute::ForceExternal ? 2 : r == PlayRoute::ForceBuiltin ? 1 : 0; }
    PlayRoute playRouteOverride_ = PlayRoute::Default;
    // True when playback left the app for an external player. `dryRun` runs the identical decision (one-shot
    // override included) but launches nothing and only REPORTS that it would have — used for a stream whose
    // proxyHeaders cannot follow it out, so the caller can keep it in-app and explain.
    bool routePlay(const QString& urlOrPath, PlayRoute explicitRoute = PlayRoute::Default, bool dryRun = false);

    // Path-based open helpers: open the file AND record it in the Recent list (the dialog-based
    // openFile/openAudio/openGame/openDocument and the Recent tab both route through these).
    void openVideoPath(const QString& path);
    void openAudioPath(const QString& path);    // queue the whole folder, starting at this file
    // Play a local music-library ALBUM (#74) through the SAME PlaybackSession queue: the album's tracks in
    // the index's disc-then-track order, starting at `startPath` (empty = the first track). Built from the
    // INDEX, not from the folder, because a multi-disc album lives in more than one folder.
    void openMusicAlbum(const QString& albumKey, const QString& startPath);
    // Play a MULTI-ALBUM music queue through that same PlaybackSession: one artist's whole discography
    // (`artistKey` set) or the whole library (`artistKey` empty), in the index's order or shuffled across the
    // entire set. The ONLY producer of a cross-record queue this app has apart from the multi-select file
    // dialog — which the themed surface does not expose — and therefore the reason crossfade (#141) and
    // ReplayGain's track mode have anything to act on. See src/core/MusicQueue.h for the ordering rules.
    void openMusicQueue(const QString& artistKey, bool shuffle);
    // Play a local AUDIOBOOK (#139) through that same PlaybackSession queue: the book's parts in the index's
    // order, starting at `startPath` (empty = the first part). Built from the INDEX rather than from the
    // folder, for the reason openMusicAlbum is: a folder queue would include anything else that happens to
    // be in the directory and would order it by filename alone. A multi-file book is therefore an ordinary
    // queue, which is what makes it play continuously and resume across a file boundary with nothing in the
    // player having to know what a book is.
    void openAudiobook(const QString& bookKey, const QString& startPath);
    // The tail both cross-record queue producers share: turn MusicQueue entries into the paths/titles/album
    // map a PlaybackSession queue is, and start it. Extracted when the reach verbs (#193 increment 2) became
    // the second producer — "nothing was playing, so the queue becomes this" builds the identical thing, and
    // a hand-written second copy is how one of them ends up without the musicQueueAlbums_ map and shows the
    // wrong sleeve for the rest of the hour.
    void startMusicEntries(const QVector<MusicQueue::Entry>& entries, const QString& title,
                           const QString& subtitle, bool titlesNameArtist);
    // The now-playing art/subtitle for the track at `path`, when the running queue spans records. Single-album
    // queues never call it (their sleeve is right for every track); a cross-album queue that did not would
    // show the first record's cover for the whole hour. No-op when musicQueueAlbums_ is empty.
    void refreshMusicQueueArt(const QString& path);
    // The shared tail of both local audio queues above (folder and album): surfaces, themed page data,
    // gapless arm, setQueue, media kind, Recents. See the definition for why it is one function.
    //
    // `alreadyPlaying` is the channel crossfade's entry (#141) and changes exactly one line: the queue is
    // ADOPTED around a file the player is already several seconds into (PlaybackSession::adoptPlayingQueue)
    // instead of being started. Everything else a local audio queue owes — the themed page's sleeve, the
    // Recents entry, the media kind, the gapless/crossfade arm — is owed identically by an item that faded
    // in, so it comes from here rather than from a second copy that would drift a line at a time.
    void startLocalAudioQueue(const QStringList& queue, int start, const QStringList& titles,
                              const QString& themedTitle, const QString& themedSubtitle,
                              const QString& themedArt, const QString& recentPath,
                              const QString& recentTitle, const QString& recentThumb,
                              bool alreadyPlaying = false);
    // title/thumb/key let the Recent entry show the catalog item's name + cover (a remote ROM is cached under
    // a hashed file name, which would otherwise be displayed); key is the stable id for de-dup.
    void openGamePath(const QString& path, const QString& title = QString(),
                      const QString& thumb = QString(), const QString& key = QString(),
                      const QString& systemHint = QString()); // console/platform name to pick the system over the file ext
    // A PC (Windows) game isn't an emulator ROM: download it to <data>/games/pc and hand it to the OS to
    // run/install (installer/portable .exe runs; an archive opens). See openLibraryItem's PC-platform branch.
    void openPcGame(const MediaItem& item);
    // Re-open a PC game without re-downloading: launch the remembered game exe (PcGameStore), or the game
    // exe now sitting in its install folder, re-run a not-yet-run installer, or ask the user to locate the
    // exe. tryLaunchInstalledPcGame returns true when it handled the open; false => no local copy yet, so
    // openPcGame downloads it. PC-game Recent entries (kind "pcgame") re-open through relaunchPcGame.
    bool tryLaunchInstalledPcGame(const QString& id, const QString& title, const QString& thumb);
    // Run a resolved local-game exe through the monitored path (ShellExecuteEx + grace-window + PlayStats).
    // `kind` is the Recent/Downloads routing kind: "pcgame" (default) also records a PC-console Download; a GOG
    // game passes "goggame" to reuse the SAME launch mechanics while recording under its own kind (so it groups
    // on the GOG console) and NOT as a PC download (a GOG game isn't a downloaded repack).
    void launchPcExe(const QString& exe, const QString& id, const QString& title, const QString& thumb,
                     const QString& kind = QStringLiteral("pcgame"));
    // Re-open a GOG game from a Recent (kind "goggame"): re-resolve its exe from the registry by id, falling
    // back to the exact path the Recent recorded, then run it through launchPcExe with the "goggame" kind.
    void relaunchGogGame(const QString& id, const QString& title, const QString& thumb, const QString& recordedPath);
    // Re-open a CODE-LESS Battle.net game from a Recent (kind "battlenetgame", key "bnet:<DisplayName>"): the
    // same shape as relaunchGogGame — re-resolve the exe from the registry by display name, fall back to the
    // recorded path, else notify instead of failing silently. A coded game re-launches from its battlenet:// URI.
    void relaunchBattleNetGame(const QString& id, const QString& title, const QString& thumb,
                               const QString& recordedPath);
    // (The full-screen emulator / external-emulator play-time tracking lives in GameLauncher now. PC games are
    // still timed separately in launchPcExe, off their own process handle.)
    // The launched game closed within a few seconds (it didn't really open - often missing redistributables,
    // or the wrong exe). Tell the user and offer to open its folder or pick a different exe.
    void onPcGameFailedToOpen(const QString& id, const QString& title, const QString& thumb, const QString& exe);
    // Run a PC game's setup, monitor the installer process, and when it finishes locate the installed game
    // (wherever the user pointed it) and launch it. gameDir is our extracted repack folder (a common install
    // target); the installer's registered InstallLocation is also checked so custom paths are found.
    void runPcInstaller(const QString& installer, const QString& id, const QString& title,
                        const QString& thumb, const QString& gameDir);
    // Find where a PC game actually installed: the installer's registered InstallLocation, an uninstall entry
    // whose name matches the title, a common install root (e.g. C:\GOG Games\<Title>), our extracted folder,
    // or any passed-in locations. Returns the game exe (title-named preferred), or empty if not found yet.
    QString locateInstalledGameExe(const QString& title, const QString& gameDir,
                                   const QStringList& extraLocations = {});
    void onPcInstallerFinished(const QString& id, const QString& title, const QString& thumb,
                               const QString& gameDir, const QString& installer,
                               const QStringList& installLocations);
    // Delete a PC game's spent install media (installer .exe / extracted repack) after it's installed - only
    // within <data>/games/pc, never the game's own install folder.
    void cleanupPcInstallMedia(const QString& installer, const QString& gameDir, const QString& installedExe);
    void relaunchPcGame(const QString& id, const QString& title, const QString& thumb, const QString& recordedPath);
    void promptLocatePcExe(const QString& id, const QString& title, const QString& thumb, const QString& startDir);
    // Forget a PC game entirely: clear its store entry, drop it from Recent + Downloads, and delete its
    // leftover install media under games/pc. Used when the user cancels the "locate the exe" prompt, so
    // re-opening it starts a fresh download/install.
    void forgetPcGame(const QString& id, const QString& title);
    void ensureEmuPage();    // lazily build the "playing in <emulator>" wait page
    void openEmulatorManager(); // Settings > Emulators: folder + per-emulator install status
    void openStreamPrompt();                    // inline form to paste a stream/URL link
    // route an http(s) link (or .m3u/.m3u8) to libmpv. `headers` is the source's proxyHeaders and reaches
    // BOTH the playlist probe (a plain HTTP fetch of the same URL) and the player.
    void openStreamUrl(const QString& url, const QString& resumeKey = QString(),
                       const QString& title = QString(), const StreamHeaders::Headers& headers = {});
    // play a single resolved link via libmpv. Defaulting `headers` to empty is what makes every other caller
    // (a pasted link, a Recent entry, a queue entry) CLEAR the previous stream's headers rather than inherit
    // them — see MpvWidget::play.
    void playStream(const QString& url, const QString& resumeKey = QString(),
                    const QString& title = QString(), const StreamHeaders::Headers& headers = {});
    // Stream an http(s) audiobook/audio link in the now-playing audio view (playlist + transport). Resume +
    // Recent key on resumeKey (the stable item id) since a debrid URL is re-resolved fresh each open.
    // `headers` defaults empty for the same reason playStream's does: a caller with nothing to pass CLEARS
    // the previous stream's headers rather than inheriting them (#59).
    void openAudioStream(const QString& url, const QString& resumeKey, const QString& title,
                         const QString& thumbnailUrl = QString(),
                         const StreamHeaders::Headers& headers = {});
    // Play a REMOTE multi-file audiobook as ONE BOOK (#214): `item.bookParts` is the release's audio files,
    // already filtered and ordered, and this turns them into the ordered queue PlaybackSession already knows
    // how to play — the same thing openAudiobook does for a local folder of parts, and for the same reason.
    //
    // The queue holds PART TOKENS, not links (RemoteAudiobook.h says why at length): a fifteen-hour book
    // reaches part forty days after part one was signed, so each part's link is minted when the app reaches
    // it, at the playRequested choke point. `firstPartUrl` is part one's, already resolved by the search the
    // user was waiting on, so the common case costs no extra round trip.
    void openRemoteAudiobook(const MediaItem& item, const QString& firstPartUrl);
    // Mint the link for the part `token` names and play it — the choke point's answer for a part token.
    // Async, guarded by remoteBookGen_ so an answer for a part the listener has already skipped past is
    // dropped rather than played over the one they chose.
    void playRemoteBookPart(const QString& token);
    bool openDocumentPath(const QString& path); // .epub / .pdf / .cbz by extension; true if it opened
    void toggleFullScreen();
    void leaveFullScreen();   // restore windowed: status bar + cursor

    // App pause menu (Esc): a small "Resume / Exit EverythingBox" overlay, à la the in-game pause menu.
    // A NavMenu (in-window child overlay from the nav kit) — it renders over the themed QML surface without
    // spawning a separate OS window, and restores the previous selection when it closes.
    void showEscMenu();
    void hideEscMenu();
    bool escMenuVisible() const;
    // The one Back rule, shared by Escape, Backspace and the controller's Back: close the topmost overlay /
    // pause menu, else go to the previous screen for whatever page is showing, and at the home root open the
    // app pause menu. Lives in the base window so every screen behaves identically. On a themed (QML) screen it
    // simply drives that screen's NavGraph back stack (nav.back()) — the graph's levels (catalog / browse
    // drills) and its rootBack (pause menu / themed home) ARE the themed multi-level back; there is no separate
    // themed-back closure any more.
    void goBack();
    QPointer<class NavOverlay> escMenuOverlay_; // alive while the pause menu is open
    // The themed screen currently on the stack has its own NavGraph selection model + back stack; this returns
    // it (null on classic screens). Overlays opened over a themed screen mirror themselves as levels on it.
    class NavGraph* currentThemedGraph() const;
    // Reconcile the current themed screen's NavGraph level stack with the app's real navigation state (the
    // XMB catalog level + the HomeView browse-drill depth), so nav.back() pops exactly one real level at a time
    // and bottoms out (rootBack) at the screen root. Idempotent; a no-op off a themed screen or while an
    // overlay owns the stack. Driven by browseItemsChanged and the themed state-entry sites.
    void syncThemedLevels();
    std::function<void()> themedCatalogPop_; // XMB: pop the "catalog" level -> re-show the catalog list (set in showThemedXmb)

    // The controller-navigation kit (src/ui/nav): overlay routing, the panel's selection ring, and the
    // per-screen Back action. updateNavForPage() re-registers both whenever the stack page changes, so
    // every screen gets arrow navigation + a working Back without per-screen wiring.
    // UI-test/automation channel (core/UiTestServer): created when enabled (env var / --uitest / the
    // Settings ▸ Debug toggle), torn down when the toggle turns it off. updateUiTestServer() reconciles.
    class UiTestServer* uiTest_ = nullptr;
    void updateUiTestServer();
    // Remote-control HTTP server (core/RemoteServer, issue #76): off by default, created when the Settings ▸
    // General ▸ Remote control toggle is on, torn down when it is off. updateRemoteServer() reconciles it with
    // the setting the same way updateUiTestServer() does. curPlayTitle_ is the now-playing title the /state
    // hook reports (tracked off PlaybackSession::trackChanged, the one place a display title flows through).
    class RemoteServer* remoteServer_ = nullptr;
    void updateRemoteServer();
    QString curPlayTitle_;
    // Debug-gated black-frame watchdog (src/ui/BlackFrameWatchdog): under the SAME gate as uiTest_, it samples a
    // downscaled window grab once a second and self-heals the intermittent all-black app state. Created/torn down
    // alongside uiTest_ in updateUiTestServer(); zero instances in a normal run.
    class BlackFrameWatchdog* blackWatchdog_ = nullptr;
    void kickThemedRepaint();         // watchdog recovery: force the themed QML scene(s) to re-render
    void addThemedSelection(class QJsonObject& o, QWidget* page); // themed-home selection -> UI-test state

    class NavContext* navCtx_ = nullptr;
    class NavRing* panelRing_ = nullptr;   // covers panelPage_ (header Back button + the built rows)
    class NavRing* libraryRing_ = nullptr; // covers the Library view (lists + buttons + search)
    void updateNavForPage();
    // Focus a themed (QML) page: widget focus AND the scene root's active focus (see the definition —
    // half of it leaves the page deaf to every key). Every site that shows a themed page goes through this.
    void focusThemedPage(QWidget* w);
    void presentBook(); // show book_ themed (wrapped in readerHost_) or classic (direct), per themedHomeEnabled
    void presentPdf();   // show pdf_ themed (wrapped in pdfHost_) or classic, per themedHomeEnabled (Task 4)
    void presentComic(); // show comic_ themed (wrapped in comicHost_) or classic, per themedHomeEnabled (Task 4)
    void captureReaderOrigin(); // record the launch surface into readerOrigin_ (skips a reader-to-reader re-open)

    // Controller navigation of the menus (EmulationStation-style): poll the shared gamepad on menu screens and
    // synthesise the arrow / Enter / Back keys the UI already understands, with a stick deadzone (in Gamepad)
    // and hold-to-repeat. RetroView owns the pad in-game, so this stays out of its way.
    void pollMenuPad();
    // Start (browse-only): open a context menu whose v1 entry is "Emulation settings" (present iff
    // emuMenuContext().kind != None), which opens presentEmulationPanel(ctx). Empty context -> Start keeps
    // its Escape/Back meaning (never an empty menu). Nav-kit only (NavMenu::pick), no QDialog.
    void openBrowseContextMenu();
    void sendNavKey(int key);   // deliver a synthetic key to the active view (themed QML window, panel, etc.)
    QTimer* padNavTimer_ = nullptr;
    qint64  padTick_ = 0;       // accumulated ms (fixed poll interval), for the repeat clock
    bool    padPrev_[8] = { false };  // per-nav-input: was it held last tick (edge detection)
    qint64  padNext_[8] = { 0 };      // per-nav-input: tick at which a held direction may repeat again
    void revealMediaControls();
    void positionMediaControls();
    // Place + re-stack the skip chip alone. Split out of positionMediaControls() because the chip's position
    // DEPENDS on whether the transport bar is currently visible, so it has to be (re)run AFTER any show()/hide()
    // of the bar — not before it, which is what drew the chip underneath the bar. Safe to call at any time: it
    // no-ops unless the chip is visible.
    void positionSkipChip();
    void hideMediaControls();               // hide the transport chrome now (shared by the idle timer + touch tap)
    void togglePlayerChrome();              // touch tap: hide if shown / reveal (+re-arm) if hidden
    bool handlePlayerTouch(class QTouchEvent* te); // player tap-toggle + double-tap ±10 s seek (touch only)
    void onPlayerTap(const QPointF& pos);   // pending-tap resolver: single = toggle, double(<350ms) = seek
    void showNextSourceFeedback(const QString& msg);          // player overlay (playing) or status bar (reader)
    void stepPlayerFocus(int dir); // arrow-key focus across the transport buttons (dir +1/-1, or 0 = enter row)
    // Show an in-window panel page (Settings/Theme/Cloud/General are embedded here, no popup windows).
    void showPanel(const QString& title, const std::function<void(QVBoxLayout*)>& build,
                   const std::function<void()>& onBack);
    // Host an existing QDialog inline as a panel page (no separate window). The dialog keeps its own
    // Save/Cancel box; onFinished runs when it accepts/rejects, onBack when the panel's Back is used.
    void showDialogPanel(const QString& title, class QDialog* dlg,
                         const std::function<void(int result)>& onFinished,
                         const std::function<void()>& onBack);
    void promptStartupProfile();        // inline "Who's using…" picker shown once the window is up

    // Form-factor adaptivity (D1 Task 3). applyFormFactorWidgets re-derives EVERY widget-side size from the
    // FormFactor tokens (the one chokepoint: NavOverlay/Osk fonts+key sizes, player-chrome hit targets, seek
    // slider, split-pane bar) — connected to FormFactor::changed and called once at startup. maybeOfferTvMode
    // is the one-time "this looks like a TV" suggestion, fired once post-show behind its guards.
    void applyFormFactorWidgets();
    void maybeOfferTvMode();

    // ---- Themed Profiles picker (B2 Task 5): the ProfileDialog surface on the Nav Contract. mustChoose is the
    // startup variant (no Back escape — rootBack runs the quit-confirm path); !mustChoose is the Home switcher
    // (Back keeps the current profile). Both reuse ProfileStore data ops exactly. ----
    void presentOnboardingChoice();                              // first-run: Restore-from-Drive vs. new-library choice screen
    // ---- Onboarding Restore flow (T2): signInAvailable gate -> async signIn -> the shipped pull chain -> the pure
    // onboardingRoute. Every failure routes back to the choice screen or the fresh path (never a dead end). ----
    static QString onboardingChoiceTitle();                      // the ONE choice-screen title (present + the late-async gate)
    bool onboardingChoiceIsTop() const;                          // "onboarding is still the active surface" — drops late OAuth
    void onboardingToFresh();                                    // setOnboardingDone(true) + the EXISTING fresh picker path
    void beginOnboardingRestore();                               // Restore tapped: signInAvailable gate -> signIn (async)
    void onboardingRestorePull();                                // signed in: checkStatus+applyRemote+pullAndMergeProgress
    void finishOnboardingRestore(bool restoreOk, bool remoteHasProfiles); // pure-router dispatch: Picker/Fresh/ChoiceScreen
    void presentProfilePicker(bool mustChoose);                  // reset()+present() the root list (also for startup, pre-home)
    void presentProfileList(bool mustChoose, bool replace);      // (re)build the profile list rows; replace = in place
    void editProfilePanel(const QString& id, bool mustChoose);   // nested name(TextField)+icon(Choice) picker; id "" = create
    void profileRowMenu(const QString& profileId, bool mustChoose); // Switch/Edit/Delete chooser for a profile row
    void confirmDeleteProfile(const QString& profileId, bool mustChoose);
    // setCurrent + openHome (the finish for both variants). `startup` is the caller's mustChoose: it is the ONLY
    // thing that distinguishes the pre-home startup path (no escape) from the runtime profile switcher, and it is
    // forwarded to presentThemePick, whose Back means two different things in the two cases (see below).
    void chooseProfile(const QString& id, bool startup);
    void quitConfirmFromStartup();                               // mustChoose Back: confirm quit, or re-present the list

    // THE forced-pick gate (roadmap #57) — the ONE place ThemeChoice::needsPick is evaluated. Called from
    // chooseProfile AND from showEvent's single-profile startup path, where main.cpp set the profile current
    // itself and chooseProfile never runs. Returns true when the step was PRESENTED (the caller must then do
    // nothing more — the continuation owns the rest of startup); false costs nothing. QML builds only, like
    // presentThemePick itself, so the showEvent call site is #ifdef'd.
    bool maybeForceThemePick(const QString& profileId, bool startup);
    // openHome() + the one-time TV-mode offer: the continuation shared by chooseProfile and every pick path.
    void finishToHome();

    // ---- The forced first-run theme step (roadmap #57) ----
    // Presented from chooseProfile when the newly-current profile has no theme stored yet; on pick it stores the
    // choice and runs the openHome() it displaced.
    //
    // VOID, and `afterPick` runs AT MOST ONCE and never more — on every path that reaches a home screen. This
    // method owns the "the user always reaches a home screen" guarantee, so no caller can drop the continuation.
    // The four paths:
    //   * REFUSAL — ThemePickerHost::present() returns false (nothing installed; see its contract): nothing was
    //     shown and no callback will fire, so run afterPick here. showHomeScreen() already falls back to the
    //     classic home with a "No themes found" notice.
    //   * PICK — store the folder, then afterPick.
    //   * BACK, startup == true — the PRE-HOME path, where there genuinely is no escape: the quit-confirm, exactly
    //     as the startup profile picker does. afterPick does NOT run; the user quits or returns to the profile
    //     list, which re-enters chooseProfile and presents a fresh step with a fresh continuation.
    //   * BACK, startup == false — the RUNTIME profile switcher (e.g. a second profile created months later).
    //     ProfileStore::setCurrent has ALREADY run, so the quit-confirm here would tell a mid-session user they
    //     "need to choose a profile", and cancelling it would strand them in a mustChoose profile picker with the
    //     new profile already current. Instead ACCEPT the resolved default (which is what the picker highlighted)
    //     WITHOUT writing a theme, and run afterPick — they reach home and can change it in Appearance at any
    //     time. Not writing is deliberate: the stored value syncs across devices, so a Back must not persist a
    //     per-device resolution (ThemeChoice::needsPick's note). needsPick stays true and they are asked again.
    // Defined with the rest of the themed startup surfaces (QML builds only), like chooseProfile itself.
    void presentThemePick(std::function<void()> afterPick, bool startup);
    static QString themePickTitle();                             // one title source for the forced step

    // ---- Themed core picker (B2 Task 5): SettingsDialog surface on the Nav Contract. ----
    void presentEmulatorCorePicker();                            // per-system core Choice rows (nested on the hub)
    void editCoreOptions(const QString& systemId);               // per-core options page as a nested panel level
    // Scope-aware overload (Task 3): in ThisGame scope (non-empty token) each row reads/writes the per-game
    // core-option DELTA layer (issue #95, Settings::gameOptionValue); Universal reads/writes the per-core
    // baseline (Settings::optionValue) exactly as the 1-arg form. The 1-arg form delegates here at Universal.
    void editCoreOptions(const QString& systemId, emuscope::Scope scope, const QString& token);

    // Scope-aware standalone-graphics panel (Task 4): present the gfx quartet (+ MSAA) for `emulatorId` on
    // `systemId` as its own nav-kit (NavMenu) loop. ThisGame reads EmuGfxStore::get(gameKey) folded over the
    // per-system default for DISPLAY and writes the per-game layer; Universal reads/writes the per-system
    // default layer (EmuGfxStore::systemKey(systemId)). Shares its row-build + handlers with editLaunchOptions.
    void presentEmuGfxPanel(const QString& systemId, const QString& emulatorId,
                            emuscope::Scope scope, const QString& gameKey);

    // ---- Emulation-context panel (Task 2): the per-game bundle levers (emulator override / gfx / core-option
    // delta) as one unit, over the existing stores. `gameKey` is the LaunchOptions/EmuGfx per-item key; `token`
    // is the gameToken() the launch path uses and `core` the resolved libretro core (both may be empty for a
    // game whose core is unknown — the core-option layer is then skipped).
    bool gameHasPerGameConfig(const QString& gameKey, const QString& token, const QString& core) const;
    void clearPerGameBundle(const QString& gameKey, const QString& token, const QString& core);

    // ---- Emulation-context resolution (Task 5): what the Start emulation panel needs, read off the live browse
    // state. A focused, override-capable game leaf yields a Game context (its system + the per-game keys); failing
    // that, a drilled-into console folder yields a Console context (its system only); else None. `token` and `core`
    // are derived EXACTLY as the launch path derives them (RetroView's overrideToken_ / resolveEmulationTarget), so
    // a per-game edit made from this panel matches the game the launch actually keys.
    struct EmuMenuContext
    {
        emuscope::ContextKind kind = emuscope::ContextKind::None;
        const GameSystem* sys = nullptr;   // resolved system (Game or Console); null for None
        QString gameKey;                   // LaunchOptions/EmuGfx per-item key (Game only)
        QString gamePath;                  // local file path (Game only)
        QString token;                     // Settings::gameToken(PlayStats::identity(...)) (Game only; "" if unresolvable)
        QString core;                      // resolved libretro core base name for the game (Game only; may be "")
    };
    EmuMenuContext emuMenuContext() const;

    // ---- Emulation-context panel (Task 7): the drill-in emulation surface Task 6's Start menu opens for the
    // focused game (or drilled-into console). Built on the nav-kit (ThemedPanelHost) — a Scope toggle (Game only),
    // the unified engine-tagged Emulator picker, and an engine-routed settings Action. presentEmulationPanelAt
    // THREADS the active scope (and the browse surface to return to) so the re-entrant panel holds no mutable
    // UI state; presentEmulationPanel is the entry point Task 6 calls.
    void presentEmulationPanel(const EmuMenuContext& ctx);
    void presentEmulationPanelAt(const EmuMenuContext& ctx, emuscope::Scope scope, QWidget* returnTo);

    // Atomic Apply/Discard for the Start emulation panel. The panel writes LIVE (so the shared sub-screens —
    // editCoreOptions / editEmuGfxLever — keep working), but every write records a restore closure in an
    // undo log while a session is active. On the panel's Back, a non-empty log triggers a deferred Apply/
    // Discard confirm: Apply clears the log; Discard replays it in REVERSE (each closure captured the value
    // prior to its own write, so reverse replay restores the true original even after a slot is written twice).
    // Recording is GATED by emuEditActive_, which is set true only in presentEmulationPanel — so the same
    // sub-screens called from the Settings hub / editLaunchOptions record nothing. emuEditRecord is public so
    // the file-local editEmuGfxLever(MainWindow* self, …) helper can reach it as self->emuEditRecord(…).
public:
    void emuEditRecord(std::function<void()> undo) { if (emuEditActive_) emuEditUndo_.push_back(std::move(undo)); }
private:
    bool emuEditActive_ = false;
    std::vector<std::function<void()>> emuEditUndo_;

    // ---- Themed Add-ons manager (B2 Task 6.5): the LibraryView source-management surface on the Nav Contract.
    // openLibrary() presents the ROOT (Browse/Install/Add-by-URL/Reload + one Action per source); drilling a
    // source opens presentAddonDetail (Toggle Enabled / Configure / Remove + info). List refresh is imperative
    // (install/remove/reload don't emit sourcesChanged) — mutating ops re-present the root. Catalog browsing /
    // Local ROMs stay OUT of scope (the themed home covers content). ----
    void presentAddonDetail(const QString& sourceId);            // per-addon nested panel (enable/configure/remove/info)
    void presentAddonConfig(const AddonManifest& manifest);      // manifest-driven config form (nested on the detail)
    void confirmRemoveAddon(const QString& sourceId);            // nested confirm (Info + destructive Action)
    void presentAddByUrl();                                      // nested TextField + Add -> addRemoteSource (async)
    void presentAddonRegistry();                                 // the add-on registry "store" as a nested panel
    void installRegistryEntry(const QJsonObject& entry, const QString& indexUrl, const QString& rowId); // registry install
    void setAddonsStatus(const QString& msg);                    // patch the root "Add-ons" status Info row in place
    void updatePanelInfo(const QString& id, const QString& value); // patch an Info row's value in place (status lines)
    QString registryInstallRowId_;                               // the registry entry row currently installing (async remote)

    // ---- Themed theme gallery: the twin of the classic Appearance panel's RegistryBrowser(Themes), so the
    // gallery is reachable from BOTH settings builders (CONTRIBUTING.md's rule — a user-facing setting on only
    // one is unreachable on the other surface). Shaped exactly like presentAddonRegistry above; everything that
    // is easy to get subtly wrong (index key, path safety, the atomic folder write) lives in ThemeRegistry, which
    // both surfaces share so they cannot disagree. ----
    void presentThemeRegistry();   // the themed twin of RegistryBrowser(Themes) — the theme gallery
    void installThemeRegistryEntry(ThemeRegistry::Entry entry, QString indexUrl, const QString& rowId);
    // A blocking theme install is on the stack. Its nested event loops leave the panel live, so another entry's
    // row is still activatable from inside one — see installThemeRegistryEntry, which refuses and says why.
    bool themeInstallBusy_ = false;

    // ---- Themed input mapping (B2 Task 5): ControllerRemapDialog as a themed SHELL. player/scope/turbo Choices +
    // per-button Action rows; activating a binding row enters CAPTURE (keyboard grab + pad poll), the row shows
    // "Press a key/button…", Esc cancels. Bindings apply+persist immediately (themed-panel convention). ----
    void presentInputMapping();
    void buildInputMappingRows(bool replace);                    // (re)build the shell rows for the current port/scope
    void beginInputCapture(int retroId, bool keyboard);          // enter capture for one button binding
    void endInputCapture(bool cancelled);                        // leave capture (bind was written, or cancelled)
    void refreshInputButtonRows();                               // re-patch every button row's binding label (cursor kept)
    void onInputCapturePadTick();                                // poll the pad while capturing a controller input
    bool inputCaptureKeyFilter(class QKeyEvent* e);              // consume the next physical key while capturing a key
    // Capture state for the themed input panel (mirrors ControllerRemapDialog's capture machinery, driven headlessly).
    struct RemapCapture { bool active = false; bool keyboard = false; int port = 0; int retroId = -1; bool sawRelease = false; };
    RemapCapture remap_;
    class QTimer* remapPadTimer_ = nullptr;
    QString remapScope_;   // system id currently being edited ("" = global default)
    bool    remapGameScope_ = false; // #95: editing the running game's per-game remap layer (overrides remapScope_)
    int     remapPort_ = 0; // player port whose profile is being edited

    QWidget* firstPanelRow() const;     // the first focusable row in the current panel content (or null)
    QVector<QWidget*> panelNavRing() const; // Back + the panel's focusable rows, top-to-bottom (arrow/Tab nav)

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    RetroParkView* retroPark_ = nullptr;   // Slice 2a: the RetroPark-backend play surface (a stacked content page)
    EbookView* book_ = nullptr;
    ReaderChromeHost* readerHost_ = nullptr; // themed chrome wrapping book_ (themed mode); null without QML
    PdfView* pdf_ = nullptr;
    ReaderChromeHost* pdfHost_ = nullptr;    // themed chrome wrapping pdf_ (Task 4); null without QML
    ComicView* comic_ = nullptr;
    ReaderChromeHost* comicHost_ = nullptr;  // themed chrome wrapping comic_ (Task 4); null without QML
    // The surface a reader (book/pdf/comic) was launched FROM, captured at present* time. On reader exit
    // themed mode returns HERE (the themed home/browse still showing its detail/browse view — the reader is a
    // separate stack page, so that surface's currentView is untouched) instead of the classic HomeView. Null /
    // a non-themed origin falls back to the classic home_ (the original behaviour). (B2 Task 6, item 1.)
    QWidget* readerOrigin_ = nullptr;
    ThemedPanelHost* themedPanelHost_ = nullptr; // themed settings-panel surface (B2); null without QML
    // The ONE theme-chooser surface (roadmap #57): the forced first-run step AND Appearance ▸ Theme. A persistent
    // stack page like themedPanelHost_, because it must be presentable PRE-HOME (at first run home_ does not exist
    // yet). Null without QML.
    ThemePickerHost* themePickerHost_ = nullptr;
    // Async signal hookups the themed General panel installs (Trakt live status). The host persists across
    // presentations, so — unlike classic's child-label connections that auto-drop on panel teardown — we own
    // these and disconnect them on each (re)present of General.
    QVector<QMetaObject::Connection> genSettingsConns_;
    // Async signal hookups for the OTHER themed child panels (Cloud Sync sign-in state, RetroAchievements login
    // result). LIFETIME MODEL (the full statement lives at openCloudSync's connect block): armed at panel
    // present; NOT cleared by nested children (a child's Back restores the parent without re-running open*, so
    // the parent's listeners must survive the drill); replaced wholesale when any pool user re-presents; cleared
    // at the settings-area boundaries (hub entry + leave-to-home); rebuild handlers self-gate on
    // themedPanelIsTop so a late async event never presents a panel over an unrelated screen.
    QVector<QMetaObject::Connection> panelPageConns_;
    void clearPanelPageConns();
    bool themedPanelIsTop(const QString& title) const; // themed host is the CURRENT page AND `title` is its top panel
    LibraryView* library_ = nullptr;
    BackgroundMusic* bgm_ = nullptr;    // menu background music; plays on menu screens, pauses on content
    void updateBackgroundMusic();       // play/pause the BGM to match the current view

    // Attract mode (idle screensaver, issue #54). attract_ is the pure controller (idle-fire, rotation,
    // round-trip); attractOverlay_ is the full-screen Ken-Burns slideshow child widget. The idle timer polls
    // the controller; noteAttractInput() is called on EVERY input path to reset the idle clock and, when the
    // slideshow is up, dismiss it and SWALLOW that one input (returns true = caller must not dispatch it).
    class AttractController* attract_ = nullptr;
    class AttractOverlay* attractOverlay_ = nullptr;
    class QTimer* attractIdleTimer_ = nullptr;
    QElapsedTimer attractClock_;         // monotonic ms fed to the controller (poll/noteInput)
    qint64 attractLastBuildMs_ = -1;     // when the slide pool was last rebuilt (throttles the disk scan)
    void applyAttractConfig();           // push Settings (enabled + timeout) into the controller
    void updateAttractPlayback();        // push "content on screen?" so the screensaver is suppressed then
    void rebuildAttractSlides();         // (re)build the art pool from MetaCache::allArt(), skipping artless
    void attractTick();                  // idle-timer tick: poll the controller, enter attract on a fire
    void enterAttract();                 // show the overlay for the slide the controller just selected
    bool noteAttractInput();             // reset idle + (if active) dismiss & swallow — true means swallow
    void updateThemedNowPlaying();      // push the current BGM track name into the themed home (Triple theme)
    void applyThemeMusic(const QString& themeDir); // theme.json "music" -> BGM default track (out-of-box music)
    HomeView* home_ = nullptr;
    quint64   libScanGen_ = 0;             // bumped per rescan; a slow earlier scan can't install over a newer one
    quint64   musicScanGen_ = 0;           // the same guard for the music scan (issue #74)
    quint64   audiobookScanGen_ = 0;       // ...and for the audiobook scan (issue #139)
    quint64   bookScanGen_ = 0;            // ...and for the reading scan (issue #134)
    qint64    traktCalFetchedAt_ = 0;      // unix secs of the last calendar fetch ATTEMPT (the refresh debounce)
    qint64    traktListsFetchedAt_ = 0;    // ...and the same debounce for the watchlist/collection fetch
    bool      traktBackfillRunning_ = false;  // one import at a time (see runTraktBackfill)
    // Installed by whichever settings builder is showing the Trakt status line, cleared when it goes
    // away. Held as a std::function rather than a widget pointer because the two builders hold that
    // line in different things (a themed info row addressed by id; a QLabel), and the caller only ever
    // wants "show the current value again".
    std::function<void()> traktStatusUpdate_;
    // The same idiom for the SCROBBLE status line (issue #192), and for the same reason: the two settings
    // builders hold that line in different things, and the caller only ever wants "show the value again".
    // Re-armed from Scrobbler::statusChanged, so a listen delivered while the panel is up moves the number.
    std::function<void()> scrobbleStatusUpdate_;
    QTimer*   traktCalTimer_ = nullptr;    // the PERIODIC top-up (see refreshTraktCalendar); runs only while linked

    // Themed (QML) home, gated by "themedHome/enabled" (default ON as of B2 Task 6 — absent key = themed; an
    // explicit stored `false` still selects classic). showHomeScreen() routes Home to it or the classic
    // HomeView. The themed-home methods are no-ops in builds without the QML engine.
    void showHomeScreen();
    bool themedHomeEnabled() const;
    // The ONE widget-side theme resolution (roadmap #57). Every site that used to read
    // the old global theme key and hand-roll a "not installed -> first" fallback now calls this;
    // ThemeChoice owns the key and the ordering, so the twelve copies of that logic cannot drift
    // apart again.
    QString currentThemeFolder() const;
    void showThemedHome();
    void showThemedXmb();    // themed PS3-style XMB home (cross of categories + the active category's column)
    void showThemedBrowse(); // themed gamelist of the current catalog level (driven by HomeView)
    void openAppearance();
    // Modal prompt for a themed-mode search query (`scope` names what's being searched). Returns a null
    // QString if the user cancels (empty-but-non-null clears the search).
    QString promptThemedSearch(const QString& scope);
    QWidget* themedHome_ = nullptr;
    QString  themedHomeBuiltTheme_;   // the theme the current themedHome_ was built with (reuse vs. rebuild)
    bool     themedHomeShownOnce_ = false; // first show is exposed by the top-level show(); later ones may need a kick
    bool     inContent_ = false;           // a content page (game/video/reader/emu) is currently showing
    bool     fsBeforeContent_ = false;     // full-screen state as we entered content, restored on return home
    void nudgeThemedHome();           // schedule a repaint of the (plain QQuickWidget) themed home after a rebuild
    QWidget* themedBrowse_ = nullptr;
    int themedHomeIndex_ = 0; // remember the highlighted system, so returning from a catalog lands back on it
    bool themedHomeIsXmb_ = false; // the themed home is an XMB cross (its column mirrors HomeView live)
    bool warnedNoThemes_ = false;  // "no themes installed" fallback notice shown once per run
    QStringList themedXmbCatKeys_;  // XMB: category index -> bucket key ("video"/.../"settings")
    QVariantList themedXmbCatalogs_; // XMB: the current bucket's catalog list (the column when not drilled in)
    bool themedXmbInCatalog_ = false; // XMB: column shows a catalog's live items (true) vs the catalog list (false)
    bool themedXmbAutoOpened_ = false; // XMB: the bucket's single catalog was opened directly (its contents ARE the root)
    int themedXmbCatalogIndex_ = 0;    // XMB: which catalog in the list we opened, so Back re-selects it
    int themedGridCatIndex_ = 0;      // grid home/browse: which `categories` row a sidebar theme is showing
    QTimer* themedMetaTimer_ = nullptr; // debounce the live-metadata addon fetch to the settled row
    int themedMetaWant_ = -1;           // the browse index that pending fetch is for
    // Panel-coalescing for rapid stepping (see refreshThemedMeta): mid-burst steps skip the panel rebuild
    // (a large software re-raster per step) and a settle timer renders the last row's panel once the burst
    // ends. Lone taps update immediately.
    qint64 lastMetaRefreshMs_ = 0;
    QTimer* metaSettleTimer_ = nullptr;
    int metaSettleIdx_ = -1;
    void refreshThemedMeta(int browseIndex); // set selectedMeta's skeleton for a row + queue the addon enrich
    // The ONE hover-fetch debounce, shared by every surface that drives selectedMeta (the XMB column and the
    // grid browse). Created on first use; a dense grid moves the selection far faster than an XMB column, so a
    // second timer would be strictly worse than reusing this one.
    void ensureThemedMetaTimer();
    // The themed surface whose `items` mirror HomeView's browse rows — the only place a browse-index-keyed
    // metadata fetch is meaningful: the grid BROWSE view, or the XMB home while drilled into a catalog.
    // Null everywhere else (the XMB catalog list, the grid home, any non-themed page).
    QQuickItem* themedMetaSurface() const;
    // The themed surface a hovered item's "theme song" may DUCK the menu music on — deliberately NARROWER
    // than themedMetaSurface(): the XMB home drilled into a catalog, and nothing else. See the definition.
    QQuickItem* themedPreviewAudioSurface() const;
    // The themed DETAIL view (on the Nav Contract, replacing the retired classic info page): open it for the
    // current selection (browseIndex < 0 = the themed root's currentIndex), run one of its action-row verbs on
    // the item it was opened for, and (grid browse) open it for an info-page leaf on Enter.
    void openThemedDetail(int browseIndex);
    bool openThemedDetailForInfoLeaf(int browseIndex); // true if it opened detail (a movie/book/… leaf), else drill
    void runThemedDetailAction(const QString& verb);   // play/download/favorite/playlist/hide/status/tags
    int themedDetailIndex_ = -1;             // the browse index the themed detail view is currently showing
    QString themedDetailKey_;                // the marks key (MetaCache::keyFor) of that item — hide/status/tags
                                             // address ItemMarks through this, index-churn-proof
    bool themedDetailMarksDirty_ = false;    // a Hide/Show change happened in this detail -> rebuild the browse
                                             // model on pop so the row vanishes/returns (no re-fetch)
    // Both take their target BY VALUE, resolved by the caller before the deferral that reaches them: they run
    // a turn after the QML emission that asked for them (see deferPastQmlEmission), and re-reading
    // themedDetailKey_ then would read a member the detail level's onPop may already have cleared.
    void themedDetailPickStatus(QString key); // the completion-status picker (NavMenu) for one item
    void themedDetailEditTags(QString key);   // the re-presenting tags picker/loop for one item
    void editLaunchOptions(QString key, QString systemId); // the per-game core/emulator/args editor (NavMenu/Osk, issue #51)
    void showOtherVersions(QString gamePath);  // the region/revision "Other versions" picker (NavMenu, issue #50)
    // Bulk edit (issue #65): a re-presenting nav-kit CHECKLIST over the current level's leaves (seeded with the
    // item the "Select…" verb came from), then a single action applied to the whole selection — favourite /
    // hide / tag / reassign-system, each an existing single-item store op run in a loop. Reassign MOVES the
    // ROM files, so it is NavConfirm-gated on the count and moves collision-safe (BulkSelect::reassignTargetPath).
    // Both params BY VALUE / snapshotted: it defers a turn past the QML emission and re-enters modal loops.
    void runBulkSelect(int seedBrowseIndex);
    void applyBulkAction(const QVector<int>& indices); // the actions NavMenu + the store loops + the reassign move
    // The per-item metadata editor (issue #24). One nav-kit loop shared by BOTH detail surfaces — the themed
    // action row passes themedDetailKey_, the classic card's button passes the same MetaCache key through
    // HomeView::editMetadataRequested — so the two can never drift apart.
    // Both parameters BY VALUE: the editor re-enters a modal nested loop at every step and both callers pass
    // a member, so the target is bound once at the boundary rather than aliased through the whole flow.
    void editItemMetadata(QString key, MediaDetail scraped = MediaDetail{});
    void refreshAfterMetaEdit(const QString& key); // re-render both cards + drop the stale session art
    void runThemedBrowseFilter();            // "F": defers, then runs the browse Filter menu (see the .cpp)
    void runThemedBrowseFilterNow();         // ... its body, on a clean stack
    QWidget* themedBrowseFilterTarget() const; // the themed surface "F" filters, or null if this isn't one

    // Run `work` on the next event-loop turn, once the QML signal emission that reached us has unwound
    // (issue #28). The rule this exists for, and what a caller still has to do itself, are on the definition
    // in MainWindow.cpp — read it before adding a call.
    void deferPastQmlEmission(std::function<void()> work);

    // The themed AUDIO now-playing view (Task 5): in themed mode, audio opens (openAudioPath/openAudioStream/
    // audio queue) route HERE instead of the classic player page — mpv plays invisibly while this QML page is
    // the surface. Following the detail mechanism: a `nowplayingAudio` currentView on the current themed
    // surface, with a pushed "nowplaying" nav level. See MainWindow.cpp for the transport-verb bridge.
    void showThemedAudioPage();                        // switch the current themed surface to the audio page
    void leaveThemedAudioPage(QWidget* surface, const QString& returnView); // the "nowplaying" level's onPop
    QWidget* themedAudioHost() const;                  // the current themed surface (home/browse), or null
    void runThemedAudioTransport(const QString& verb);
    // Build the transport strip from what THIS media supports and push it to the page. Track skip only when
    // there is more than one track (an audiobook is one file on purpose), chapter skip only when the file
    // carries chapters — the same "> 1" rule the classic transport's chapter buttons use.
    void pushThemedAudioTransport();
    int chapterCount_ = 0;   // last count mpv reported; chapters arrive asynchronously after a load // play/pause/seek/chapter/track/speed on the live player
    void updateThemedAudioProgress();                  // push the throttled position/duration into the QML props
    void pushThemedAudioQueue();                       // push the session queue titles + current row into the QML
    // ---- Editing the queue you are listening to (issue #193) --------------------------------------------
    // The SURFACE half; PlaybackSession + QueueEdit own the arithmetic and mpv's repair. One NavMenu serves
    // both layouts — the themed now-playing page's queue panel and the classic player page's playlist — so
    // the verbs cannot come to differ between them. Reached by Start (controller), "M" (keyboard) and the
    // panel's own chip (mouse). See the long note above queueEditable() in the .cpp.
    bool queueEditable() const;      // an editable AUDIO queue is on screen (video/IPTV queues are excluded)
    int  queueMenuRow() const;       // the queue index the verbs act on, from the live surface's cursor
    void selectQueueRow(int row);    // put that cursor back on `row` after an edit moved it
    void showQueueMenu();            // the verbs: play next / move / remove / save as playlist
    void saveQueueAsPlaylist();      // the live queue -> a new per-profile "audio" PlaylistStore playlist
    void reseatQueueFeed();          // an edit crossed the gapless frontier: re-hand mpv the coming boundary
    // ---- Reaching those verbs from a row you are BROWSING (issue #193, increment 2) ----------------------
    // Increment 1 left playNext/enqueue with exactly one caller — the queue panel — so a queue could be
    // reordered and never added to. These are the reach: one implementation of the verb (queueMusic) and the
    // three ways a browse surface asks for it (the themed XMB's inline chooser, the Start/Menu context menu
    // on BOTH layouts, and a right-click on the classic grid).
    QString queueVerbLabel(bool playNext) const;   // "Add to queue" / "Play next", said once for both menus
    int  themedBrowseIndex() const;  // the themed column's highlighted row, or -1 (not a themed browse surface)
    bool browseQueueTarget(browse::QueueTarget* out) const;  // …resolved to a music row, on whichever layout
    void showBrowseQueueMenu(int itemsRow);   // the right-click route: the verbs over one classic grid row
    void queueMusic(const browse::QueueTarget& target, bool playNext);  // THE verb; see the .cpp for the states
    // A queue that was ONE track armed neither gapless nor crossfade (both are armed from queue.size() > 1 at
    // setQueue time). An add is the first thing that can give such a queue a boundary. Returns whether
    // anything was armed, which is when the caller owes mpv a re-seat.
    bool armBridgingForGrownQueue();
    // Keep the cross-record sleeve map honest across an add: the tracks that just arrived, plus a backfill of
    // the ones that were already there when the queue was single-album (an empty map, which refreshMusicQueueArt
    // reads as "every boundary in this queue is inside one record" — no longer true the moment music from a
    // second record is appended).
    void noteQueueAlbums(const QHash<QString, QString>& added);
    void loadTrackLyrics(const QString& audioPath); // resolve a track's lyrics across all three #142 sources
    // ---- Music that survives leaving its now-playing page (issue #193, increment 3) ----------------------
    // Back on the now-playing page used to run `player_->stop(); session_->clearQueue();` on BOTH layouts, so
    // the app could not play an album while you looked at anything else — and increment 2's Append arm had no
    // live path at all. BackgroundAudio.h holds the rules (what a page exit still owes, when the route back is
    // offered, which surface it reopens); these are the host half. Video and IPTV exit exactly as they did.
    BackgroundAudio::Session audioSessionState() const;  // count + the media-kind latch, in one place
    bool nowPlayingVisible() const;        // a now-playing surface is the page in front of the user
    bool musicPlayingInBackground() const; // …and the music is playing while it is NOT
    void resumeNowPlayingPage();           // "get me back to what I was listening to"
    void stopMusicPlayback();              // THE stop verb behind every affordance that offers one
    QString nowPlayingLabel() const;       // the playing track's title, for a menu row that names it
    // Increment 4: the ONE call the nine reader-open sites make instead of their own stop-and-clear. A book,
    // a PDF or a comic owns the screen and nothing else, so it is answered from the same table a page exit
    // is; a film, a game or an emulator still takes the speakers. See the definition.
    void partPlaybackForReader();
    // Increment 4: push "something is playing" to whichever surfaces exist — the classic HomeView's chip and
    // the themed root's declared `backgroundTrack`. One call, one predicate, so the standing sign and the
    // Start/Menu route back cannot disagree about whether there is anything to go back to.
    void syncNowPlayingIndicator();
    // Which page this listening session was left FROM, remembered when it OPENED rather than derived at
    // resume time: a theme with no `nowplayingAudio` view falls back to the classic player page for the whole
    // session, and re-deriving from the current layout would send it back to a page its theme cannot draw.
    bool audioPageWasThemed_ = false;
    void pushTrackLyrics(const LyricSources::Choice& choice); // ...and push the winner to the QML page (#142)
    bool themedAudioSession_ = false;   // the current queue is a themed-mode AUDIO session (route to the page)
    bool themedAudioPaused_ = false;    // our tracked play/pause state for the transport button (reset on a new file)
    QVariantMap themedAudioData_ = {};  // the now-playing item's `selected`-shaped data (art/title/subtitle)
    QStringList themedAudioQueue_ = {}; // the session queue titles (mirrored into the page's queue list)
    // Track path -> MusicLibrary album key, for the running MULTI-ALBUM queue only (openMusicQueue fills it,
    // startLocalAudioQueue clears it). Keyed by PATH rather than by queue index so it cannot mis-index if the
    // queue moves under it: the worst a stale entry can do is give a file that really is in the library its
    // own correct sleeve. Empty for every single-album/folder queue, which is what makes this cost nothing
    // for them.
    QHash<QString, QString> musicQueueAlbums_;
    // #193 increment 5: playback path -> the INDEX path that identifies the track. Non-empty only for a
    // queue holding Subsonic tracks, where what the player was handed (a signed stream url) is deliberately
    // not what identifies the track (a qualified id) — see SubsonicClient.h. Everything that has to get back
    // from one to the other reads this one table.
    QHash<QString, QString> musicQueueIndexPaths_;
    // #204: install that table on the host AND on the session, and migrate anything already banked under the
    // stream urls it names. The one entry point — see the definition for why the three are inseparable.
    void adoptMusicQueueIdentities(QHash<QString, QString> indexPaths);
    int themedAudioCurrent_ = 0;        // the playing row in the queue
    int themedAudioPushSec_ = -1;       // last whole-second position pushed to the page (progress-bar throttle)
    LrcLyrics::Lyrics trackLyrics_ = {}; // the WINNING source's lyrics for the current track (empty = none of the three had any); pushed to host.lyrics (#142)
    QString trackLyricsPath_ = {};       // the track path trackLyrics_ was resolved for: the resolve-once-per-track key, and what a late LRCLIB reply is checked against for staleness
    int trackLyricLine_ = -2;            // last lyric line index pushed (−2 = "unset", so the first real push always fires)
    double trackLyricOffset_ = 0.0;      // this track's remembered ±0.5 s nudge, in seconds (LyricOffsetStore; #142)

    // ---- lyric PRESENTATION, shared by both layouts (issue #142) ------------------------------------------
    // Everything above resolves and holds a track's lyrics; these three act on them, and none of them is
    // themed-only. The themed page draws them through the `lyrics` element, the classic player page through
    // lyricsPanel_ below, and both go through these — a seek offered on one surface and not the other is
    // exactly the split this issue was reopened for.
    void seekToLyricLine(int line);   // selecting line i: seek to its timestamp (LyricSeek::seekTarget)
    void nudgeLyricOffset(int steps); // ±0.5 s per step, remembered for this track, re-highlight immediately
    void refreshLyricLine();          // recompute the current line from the live position and push it out
    // The offset's identity key for the track currently loaded (LyricFetch::cacheKey of its path), or empty
    // when nothing seekable is loaded. One definition, so the read on load and the write on a nudge cannot
    // key on different things and remember a nudge under an item the next play never asks about.
    QString lyricOffsetKey() const;

    // ---- the CLASSIC player page's lyric panel (issue #142) -----------------------------------------------
    // A third pane in the player page's splitter, beside the playlist: plain, toggleable from the transport's
    // gear menu, and hidden by default. It exists because lyrics were themed-only — anyone on the classic
    // surface had no lyrics at all, on any track, however many sources resolved them.
    QListWidget* lyricsPanel_ = nullptr;
    bool lyricsPanelOn_ = false;      // the user's toggle (persisted in playback/lyricsPanel)
    int classicLyricSec_ = -1;        // last whole-second position the classic panel was refreshed at
    void updateClassicLyrics();       // re-fill / re-highlight the classic panel from trackLyrics_
    void toggleClassicLyrics();       // the gear-menu row: flip the panel and remember the choice
    void openLyricOffsetMenu();       // the gear-menu row: nudge this track's lyrics earlier / later
    class QFileSystemWatcher* themeWatcher_ = nullptr; // hot-reload: rebuild the themed home on theme.json edits

    class SplitView* splitView_ = nullptr;   // two-pane split screen (its own engines per pane)
    class MediaPane* splitTarget_ = nullptr; // the pane the next opened item loads into (split "Open here")
    // Context the split branch's async core + BIOS fetches are parented to: child of the target pane (a closed
    // pane cancels the pending load), QPointer so deleting a stale one is safe, recreated per split game open.
    QPointer<QObject> splitLaunchCtx_;
    bool splitMode_ = false;                 // currently showing the split screen
    class Achievements* ach_ = nullptr;      // RetroAchievements client (full-screen emulator)
    std::unique_ptr<AddonManager> addons_;
    // Local Library ID-resolver: the on-disk match cache + the background resolver that fills it. Constructed
    // after addons_ (its search source) and before the first rescan; the resolver's resolved() rebuilds the index.
    std::unique_ptr<LocalResolveCache> resolveCache_;
    std::unique_ptr<CatalogResolver> resolver_;
    CatalogPrefetcher* prefetcher_ = nullptr; // background catalog warmer (QObject child of this); kicked post-paint
    std::unique_ptr<CloudSync> cloud_;
    // Per-file sync of emulator saves and save states (save-sync T5). Declared AFTER cloud_ on purpose: it
    // holds a raw CloudSync* and members are destroyed in reverse declaration order, so this one goes first.
    // The rules it obeys are SaveSyncPlan's; this window only decides WHEN it runs.
    std::unique_ptr<SaveSync> saveSync_;
    // One full save/state reconcile, guarded on "signed in" and safe to call more than once. Must only be
    // called AFTER the state bundle has been applied — a legacy bundle can still carry saves/ entries, and
    // applying one over a just-resolved file would put a stale save back. Both pull chains call it at their
    // own completion: the steady-state one (main.cpp's cloudPullAtStartup, which finishes before this window
    // exists) via the deferred startup kick, and the first-run restore via finishOnboardingRestore.
    void startSaveSync();
    // "Continue watching" cloud sync: a small resume+recent JSON file, pulled+merged on startup and pushed
    // (debounced) when a position changes — separate from the heavy state bundle so it stays timely across devices.
    QTimer* progressSyncTimer_ = nullptr;
    void scheduleProgressSync();          // (re)arm the debounced push after a resume/recent change
    void pushProgressNow();               // serialize local progress + upload the small JSON to Drive
    void pullAndMergeProgress();          // download remote progress + merge into local, then refresh the home view
    QByteArray serializeProgress() const; // current resume positions + per-profile recent lists -> JSON
    void mergeProgress(const QByteArray& json); // merge remote JSON into local by recency (never deletes local)

    // ---- push settings on Save, with a durable retry when offline (#34) ---------------------------------
    // The POLICY (when to attempt, when to wait, when to stop, what to do about a peer's push) lives in
    // core/PendingPush and is exercised headlessly by probe_cloudmerge §19-23. What lives here is only the
    // plumbing: two timers, one in-flight guard, and the funnel that feeds every push's outcome back into the
    // durable record. Nothing on this path may block the UI or gate a local save on the network.
    QTimer* settingsPushTimer_ = nullptr;  // Save -> one push per settings VISIT (short debounce off the nav path)
    QTimer* pendingRetryTimer_ = nullptr;  // the backoff timer; re-armed from the record after every attempt
    QTimer* cloudPushWatchdog_ = nullptr;  // releases cloudPushBusy_ if the Drive chain never calls back at all
    bool cloudPushBusy_ = false;           // one attempt in flight at a time (an attempt is 1-3 Drive round trips)
    // Bumped when an attempt takes the guard, and again when the watchdog gives up on one. A callback carries
    // the epoch it started under, so a reply that lands after its attempt was abandoned is DROPPED instead of
    // double-counting the failure (or clearing a record the next attempt now owns).
    quint32 cloudPushEpoch_ = 0;
    // Carried across a deferral: PendingPush::due() may answer Deferred (a settings visit is open), and the
    // re-armed attempt must remember whether a user action stood behind it.
    bool settingsPushManual_ = false;
    // WHAT SET THIS ATTEMPT OFF. Two facts come out of it, and they do not line up the same way, which is why
    // one bool could not carry it: `manual` (may this override a backoff window and un-park a give-up?) is
    // true for AfterSave and UserAction, while the settings-visit gate (PendingPush decision 5) applies to
    // both TIMERS and not to UserAction — a push the user is asking for at this instant is reachable only from
    // inside the settings area, so a transaction is open by construction and gating it would make the panel's
    // "Retry sync" a button that visibly does nothing.
    enum class PushTrigger { Backoff, AfterSave, UserAction };
    void pushSettingsAfterSave();          // called from leaveSettingsArea's Save branch, and ONLY from there
    void armSettingsPushTimer();           // (re)arm the short debounce — the Save path AND the deferral path
    void runPendingPush(PushTrigger t);    // one attempt: due() -> checkStatus -> resolve() -> push/pull+push
    void finishPendingPush(bool ok, quint32 epoch); // an ATTEMPT ended: release the guard (if still ours), record
    void recordPushOutcome(bool ok);       // THE funnel — every push in the app reports its result here
    void armPendingRetry();                // (re)arm pendingRetryTimer_ from the durable record
    // The Cloud Sync panel's honesty line: empty when nothing is owed, else why and what the user can do.
    // Reports STATE, never a credential or a settings value.
    QString cloudPendingLine() const;
    void refreshCloudPendingRow();         // patch that line into whichever Cloud Sync surface is built
    // The classic panel's pending line. A QPointer because the panel is rebuilt (and the label destroyed)
    // whenever showPanel runs, while a push completing minutes later still wants to patch it if it is alive.
    QPointer<QLabel> cloudPendingLabel_;
    // ...and its "Retry sync" row, held for the same reason: a park arising while the panel is OPEN has to
    // move the ACTION the line tells the user to choose, not just the line.
    QPointer<QPushButton> cloudRetryRow_;
    // Whether the themed Cloud panel was built WITH the retry row. Themed rows are omitted, not hidden, so
    // making one appear needs a rebuild — and a rebuild is only correct when the row set actually changed.
    bool cloudRetryRowShown_ = false;
    QNetworkAccessManager* docNam_ = nullptr; // lazily created: fetches remote CBZ/EPUB/PDF to a cache file

    class AppUpdater* updater_ = nullptr; // checks GitHub Releases for a newer app build + installs it in place

    // Auto-subtitle download (OpenSubtitles): when a movie/episode video loads with no subtitle in the
    // preferred language, fetch one and load it into the player. subCtx_ holds the current video's match
    // hints, set only for eligible opens and consumed once by the MpvWidget::fileLoaded handler.
    // subCache_ (app-owned, the fetcher stays pure transport) remembers the .srt already downloaded for an
    // (identifier, language) so a replay costs zero network and zero OpenSubtitles quota.
    class SubtitleFetcher* subFetcher_ = nullptr;
    std::unique_ptr<SubtitleCache> subCache_;
    // The release the user chose per series, by Stremio's own bingeGroup, so the rest of the show keeps using
    // it (episodes only — BingeStore::seriesKeyFor returns empty for a movie). Owned HERE, like subCache_:
    // AddonManager takes the preference as a parameter rather than depending on a UI-owned store. HomeView is
    // handed the raw pointer (setBingeStore) because the browse Play paths live there.
    std::unique_ptr<BingeStore> bingeStore_;
    // localPath: the video file on disk when this open is a local one — enables the exact-rip OSDb hash tier.
    // `type` is the Stremio /subtitles route type (episode/tv folded to "series") — the add-on subtitle tier
    // needs it; the OpenSubtitles tier keys on imdbStreamId/title alone and ignores it.
    struct SubContext { QString imdbStreamId; QString title; QString localPath; QString type; bool active = false; } subCtx_;
    void armSubtitleFetch(const MediaItem& item); // set subCtx_ if this video is eligible for auto-subtitles
    // The add-on subtitle tier (#79): fan /subtitles out across enabled Stremio subtitle add-ons, present the
    // rows in the NavMenu picker (tagged as the add-on source), download+cache+attach the chosen one.
    void presentAddonSubtitles(const QVector<StremioTranslate::SubtitleAddonResult>& list, const QString& lang,
                               const QString& cacheKey);
    // The zero-config auto-pick: when OpenSubtitles is unconfigured and an add-on returns a match in the
    // preferred language, apply it automatically (the OpenSubtitles auto-pick's equivalent for add-on users).
    void autoFetchAddonSubtitle(const QString& lang, const QString& cacheKey);
    // When a TV episode finishes, resolve + play the next one (same season ep+1, then next season ep1).
    void tryPlayNextEpisode();
    // The preconditions tryPlayNextEpisode() itself applies, factored out so the credits-skip branch can ask
    // "will the hand-off actually happen?" instead of merely "is autoplay on?" — the two cannot drift.
    bool canPlayNextEpisode() const;
    // Hand-off latching, same shape as the channel's (channelAiring_ + channelAirGen_): the bool stops a second
    // hand-off starting while one is in flight (credits-skip starts one, EOF would start another because the
    // credits branch doesn't seek), and the generation gates the async resolve's late callback so a stale result
    // can't re-open an episode after the user moved on. Both are cleared/bumped in resetSegmentState().
    bool nextEpPending_ = false;
    int  nextEpGen_ = 0;
    // The gate both async resolve callbacks pass through. Answers "is this result still ours?" AND, when it is
    // not, unlatches + hides the "Up next…" notice — a dropped callback is the hand-off dying, and nothing else
    // in this file would ever clear either one before the next open.
    bool nextEpHandoffStillOurs(int gen);
    void playResolvedEpisode(const QString& imdbStreamId, const QString& url, const QString& mime,
                             const StreamHeaders::Headers& headers = {});

    // Intro/credits skipping. A SEPARATE context from subCtx_ on purpose: subCtx_ is the subtitle system's
    // and is deliberately cleared on the openVideoPath route, whereas segments can still be derived there
    // from the filename alone.
    struct SegmentCtx { QString seriesKey; int season = 0; QString localPath; };
    SegmentCtx               segCtx_;
    MediaSegments::Tracker   segTracker_;
    std::unique_ptr<SegmentStore> segStore_;
    // gatherSegments() is once-per-open: mpv re-emits `duration` on every observed change, and a second run
    // would reset() the tracker and wipe its consumed_ set — re-offering a segment the user already passed
    // (duplicate seek + duplicate notice) and repeating the synchronous .edl disk read on the GUI thread.
    bool                     segGathered_ = false;
    void gatherSegments();
    // Re-run gatherSegments() after the user marks or forgets a range, so the mark applies to the rest of THIS
    // episode. The latch is dropped HERE and not inside gatherSegments(): the once-per-open guarantee exists to
    // survive mpv re-emitting `duration`, and only an explicit user action may re-arm past it.
    void regatherSegments();
    void onSegmentEntered(const MediaSegments::Segment& seg);
    // The learn tier: mark an intro/credits range against this season, or forget the season's marks.
    void showSegmentMarksMenu();
    // A marked intro START awaiting its END (-1 = none pending). Per-playback, so resetSegmentState() clears it.
    double                   segIntroStart_ = -1.0;
    // The per-open reset of every segment/hand-off latch. notePlaybackStart() calls it, but two mpv-open routes
    // (openAudio's multi-select branch, the StreamResolver::playQueue lambda) go straight to setQueue and never
    // reach notePlaybackStart — they call this directly so a previous episode's learned intro can't be armed
    // against a music track. Kept separate from notePlaybackStart() because those routes don't want its
    // channel-guard work.
    void resetSegmentState();

    // ---- Channel mode: shuffle-bag random autoplay over a video/audio playlist ------------------------------
    // A "channel" turns a playlist into a personal TV network: it airs a random item, and on each NATURAL end
    // (EOF only — the queueFinished seam is already EOF-gated) shows a cancelable countdown then airs the next
    // bag pick. State is session-only. channelPlaylistId_ non-empty == a channel is live. The bag draws each
    // item once before repeating.
    //
    // Latch shape (no-leak): `channelAiring_` is a bool true ONLY across a SYNCHRONOUS play-dispatch span — from
    // airChannelPick (or onChannelPickResolved) until the play sink consumes it — so no user input can interleave
    // and be adopted. Continuity across the ONE async gap (a remote /stream resolve) is carried by the generation
    // counter `channelAirGen_`: each airing tags its async result with the gen, and a result is dropped if its gen
    // is stale (a later pick, a manual play, or channel exit each bump the gen). So a manual play can never be
    // mistaken for the channel's pick, and a stale/superseded pick can never fire.
    QString    channelPlaylistId_;          // the playlist a live channel is airing; empty == no channel
    ShuffleBag channelBag_;                  // the random sequencer (no repeat until exhausted)
    bool       channelAiring_ = false;       // true only within a synchronous play-dispatch of the channel's pick
    int        channelAirGen_ = 0;           // bumped per airing / exit / manual play; gates async pick results
    int        channelSkips_ = 0;            // consecutive picks that couldn't play directly (cap = playlist size)
    // The next pick, drawn BEFORE the boundary instead of at it (issue #141, crossfade in channel mode). A
    // channel used to decide what follows only once the current item had finished, and that is the one thing
    // a crossfade cannot be given: the overlap has to open the incoming file SECONDS BEFORE the outgoing one
    // ends. So the draw moves earlier — and only the draw. What the channel plays is unchanged because every
    // pick leaves the bag through takeChannelPick(): a pre-drawn index is kept and aired in its turn, never
    // discarded and never drawn twice, so the bag's sequence reaches the screen in the order the bag made it.
    //
    // channelNextPath_ is that pick's local music file and is empty far more often than not — it is filled in
    // only for a pick that would air as a local music folder queue, which is what a crossfade can open with
    // no round trip and no side effect. Empty means "this boundary is aired the ordinary way", which is every
    // remote pick, every audiobook, every video, and every pick at all while the setting is off.
    int        channelNextIndex_ = -1;       // pre-drawn bag index awaiting its turn; -1 = nothing drawn
    QString    channelNextPath_;             // that pick's local music file, or empty = not one to fade into
    bool channelActive() const { return !channelPlaylistId_.isEmpty(); }
    int  takeChannelPick();                       // the ONE way a pick leaves the bag: the pre-drawn one, else a draw
    void prepareChannelNextPick();                // draw the next pick early, and note whether it can be faded into
    void promoteChannelCrossfade();               // a window handed over across a CHANNEL boundary: adopt the pick
    void startChannel(const QString& playlistId); // build the bag, air the first pick, go live
    void advanceChannel();                        // next bag pick -> countdown interstitial -> air it (or exit)
    void airChannelPick(int index);               // drive playlist item `index` through the per-entry open path
    void channelSkip();                           // a pick detoured (detail page / no stream): skip to next, or give up
    void onChannelPickResolved(int gen, const MediaItem& item); // async pick got a stream -> play it (if gen current)
    void onChannelPickDetoured(int gen);          // async pick had no stream -> skip it (if gen current)
    void exitChannel();                           // clear channel state (every user-stop / manual-play path)
    void notePlaybackStart();                     // a play sink reached: keep the channel iff this IS its pick
    // A non-game play surface is about to own the screen -> cancel a still-pending external emulator launch,
    // so it cannot boot over the film / track / book minutes later. No-op when nothing is pending, so every
    // caller is unconditional. NOT called from notePlaybackStart() — see the definition for why.
    void supersedePendingExternalLaunch();

    // Casting the current stream to a Chromecast / DLNA device on the LAN. castUrl_ etc. hold the currently
    // playing stream so the picker can hand it to the chosen device.
    class CastManager* castMgr_ = nullptr;
    QString castUrl_, castTitle_, castMime_;
    // The stream in castUrl_ needs HTTP headers a cast device cannot be given (it fetches the URL itself).
    // Set alongside castUrl_ on every path that assigns it, so it can never describe a previous stream.
    bool castHeaderGated_ = false;
    void showCastMenu(QWidget* anchor);           // device picker popup for the cast button

    // Trakt.tv scrobbling: mark movies/episodes watched as you play them. scrobbleImdb_ is the id currently
    // being scrobbled (empty when nothing is).
    class TraktClient* trakt_ = nullptr;
    QString scrobbleImdb_;
    void startScrobble(const QString& imdbStreamId); // begin scrobbling a video (stops any prior one)
    void stopScrobble();                             // stop + mark-watched the current scrobble

    // ---- MUSIC scrobbling (issue #192) ------------------------------------------------------------
    // The counterpart to the Trakt block above, and deliberately NOT an extension of it: film and TV go to
    // Trakt as a start/stop pair against a live connection, while music goes to a listening service as a
    // completed, timestamped, offline-safe listen. Everything that decides WHETHER and WHEN lives in
    // core/Scrobble.h; this window only reports three facts to it (see Scrobbler.h).
    class Scrobbler* scrobbler_ = nullptr;

    // WHICH RECORD THE RUNNING QUEUE IS FROM, for the tracks musicQueueAlbums_ does not name. That map is
    // filled AFTER startLocalAudioQueue returns (its own comment says why: setQueue's first trackChanged
    // fires inside the tail), so track 0 of every queue would otherwise have no album to look its tags up in
    // — and track 0 is the one track every single-album play has. The pending/live pair is the
    // pendingChannelGroups_ idiom: the opener sets the pending value BEFORE the tail, and the tail adopts it
    // at exactly the point it clears the multi-album map.
    QString pendingScrobbleAlbumKey_;
    QString scrobbleAlbumKey_;

    // A STREAMED track's tags, when the addon/server item carried them. Empty for everything else, which is
    // what makes an untagged stream skipped rather than scrobbled as "Unknown Artist". Same pending/live
    // shape and for the same reason.
    Scrobble::Track pendingScrobbleStream_;
    Scrobble::Track scrobbleStream_;
    // Note this item's tags for the scrobbler before opening it as a stream. Called at the audio/audiobook
    // leaves, which are the only two places an addon item and an audio open meet.
    void noteStreamScrobble(const MediaItem& item, const QString& type);
    // The tags for a track, or false when there are none to be had. `path` is what PlaybackSession holds —
    // the file path for a library track, the url for a stream.
    bool scrobbleTrackFor(const QString& path, Scrobble::Track& out) const;
    // Tell the scrobbler a track began, from the ONE signal that crosses a gapless boundary.
    void noteScrobbleTrack(const QString& path);

    // Parental gate: true if the action may proceed. When a restricted (kids) profile is active and a PIN is
    // set, prompt for it; otherwise allow. `reason` is shown in the prompt.
    bool parentalUnlock(const QString& reason);

    // ---- Per-profile passcode (issue #30) ----------------------------------------------------------
    // DELIBERATELY A DIFFERENT GATE FROM parentalUnlock ABOVE, not an extension of it. That one asks "may
    // this kid LEAVE their profile / open Settings?" and is answered by the one global PIN; this asks "may
    // this person ENTER this profile?" and is answered by that profile's own code. A user may reasonably
    // want either without the other, so they never share a value, a salt or a prompt.
    //
    // On a RESTRICTED (kids) profile the two compose rather than merge, and the combination is spelled out
    // here so it is not left emergent:
    //   * entering it       -> the profile passcode (this gate). The parental PIN also opens it, as the
    //                          documented override; the kid's own passcode does NOT open anything else.
    //   * leaving it, or opening Settings from inside it -> the parental PIN, exactly as before. Knowing the
    //                          profile passcode buys a kid nothing here — parentalUnlock never consults it.
    //   * forgetting it     -> THE PARENTAL PIN ONLY. A restricted profile never gets the timed self-service
    //                          reset, with or without a PIN set (ProfilePasscode::entryOptions owns that
    //                          rule): a sixty-second countdown is no obstacle to the one person a kids
    //                          profile exists to hold. A household that sets a kid passcode with no parental
    //                          PIN therefore has no in-app way back, and profilePasscodeMenu says exactly
    //                          that — and offers to set the PIN — at the moment the passcode is set.
    //
    // A LOCKOUT NEVER CLOSES THE PAD. The recovery rows live on the pad and nowhere else, so refusing to draw
    // it during a lockout made a child's wrong guesses block the parent's own override. The pad opens; the
    // lockout only makes a typed code be refused without comparison (ProfilePasscode::evaluate).
    //
    // Returns true when the profile may be opened: no passcode set, a live ticket (below), the correct code,
    // the parental-PIN override, or a completed timed reset. False means the caller must not proceed.
    bool profilePasscodeUnlock(const QString& profileId);
    // Record that `id` is now the active, UNLOCKED profile: setCurrent + enteredProfile_ + spend the tickets.
    // The one definition of "a profile was entered", shared by all three front doors.
    void markProfileEntered(const QString& id);
    // THE LANDING GATE. Called from openHome() — the single point where a profile's home becomes what is on
    // screen — so that landing on a profile always passes the gate however `profiles/current` got there,
    // including ProfileStore::remove()'s silent repoint onto a survivor. See the .cpp for the full case.
    // False means the caller must NOT render: the picker has been queued instead.
    bool ensureActiveProfileUnlocked();
    // The same question WITHOUT prompting — for the renderers that rebuild the home out of band (a cloud pull
    // landing) rather than navigating to it, where raising a pad from a network reply would be wrong.
    bool activeProfileEntered() const;
    // Set the GLOBAL parental PIN from outside Settings (offered when a kids passcode is being set with no
    // PIN in place). SET only — changing/clearing an existing PIN stays in Settings behind the current PIN.
    bool promptSetParentalPin(class NavGraph* graph);
    // May the `restricted` flag on this profile be changed? That flag is credential-bearing since the timed
    // reset is withheld from kids profiles: un-restricting hands the reset back, so it is a passcode-reset
    // bypass unless gated. Parental PIN when one exists (both directions); otherwise the profile's OWN
    // passcode to turn it OFF. False means the caller must put its toggle back. See the .cpp for the rule.
    bool allowRestrictedChange(const QString& profileId, bool turningOn);
    // The shared "no parental PIN ⇒ this is unrecoverable" card, raised from BOTH sides of the pair that
    // creates that state: setting a passcode on a restricted profile, and restricting one that already has a
    // passcode. Offers to set the PIN on the spot. NEVER blocks.
    void warnKidsPasscodeUnrecoverable(const QString& profileId, class NavGraph* graph);
    // The profile whose gate this session has actually passed. Not "the current profile": that is exactly the
    // distinction the landing gate turns on. Cleared on a refusal so the next landing asks again.
    QString enteredProfile_;
    // Re-entrancy guard, and it DENIES. The pad runs a nested event loop, so anything dispatched inside it
    // re-enters the gate; answering "true" there rendered the locked profile's home out from under the pad
    // still asking for its code. Set only inside ensureActiveProfileUnlocked, which only openHome() calls —
    // so "busy" implies an openHome is in flight and will land, and a denied re-entrant call is a duplicate,
    // not a lost navigation.
    bool    profileGateBusy_ = false;
    // The Set / Change / Remove chooser, opened from a profile's edit surface in BOTH the themed and the
    // classic builder. Every branch goes through profilePasscodeUnlock first, so all three require the code.
    // `onDone` runs after the chosen branch resolves (or immediately-ish on a back-out) — the chooser is a
    // NavMenu, whose callback fires AFTER the overlay closes, so a caller that refreshed its "Passcode: set"
    // label on the next line would be reading the state from before the user had chosen anything.
    void profilePasscodeMenu(const QString& profileId, const std::function<void()>& onDone = {});
    // A SHORT-LIVED grant, keyed by profile id (value = the ms-epoch of the successful unlock). It exists so
    // one unlock covers the action it was asked for plus the immediate follow-on — unlock to edit a profile,
    // then change its passcode on the next screen — instead of prompting twice in four seconds for the same
    // secret. Cleared wholesale by markProfileEntered, i.e. when a profile actually becomes current.
    //
    // PRECISELY what that buys, because an earlier version of this comment overstated it as "ENTERING a
    // profile always asks": a ticket is spent on the FIRST thing that happens after it is granted, and that
    // thing may be an entry. Unlock to Delete a profile, back out of the confirm, then pick that same profile
    // within the window and you walk in without being asked again. Defensible — the person demonstrably knew
    // the code seconds ago, and the alternative is asking twice in four seconds for the same secret — but it
    // is "one unlock covers one burst of actions", NOT "entry is always challenged".
    QHash<QString, qint64> passcodeTickets_;
    static constexpr qint64 kPasscodeTicketMs = 90000;
    // The player bar's single subtitle button opens a full-player overlay panel (Stremio-style): track pick,
    // sync/size, load-from-file, download. subOverlay_ is the scrim+panel (null when closed); the focusable
    // controls are collected in subPanelButtons_ for arrow/remote navigation, like the transport row.
    void showSubtitleMenu();
    void hideSubtitleMenu();
    // The panel's arrow/Enter/Back navigation, in ONE place. Called both from keyPressEvent (keys that
    // propagate up normally) and from eventFilter on the track list's QScrollArea — which otherwise EATS
    // every arrow key to scroll itself (QAbstractScrollArea::keyPressEvent accepts them), so focus could
    // never leave the track list: not to the sync/size column, not to the close button. Returns true when
    // the key was the panel's to handle.
    bool handleSubtitlePanelKey(int key);
    // Push the current subtitle-appearance Settings to the live player (issue #71). No-op when no player is
    // up (the style is re-read at the next player creation anyway). Both settings builders call this after a
    // Subtitles row changes so the restyle is visible on the currently-playing sub at once.
    void applySubtitleStyleLive();
    // Push the current reader-typography Settings to the live reader (issue #135). No-op when no book is open
    // (the typography is re-read at the next openBook anyway). Both settings builders call this after a Reading
    // row changes so the reflow is visible in the currently-open book at once, keeping the reader's place.
    void applyReaderTypographyLive();
    // Push the current audio-output Settings (device / passthrough / exclusive) to the live player (issue #69).
    // No-op when no player is up (re-read at the next player creation anyway). Both settings builders call this
    // after an Audio row changes; the device switch is audible at once, passthrough/exclusive on the next AO init.
    void applyAudioOutputLive();
    // Push the current refresh-rate-matching Setting (issue #70) to the live player. No-op when no player is up
    // (re-read at the next player creation anyway). Both settings builders call this after the "Reduce judder"
    // toggle changes so video-sync switches on the currently-playing video.
    void applyRefreshSyncLive();
    // Push the current HDR-output Setting (issue #68) to the live player. No-op when no player is up (re-read at
    // the next player creation anyway). Both settings builders call this after the "HDR video" choice changes so
    // the tone-mapping / passthrough options switch on the currently-playing video.
    void applyHdrOutputLive();
    // Push the current ReplayGain Settings (mode + preamp) to the live player for the item that is loaded RIGHT
    // NOW (issue #141). Called from the fileLoaded choke point at every open — the point at which both halves of
    // the answer are finally known (the session's audio/video kind, and mpv's parsed chapters, which is what
    // separates music from an audiobook) — and again from both settings builders when a ReplayGain row changes,
    // so a mode switch is audible on the track that is already playing. No-op when no player is up.
    void applyReplayGainLive();
    // The ONE music-vs-audiobook/podcast/video answer, in one place. #140's per-item speed computes it (audio
    // with no chapters is music, audio with chapters is a book) and parks it in speedIsMusic_; #141's
    // ReplayGain reads it, and #141's crossfade reads it. Written as a single accessor so a third caller
    // cannot spell the test slightly differently and give the two features different ideas of what a file is:
    // the audiobook carve-out has to mean exactly the same thing to both, or one of them is wrong about a
    // file the other is right about. Video is false through the same expression - which is also #141's
    // "never crossfade video" - because mediaIsVideo() is stamped by the app at every open site.
    bool currentItemIsMusic() const;

    // ---- Crossfade (issue #141) --------------------------------------------------------------------------
    // Decide, ONCE per track, whether the boundary out of the track now playing may be crossfaded and for how
    // long, and hand the boundary to whoever owns it: a decision of 0 hands it back to gapless (feedNextTrack),
    // anything else keeps it for the overlap. Called from BOTH the fileLoaded choke point and onDuration
    // because it needs a fact from each (the music-vs-book split, and the length) and mpv reports them in no
    // guaranteed order - whichever arrives second is the one that decides. Guarded so it runs once per file.
    void decideCrossfadeBoundary();
    // Start the overlap when the outgoing track is within the decided window of its end. Driven from
    // onPosition; the decision itself was already made and is only READ here.
    void maybeStartCrossfade(double positionSec);
    // The transport's Next/Prev, routed so a press inside a crossfade window is not a plain queue skip. Next
    // resolves the window to the incoming track (#141's wording); Prev abandons it and goes back from the
    // track that was on its way out. Both settings surfaces, the remote API and the themed transport go
    // through these rather than calling PlaybackSession directly.
    void skipToNextTrack();
    void skipToPrevTrack();
    // The manual picker's second half: show the OpenSubtitles search results as a controller-navigable
    // NavMenu (an in-window overlay — never a QDialog), and download + cache whichever row the user picks.
    // cacheKey is PINNED by the caller at request time (subCtx_ is rewritten by every media open, and the
    // download is a further round-trip), so a late reply can never cache under the wrong video's key.
    void presentSubtitleCandidates(const QVector<SubtitleCandidate>& list, const QString& lang,
                                   const QString& cacheKey);
    // "Choose source…" on a catalog item that resolves through the Stremio stream add-ons: list every
    // candidate release and let the user pick one, instead of taking whatever the auto rule ranked first.
    void chooseStreamSource(const MediaItem& item);
    // "Romhacks…" on a retro game leaf: list what the server's romhack sources have for this game, let the
    // user pick one, show what the author said it targets, and install it into the ROMs folder as a real
    // playable game. See RomhackInstall for why an installed hack is a file rather than a virtual entry.
    void showRomhacks(const MediaItem& item, const QString& systemId);
    // A hack the user has already chosen and confirmed, waiting for the base ROM it patches. Held by value:
    // the flow that chose it has returned by the time this is used, and a download can outlive the page the
    // game was picked from.
    struct PendingRomhack
    {
        MediaItem base;            // the game being patched — its title and artwork name the installed hack
        QString systemId;
        RomhackEntry hack;
        // WHERE the patch is, on our own disk — not the bytes. Acquired once the user has committed, because
        // applyRomhack can run an hour later behind a base-ROM download and the server keeps a fetched file
        // only for a while; a path rather than a buffer because at disc scale a buffer is not a field, it is
        // a liability. Both routes into this struct are copied BY VALUE into a lambda that outlives the frame
        // that built it, so a QByteArray here meant the patch was held twice for the length of a download.
        // The chosen RomhackPatchFile is deliberately NOT kept beside it: two fields a word apart, one of
        // them the file and one of them a description of it, is how a later change reaches for the wrong one.
        QString patchPath;
        RomhackTarget target;      // the dump the source says it was built for, when it said
    };
    // The second half of the romhack flow: unpack the base ROM if needed, patch it, install the result as its
    // own library game, then offer to play it. Split out because it now has TWO callers — the game was
    // already on disk, or it has just finished downloading.
    void applyRomhack(const QString& baseRom, const PendingRomhack& req);
    // The shared tail of an install: metadata, library rescan, and the offer to play. Both routes end here —
    // patched from a base ROM, or a finished ROM written straight to the library.
    void finishRomhackInstall(const QString& installed, const QString& displayTitle,
                              const PendingRomhack& req);
    // Queue the base game's download and arrange for applyRomhack to run when it lands. Returns false if the
    // download could not be started, in which case nothing is left pending.
    bool downloadBaseRomThenApply(const PendingRomhack& req);
    // The tail of the flow, once the patch is on disk and every question has been answered: either the base
    // game still has to be fetched, or it is already there and this applies straight away. One function
    // because there are two ways to arrive here — the patch came down inline, or it came through the download
    // queue minutes later — and they must not drift into two slightly different endings.
    void resumeRomhackAfterPatch(const PendingRomhack& req, bool needBaseRom);
    // Drop patch files nobody is coming back for. A patch is kept after a FAILED install on purpose — that is
    // the retry cache, and it is what makes a second press cost nothing — but "kept" cannot mean "forever" at
    // disc scale.
    void pruneRomhackPatchCache();
    // The picker itself: a NavMenu of StremioTranslate::describe rows. seriesKey is PINNED by the caller at
    // REQUEST time — listStremioStreams is async and the user can move on, and keying the remembered choice
    // off whatever is current when the reply lands would file it under the wrong show (the subtitle picker
    // was fixed for exactly this in a94e995; the intro-skip marks menu repeated it).
    void presentStreamCandidates(const QVector<StremioTranslate::StreamCandidate>& all,
                                 const QString& seriesKey, const MediaItem& item);
    // Play one chosen candidate: a direct http url as-is, else its infoHash through the SAME TorBox
    // resolution the automatic path uses (AddonManager::resolveTorBoxInfoHash) — no second copy of the chain.
    void playChosenStream(const MediaItem& item, const StremioTranslate::StreamCandidate& c);
    // Staleness latch for the picker, the shape nextEpGen_/channelAirGen_ already use here. listStremioStreams
    // fans out to every enabled Stremio add-on (15 s transfer timeout) and answers only after the last one, so
    // a reply can easily land after the user backed out and started playing something else. The gen is
    // captured at REQUEST time and re-checked before the reply touches ANY shared surface — including
    // hideNotice(), which would otherwise clear a notice the new playback raised. Bumped by every media open
    // and every themed-detail pop via bumpChooseSourceGen(), which also clears the in-flight flag.
    void bumpChooseSourceGen();
    int  chooseSourceGen_ = 0;
    bool chooseSourceBusy_ = false;   // a request is in flight: a second press must not stack a second menu
    // The romhack flow waits on the network with no overlay up, so the UI stays live and a second press can
    // start a second flow on top of the first — two interleaved installs writing the same status line.
    bool romhackBusy_ = false;
    // One hack waiting on its base ROM download, and the connection watching for that download to finish.
    // One at a time: a second would silently replace the first, and the first's patch bytes would be lost
    // after the user had already confirmed it.
    QScopedPointer<PendingRomhack> pendingRomhack_;
    QMetaObject::Connection pendingRomhackConn_;
    // The hack ids whose FINISHED-GAME download is in flight. romhackBusy_ cannot cover this route: it is
    // released when showRomhacks returns, which here is the moment the job is enqueued, so the whole download
    // — hours, at disc size — is re-enterable. Nothing else catches a repeat either: only the ".part" exists
    // yet so the destination-exists check passes, and enqueue() de-dups by dest while our handler matches on
    // key, so a second press folds into ONE job with TWO handlers armed and the second fires inside the
    // first's NavConfirm loop (#28). A SET rather than a single slot on purpose: two DIFFERENT hacks
    // downloading together are not in conflict, and making them exclusive would be a bug of its own.
    QSet<QString> romhackRomDownloads_;
    // The hack ids whose PATCH transfer is in flight. Separate from romhackRomDownloads_ above because they
    // are different transfers with different endings — a finished ROM is the install, a patch is the step
    // before it — and a shared set would make one hack's patch refuse another hack's finished ROM.
    //
    // Same three reasons that one exists, all of which apply identically here: romhackBusy_ is released when
    // showRomhacks returns, which on this route is the moment the job is enqueued; only the ".part" exists so
    // no destination check catches a repeat; and enqueue() de-dups by dest while the handler matches on key,
    // so a second press would fold into ONE job carrying TWO handlers, the second firing inside the first's
    // nested loop (#28).
    QSet<QString> romhackPatchDownloads_;
    void captureVideoScreenshot();                // save the current video frame to <app>/screenshots
    QWidget* subOverlay_ = nullptr;
    // The panel is a two-column card: track list (left) and sync/size/load/download (right). Up/Down move
    // within a column, Left/Right jump between them - so you reach the settings without walking the track list.
    QVector<QPushButton*> subLeftCol_;
    QVector<QPushButton*> subRightCol_;
    QObject* subTrackScroll_ = nullptr; // the track list's QScrollArea, filtered so it can't eat arrow keys
    // Resume key of the currently-playing media (raw, unhashed — SyncOffsets hashes internally). Set by each
    // play path beside its beginResume() so the card's sync controls read/write the right per-file offsets;
    // empty when nothing is playing (cleared on queueCleared, i.e. whenever we leave the media).
    QString syncKey_;

    // Gapless playback armed for the CURRENT media (issue #141): true only while an audio queue is playing with
    // the "Gapless playback" setting on. Gates every gapless behaviour so the off path is byte-for-byte the
    // pre-#141 one: when false, the per-track EOF drives handleTrackEnd as before, mpv's playlist-pos signal is
    // ignored, and the append feed never runs. Set true only at the two local-audio-queue starts; forced false
    // for video/IPTV queues, single-file streams, and on leaving the media (queueCleared).
    bool gaplessAudioActive_ = false;

    // Crossfade armed for the CURRENT media (issue #141): true only while a local audio queue with more than
    // one track is playing and the crossfade setting is non-zero. Gates every crossfade behaviour, so with the
    // setting off nothing below runs at all - no tag read, no second deck, no deferred append. crossfadeSecs_
    // is the decision for the boundary out of the CURRENT track (0 = this boundary is not a crossfade), and
    // crossfadeGen_ stamps which file that decision belongs to, against the same nextEpGen_ counter durGen_ /
    // posGen_ use - a decision is only usable while it still names the file on screen.
    //
    // crossfadeSpent_ is a LATCH on the boundary, not a "a window is running" flag, and the difference is what
    // keeps a failure from becoming a loop: it is set when the overlap is handed to the player and cleared
    // only when the NEXT file's decision is made. If the incoming file turns out to be unopenable the player
    // abandons the window (MpvWidget's inactive-deck END_FILE branch) and the outgoing track carries on - and
    // a flag that tracked the window would go false right there, letting the next position tick re-attempt the
    // same dead file every 100 ms until the track ended. A boundary gets one attempt.
    bool   crossfadeArmed_ = false;
    double crossfadeSecs_ = 0.0;
    int    crossfadeGen_ = -1;
    bool   crossfadeSpent_ = false;

    // External (standalone) emulators: the launch pipeline + process lifecycle lives in GameLauncher; this window
    // keeps only the in-app "playing in <emulator>" wait page it drives via signals, and the state to restore
    // after the emulator exits.
    GameLauncher* launcher_ = nullptr;
    QWidget* emuPage_ = nullptr;
    QLabel* emuLabel_ = nullptr;
    QPushButton* emuStopBtn_ = nullptr;
    Qt::WindowStates emuReturnState_ = Qt::WindowNoState; // our window state to restore after the emulator exits
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    // IPTV channel list (#75): a channel queue is sectioned by group-title, so the playlist_ widget grows
    // non-selectable group-header rows and its row indices no longer map 1:1 to session track indices.
    // These carry the per-entry group/logo alongside the queue (set just before setQueue, consumed by the
    // queueChanged handler), and the two maps translate between widget rows and track indices for clicks and
    // for highlighting the current channel. All empty/identity for a plain audio queue.
    QStringList pendingChannelGroups_ = {}; // group-title per entry, parallel to the queue (consumed once)
    QStringList pendingChannelLogos_  = {}; // tvg-logo per entry, parallel to the queue (consumed once)
    QVector<int> plRowToTrack_ = {};        // playlist_ row -> session track index (-1 for a header row)
    QVector<int> plTrackToRow_ = {};        // session track index -> playlist_ row (for setCurrentRow)
    // Async channel-logo art for the classic in-player playlist_ (#75): each IPTV rebuild bumps the generation
    // so a late reply from a prior list can't paint a reused row, and a bounded queue keeps a big playlist from
    // firing thousands of requests at once (mirrors HomeView::pumpThumbnails). A missing/failed logo stays
    // text-only. pumpChannelLogos() drains the queue; docNam_ (lazily created) fetches the bytes.
    QVector<QPair<int, QString>> channelLogoQueue_ = {}; // (playlist_ row, remote logo URL) still to fetch
    int channelLogoActive_ = 0;             // in-flight logo fetches (bounded by kMaxChannelLogoFetch)
    int channelLogoGen_ = 0;                // bumped on every playlist_ rebuild; guards a stale reply's setIcon
    void pumpChannelLogos();                // fetch queued channel logos, capped; setIcon on arrival
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QFrame* mediaControls_ = nullptr; // floating transport overlay over the player
    QPushButton* videoBack_ = nullptr; // top-left "Back" overlay to exit the movie
    QPushButton* streamIssueBtn_ = nullptr; // top-left "Issue with Streaming" overlay (next to Back) for Allarr media
    // The skip affordance shown when auto-skip is OFF. Deliberately a plain child of player_ like
    // streamIssueBtn_ above and NOT a NavOverlay: every overlay grabs all input (keyboard grab + NavContext
    // routing, topmost owns everything), which is exactly wrong for a non-modal prompt over live video that
    // the user is free to ignore.
    QPushButton*           skipChip_ = nullptr;
    QTimer*                skipChipTimer_ = nullptr; // its OWN life, not the shared 4 s controlsHideTimer_
    MediaSegments::Segment skipChipSeg_;             // what the visible chip would skip
    // One predicate behind both the chip's LABEL and its action, so "Next Episode" is never a button that does
    // nothing: tryPlayNextEpisode() silently no-ops without episode context.
    bool skipChipHandsOff(const MediaSegments::Segment& seg) const;
    void showSkipChip(const MediaSegments::Segment& seg);
    void hideSkipChip();
    void activateSkipChip();
    bool currentNextSourceCapable_ = false; // the open media came from a file provider that can serve another source
    // ---- The remote audiobook the queue is currently playing (#214) --------------------------------------
    // Everything needed to MINT the link for a part when the app reaches it, and nothing that expires.
    //
    // A per-session table rather than anything persisted, on purpose: `partIds_` holds the SOURCE's ids for
    // this release's files, which mean something only to that source and only for as long as that release is
    // the one it picked. The tokens the QUEUE holds are the durable half and they are not stored here at all
    // — they are derived from the book key and the file name, so a resume row written today is still readable
    // by a build that resolves a different release tomorrow.
    QHash<QString, QString> remoteBookPartIds_;   // part token -> that part's source item id
    QHash<QString, QString> remoteBookMinted_;    // part token -> a link already minted THIS session
    // Bumped by every new queue and every jump. A mint that comes back carrying a stale generation is
    // dropped: a slow answer for a part the listener skipped past must not play over the one they chose.
    quint64 remoteBookGen_ = 0;
    class Notifier* notifier_ = nullptr;    // the app's single user-feedback channel (window notice + player notice)
    class StreamResolver* streams_ = nullptr; // .m3u/.m3u8 playlist + stream-link classification (see connect block)
    class PlaybackSession* session_ = nullptr; // audio-queue + resume state machine (see connect block)
    QVector<QPushButton*> playerButtons_; // transport buttons in Left/Right arrow-nav order
    // Where the transport cursor was when the chrome auto-hid. hideMediaControls() has to clear focus (a
    // hidden button must not hold it), which otherwise made the next arrow press re-enter at an END of the
    // row — you were on the volume and came back to skip-back. Restored on the next entry, so a bar that
    // hides under you and comes straight back feels continuous rather than reset.
    QPointer<QPushButton> lastPlayerFocus_;
    QTimer* controlsHideTimer_ = nullptr;
    QTimer* playerTapTimer_ = nullptr;  // pending single-tap; a 2nd tap within 350ms upgrades it to a seek
    QPointF playerTouchStart_;          // TouchBegin pos, for the tap-vs-drag discriminator
    bool    playerTouchTap_ = false;    // the in-flight touch is still a tap candidate (small travel, 1 finger)
    QStackedWidget* stack_ = nullptr;
    QSlider* seek_ = nullptr;
    QLabel* time_ = nullptr;
    QSlider* volume_ = nullptr;        // player volume (0..200; above 100% = software boost)
    QPushButton* muteBtn_ = nullptr;   // speaker / mute toggle
    QPushButton* speedBtn_ = nullptr;  // playback-speed cycle button (shows the current rate)
    QPushButton* stopBtn_ = nullptr;   // transport Stop — audio only (video leaves with Back; see applyRememberedSpeed)
    void setPlaybackSpeed(double s);   // apply a speed + refresh the button label
    void cyclePlaybackSpeed(int dir);  // step to the next/previous preset speed
    // Per-item speed memory (issue #140). speedItemKey_ is the stable resume key of the currently-loaded audio
    // item (empty for video / nothing loaded); speedIsMusic_ marks whether it defaults to 1x (music) or to the
    // global default (audiobook/podcast). Set at each audio open; consumed when applying + persisting speed.
    QString speedItemKey_;
    bool    speedIsMusic_ = true;
    // Which file speedIsMusic_ was computed for (#141 crossfade), against the same nextEpGen_ counter durGen_
    // uses. The crossfade decision needs BOTH the music split and the length, and reading a stale music answer
    // for the previous track is exactly how an audiobook would get crossfaded after an album.
    int     musicGen_ = -1;
    void applyRememberedSpeed();       // resolve + apply this item's speed on load (audio only)
    void persistItemSpeed(double s);   // remember a user-chosen speed for the current audio item

    // Sleep timer (issue #140). sleepBtn_ is the transport entry point; sleepExpirySec_ is the absolute
    // playback-second the armed timer fires at (<0 = not armed); sleepBaseVolume_ is the volume the fade ramps
    // DOWN from, captured at arm so an extend/cancel can restore it. The pure decision lives in SleepTimer.h.
    QPushButton* sleepBtn_ = nullptr;
    double sleepExpirySec_  = -1.0;
    int    sleepBaseVolume_ = 100;
    void openSleepTimerMenu(QWidget* anchor);          // the transport menu (presets / End of chapter / Custom / Off)
    void armSleepTimer(int mode, double minutes);      // mode: 0 minutes, 1 end-of-chapter (see the .cpp)
    void cancelSleepTimer();                           // disarm + restore the pre-fade volume
    void tickSleepTimer(double posSec);                // per position tick: drive the fade, then fire at expiry
    // Audio bookmarks (issue #140). bookmarkBtn_ is the transport entry point; the menu drops a bookmark at the
    // live position and jumps to / removes any stored for the current item (AudioBookmarkStore owns the list +
    // its cross-device sync). audioSkipStep() is the transport skip amount — the configured jump interval for
    // audio, the historical 10 s for video.
    QPushButton* bookmarkBtn_ = nullptr;
    void openAudioBookmarksMenu(QWidget* anchor);
    double audioSkipStep() const;
    bool muted_ = false;
    // Inline settings/panel page (replaces popup dialogs).
    QWidget* panelPage_ = nullptr;
    QScrollArea* panelScroll_ = nullptr;
    QLabel* panelTitle_ = nullptr;
    QPushButton* panelBack_ = nullptr;     // the panel header's Back button (arrow-key reachable from the top)
    // The page to return to when the top-level panel's Back is hit. A QPointer, not a raw one: the pages it can
    // hold (the themed home/browse QQuickWidgets) are destroyed and rebuilt under it by showHomeScreen(), so a
    // raw pointer here is a dangle waiting for a destination change. See leaveSettingsArea's SettingsReturn note.
    QPointer<QWidget> panelReturnTo_;
    // An embedded dialog hosted in the panel (owns keyboard nav), or null. A QPointer for the same reason as
    // panelReturnTo_ above, and a HARDER one: the dialog is a child of the panel content widget, and
    // showPanel's `panelScroll_->setWidget(content)` deletes that content SYNCHRONOUSLY. Issue #122 is the
    // dangling read that follows — the very next line, `stack_->setCurrentWidget(panelPage_)`, emits
    // currentChanged, whose slot is updateNavForPage(), which type-tests this pointer. A raw pointer is
    // therefore freed-but-non-null for the duration of that call: QObject::inherits dispatches through the
    // dead object's vptr and takes an access violation. QPointer nulls the moment the dialog dies, so the
    // slot sees "no dialog" — which is the truth — however the destruction was reached.
    QPointer<QWidget> panelDialog_;
    std::function<void()> panelOnBack_;
    double duration_ = 0.0;
    double lastPos_ = 0.0;   // last reported playback position, for the segment marks menu
    // Which playback epoch (nextEpGen_) these two were last reported FOR. mpv reports neither until well after an
    // open, so between the open and its first callback they still hold the PREVIOUS file's numbers. These are the
    // player's own live transport state and must NOT be zeroed on an open (a play ATTEMPT that fails leaves a
    // still-running file with a dead slider and a "0:00" length) — so segment code asks "is this MY file's number
    // yet?" instead, with the same nextEpGen_ epoch the marks menu already uses for the cross-episode case.
    // -1 = never reported.
    int durGen_ = -1;
    int posGen_ = -1;
    bool sliderDown_ = false;
    bool focusedOnShow_ = false; // ensure we grab keyboard focus only once, on the first show
    bool forceClose_ = false;        // set once the exit push completes, so closeEvent stops deferring the quit
    bool startupChooseProfile_ = false; // show the profile picker inline on first show
};
