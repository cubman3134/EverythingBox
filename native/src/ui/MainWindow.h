#pragma once
#include <QMainWindow>

#include <QStringList>
#include <QVector>
#include <QHash>
#include <QColor>
#include <QPointer>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include "../addons/AddonModels.h"

class MpvWidget;
class RetroView;
class EbookView;
class PdfView;
class ComicView;
class LibraryView;
class BackgroundMusic;
class HomeView;
class AddonManager;
class CloudSync;
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
    // Documents (CBZ/EPUB/PDF) open through file-based readers, so a remote http(s) url must be
    // fetched to a local cache file first; this downloads then re-enters openLibraryItem locally.
    void fetchRemoteDocumentThenOpen(const MediaItem& item, const QString& ext);
    // Download (for keeps) a resolved item to <app>/downloads and record it. Fed by HomeView's downloadItem
    // signal (single item or a crawl); handed to the persistent DownloadManager which runs + tracks them.
    void enqueueDownload(const MediaItem& item);
    void openDownloadManager();          // Settings ▸ Downloads: the download-manager panel
    void updateDownloadRow(const QString& id); // refresh one job's progress bar/label in place
    // Window-level notification overlay for download/resolve progress + errors. A top-level Tool window (not a
    // child widget) so it floats over ANY current view, including a themed home (a native QQuickView our own
    // child widgets can't paint over). Driven by HomeView's toastRequested/toastHideRequested and by the
    // library download queue, so the "info while pulling a file" feedback shows regardless of the active theme.
    void notify(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide)
    void hideNotice();
    void positionNotice();                           // re-anchor to our bottom-centre (global coords)
    // A manga chapter resolves to a list of page image URLs; download them, pack into a cached CBZ,
    // then hand it to the comic reader (which gives natural page order + resume for free).
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls);
    void openSettingsHub();   // centralized "Settings" area (emulator + input)
    void openGeneralSettings(); // general playback options (subtitle defaults)
    void openCloudSync();     // Google Drive sign-in + sync panel
    void openCloudClientSetup(); // inline form to paste the Google OAuth client id/secret
    void cloudSyncNow();      // pull (if newer) then push the current state
    void openDebug();         // diagnostic log viewer (refresh / clear / open file location)
    void confirmUninstall();  // Settings ▸ Uninstall: warn, then performUninstall() on confirm
    void performUninstall();  // remove the app folder + cache/registry/dumps via a detached post-exit script
    void openRetroAchievements(); // RetroAchievements sign-in panel
    void openBiosCheck();         // per-system BIOS presence check + download-missing (RetroBat-style)
    void openEmulatorSettings();
    void openInputMapping();
    void nextTrack();
    void prevTrack();
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

private:
    class DownloadManager* dm_ = nullptr;
    // Live widgets in the open Downloads panel, keyed by job id, so progress ticks update in place without
    // rebuilding (which would steal keyboard/controller focus). Repopulated each time the panel is built.
    QHash<QString, class QProgressBar*> dlBars_;
    QHash<QString, class QLabel*> dlStatus_;
    bool dlPanelOpen_ = false;           // the Downloads panel is the current view (rebuild it on state changes)

    static QString fmt(double seconds);
    // Path-based open helpers: open the file AND record it in the Recent list (the dialog-based
    // openFile/openAudio/openGame/openDocument and the Recent tab both route through these).
    void openVideoPath(const QString& path);
    void openAudioPath(const QString& path);    // queue the whole folder, starting at this file
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
    void launchPcExe(const QString& exe, const QString& id, const QString& title, const QString& thumb);
    // Play-time tracking for the full-screen emulator / external-emulator flow: stamp last-played + start the
    // clock when a game begins, and bank the elapsed session when it ends. (PC games are timed separately in
    // launchPcExe, off their own process handle.) beginPlaySession auto-closes any session still open.
    void beginPlaySession(const QString& identity);
    void endPlaySession();
    QString activePlayId_;          // identity of the game currently being timed ("" = none)
    qint64  activePlayStart_ = 0;   // epoch seconds the active session began
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
    // Systems flagged as external (GameCube/Wii via Dolphin) run in a standalone emulator launched as a
    // child process: ensure it's installed (auto-download), boot the ROM, and show a wait page until it exits.
    void launchExternalGame(const GameSystem* sys, const QString& rom, const QString& title,
                            const QString& thumb, const QString& key);
    // Run a standalone emulator: stop our playback, show the wait page, minimise, and launch (auto-installing
    // if needed). rom empty => open the emulator's own UI (e.g. TeknoParrot, or another emulator for setup).
    void runEmulator(const ExternalEmulator& em, const QString& rom = QString(), const QString& title = QString(),
                     const QString& thumb = QString(), const QString& key = QString(), const QString& system = QString());
    void ensureEmu();        // lazily create EmulatorManager + wire its signals
    void ensureEmuPage();    // lazily build the "playing in <emulator>" wait page
    void openEmulatorManager(); // Settings > Emulators: folder + per-emulator install status
    void openStreamPrompt();                    // inline form to paste a stream/URL link
    void openStreamUrl(const QString& url, const QString& resumeKey = QString(),
                       const QString& title = QString()); // route an http(s) link (or .m3u/.m3u8) to libmpv
    void playStream(const QString& url, const QString& resumeKey = QString(),
                    const QString& title = QString());    // play a single resolved link via libmpv
    // .m3u/.m3u8 playlists: openM3u reads the file/URL, handleM3u dispatches its contents - an HLS manifest
    // streams via libmpv, an IPTV/media list becomes a channel queue, a PlayStation disc list opens the emulator.
    void openM3u(const QString& src, const QString& title = QString());
    void handleM3u(const QString& src, const QString& text, const QString& title);
    // Stream an http(s) audiobook/audio link in the now-playing audio view (playlist + transport). Resume +
    // Recent key on resumeKey (the stable item id) since a debrid URL is re-resolved fresh each open.
    void openAudioStream(const QString& url, const QString& resumeKey, const QString& title,
                         const QString& thumbnailUrl = QString());
    void openDocumentPath(const QString& path); // .epub / .pdf by extension
    void setAudioQueue(const QStringList& files, int startIndex, const QStringList& titles = {});
    void playTrack(int index);
    void clearAudioQueue();   // leave audio mode (video/game/doc)
    // Resume tracking for timed media (video / audio / audiobooks): remember the playback position per file.
    void beginResume(const QString& path); // start tracking this file (and queue its saved spot to seek to)
    void persistResume();                  // save the current file's position (throttled / on leave / on exit)
    void finishResume();                   // a file played to the end -> drop its saved position
    void toggleFullScreen();
    void leaveFullScreen();   // restore windowed: status bar + cursor

    // App pause menu (Esc): a small "Resume / Exit My Media Vault" overlay, à la the in-game pause menu.
    // A NavMenu (in-window child overlay from the nav kit) — it renders over the themed QML surface without
    // spawning a separate OS window, and restores the previous selection when it closes.
    void showEscMenu();
    void hideEscMenu();
    bool escMenuVisible() const;
    // The one Back rule, shared by Escape, Backspace and the controller's Back: close the topmost overlay /
    // pause menu, else go to the previous screen for whatever page is showing, and at the home root open the
    // app pause menu. Lives in the base window so every screen behaves identically. themedOnBack_ carries the
    // themed (QML) home's own multi-level back (drill up, then the menu), set when that home is built.
    void goBack();
    std::function<void()> themedOnBack_;
    QPointer<class NavOverlay> escMenuOverlay_; // alive while the pause menu is open

    // The controller-navigation kit (src/ui/nav): overlay routing, the panel's selection ring, and the
    // per-screen Back action. updateNavForPage() re-registers both whenever the stack page changes, so
    // every screen gets arrow navigation + a working Back without per-screen wiring.
    // UI-test/automation channel (core/UiTestServer): created when enabled (env var / --uitest / the
    // Settings ▸ Debug toggle), torn down when the toggle turns it off. updateUiTestServer() reconciles.
    class UiTestServer* uiTest_ = nullptr;
    void updateUiTestServer();
    void addThemedSelection(class QJsonObject& o, QWidget* page); // themed-home selection -> UI-test state

    class NavContext* navCtx_ = nullptr;
    class NavRing* panelRing_ = nullptr;   // covers panelPage_ (header Back button + the built rows)
    class NavRing* libraryRing_ = nullptr; // covers the Library view (lists + buttons + search)
    void updateNavForPage();

    // Controller navigation of the menus (EmulationStation-style): poll the shared gamepad on menu screens and
    // synthesise the arrow / Enter / Back keys the UI already understands, with a stick deadzone (in Gamepad)
    // and hold-to-repeat. RetroView owns the pad in-game, so this stays out of its way.
    void pollMenuPad();
    void sendNavKey(int key);   // deliver a synthetic key to the active view (themed QML window, panel, etc.)
    QTimer* padNavTimer_ = nullptr;
    qint64  padTick_ = 0;       // accumulated ms (fixed poll interval), for the repeat clock
    bool    padPrev_[8] = { false };  // per-nav-input: was it held last tick (edge detection)
    qint64  padNext_[8] = { 0 };      // per-nav-input: tick at which a held direction may repeat again
    void revealMediaControls();
    void positionMediaControls();
    void showPlayerNotice(const QString& msg, int ms = 6000); // centred transient message over the player
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
    QWidget* firstPanelRow() const;     // the first focusable row in the current panel content (or null)
    QVector<QWidget*> panelNavRing() const; // Back + the panel's focusable rows, top-to-bottom (arrow/Tab nav)

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    EbookView* book_ = nullptr;
    PdfView* pdf_ = nullptr;
    ComicView* comic_ = nullptr;
    LibraryView* library_ = nullptr;
    BackgroundMusic* bgm_ = nullptr;    // menu background music; plays on menu screens, pauses on content
    void updateBackgroundMusic();       // play/pause the BGM to match the current view
    void updateThemedNowPlaying();      // push the current BGM track name into the themed home (Triple theme)
    HomeView* home_ = nullptr;

    // Themed (QML) home, opt-in via "themedHome/enabled" (default off). showHomeScreen() routes Home to it
    // or the classic HomeView. The themed-home methods are no-ops in builds without the QML engine.
    void showHomeScreen();
    bool themedHomeEnabled() const;
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
    QStringList themedXmbCatKeys_;  // XMB: category index -> bucket key ("video"/.../"settings")
    QVariantList themedXmbCatalogs_; // XMB: the current bucket's catalog list (the column when not drilled in)
    bool themedXmbInCatalog_ = false; // XMB: column shows a catalog's live items (true) vs the catalog list (false)
    bool themedXmbAutoOpened_ = false; // XMB: the bucket's single catalog was opened directly (its contents ARE the root)
    int themedXmbCatalogIndex_ = 0;    // XMB: which catalog in the list we opened, so Back re-selects it
    QTimer* themedMetaTimer_ = nullptr; // XMB: debounce the live-metadata addon fetch to the settled row
    int themedMetaWant_ = -1;           // XMB: the browse index that pending fetch is for
    void refreshThemedMeta(int browseIndex); // XMB: set the panel's skeleton for a row + queue the addon enrich
    bool themedReturnAfterDetail_ = false; // showing the classic info page over a themed home; return on back
    QWidget* themedDetailFrom_ = nullptr;  // the themed widget to return to after the info page
    class QFileSystemWatcher* themeWatcher_ = nullptr; // hot-reload: rebuild the themed home on theme.json edits

    class SplitView* splitView_ = nullptr;   // two-pane split screen (its own engines per pane)
    class MediaPane* splitTarget_ = nullptr; // the pane the next opened item loads into (split "Open here")
    bool splitMode_ = false;                 // currently showing the split screen
    class Achievements* ach_ = nullptr;      // RetroAchievements client (full-screen emulator)
    std::unique_ptr<AddonManager> addons_;
    std::unique_ptr<CloudSync> cloud_;
    // "Continue watching" cloud sync: a small resume+recent JSON file, pulled+merged on startup and pushed
    // (debounced) when a position changes — separate from the heavy state bundle so it stays timely across devices.
    QTimer* progressSyncTimer_ = nullptr;
    void scheduleProgressSync();          // (re)arm the debounced push after a resume/recent change
    void pushProgressNow();               // serialize local progress + upload the small JSON to Drive
    void pullAndMergeProgress();          // download remote progress + merge into local, then refresh the home view
    QByteArray serializeProgress() const; // current resume positions + per-profile recent lists -> JSON
    void mergeProgress(const QByteArray& json); // merge remote JSON into local by recency (never deletes local)
    QNetworkAccessManager* docNam_ = nullptr; // lazily created: fetches remote CBZ/EPUB/PDF to a cache file

    class AppUpdater* updater_ = nullptr; // checks GitHub Releases for a newer app build + installs it in place

    // Auto-subtitle download (OpenSubtitles): when a movie/episode video loads with no subtitle in the
    // preferred language, fetch one and load it into the player. subCtx_ holds the current video's match
    // hints, set only for eligible opens and consumed once by the MpvWidget::fileLoaded handler.
    class SubtitleFetcher* subFetcher_ = nullptr;
    struct SubContext { QString imdbStreamId; QString title; bool active = false; } subCtx_;
    void armSubtitleFetch(const MediaItem& item); // set subCtx_ if this video is eligible for auto-subtitles
    // When a TV episode finishes, resolve + play the next one (same season ep+1, then next season ep1).
    void tryPlayNextEpisode();
    void playResolvedEpisode(const QString& imdbStreamId, const QString& url, const QString& mime);

    // Casting the current stream to a Chromecast / DLNA device on the LAN. castUrl_ etc. hold the currently
    // playing stream so the picker can hand it to the chosen device.
    class CastManager* castMgr_ = nullptr;
    QString castUrl_, castTitle_, castMime_;
    void showCastMenu(QWidget* anchor);           // device picker popup for the cast button

    // Trakt.tv scrobbling: mark movies/episodes watched as you play them. scrobbleImdb_ is the id currently
    // being scrobbled (empty when nothing is).
    class TraktClient* trakt_ = nullptr;
    QString scrobbleImdb_;
    void startScrobble(const QString& imdbStreamId); // begin scrobbling a video (stops any prior one)
    void stopScrobble();                             // stop + mark-watched the current scrobble

    // Parental gate: true if the action may proceed. When a restricted (kids) profile is active and a PIN is
    // set, prompt for it; otherwise allow. `reason` is shown in the prompt.
    bool parentalUnlock(const QString& reason);
    // The player bar's single subtitle button opens a full-player overlay panel (Stremio-style): track pick,
    // sync/size, load-from-file, download. subOverlay_ is the scrim+panel (null when closed); the focusable
    // controls are collected in subPanelButtons_ for arrow/remote navigation, like the transport row.
    void showSubtitleMenu();
    void hideSubtitleMenu();
    void captureVideoScreenshot();                // save the current video frame to <app>/screenshots
    QWidget* subOverlay_ = nullptr;
    // The panel is a two-column card: track list (left) and sync/size/load/download (right). Up/Down move
    // within a column, Left/Right jump between them - so you reach the settings without walking the track list.
    QVector<QPushButton*> subLeftCol_;
    QVector<QPushButton*> subRightCol_;

    // External (standalone) emulators: install/run manager + the in-app wait page shown while one runs.
    EmulatorManager* emu_ = nullptr;
    QWidget* emuPage_ = nullptr;
    QLabel* emuLabel_ = nullptr;
    QPushButton* emuStopBtn_ = nullptr;
    QString pendingEmuRom_, pendingEmuTitle_, pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_; // Recent entry, added on launch
    Qt::WindowStates emuReturnState_ = Qt::WindowNoState; // our window state to restore after the emulator exits
    // While a standalone emulator (melonDS, Dolphin…) owns the screen, watch for a global exit hotkey — Start+Select
    // on a pad, or Esc on the keyboard — and close it back to MMV, the way RetroBat does. Runs only between the
    // emulator's launched and finished signals (MMV is minimized then, so Qt can't see the input itself).
    QTimer* emuHotkeyTimer_ = nullptr;
    bool emuComboPrev_ = false;          // edge-detect: Start+Select was held last poll
    bool emuEscPrev_ = false;            // edge-detect: Esc was held last poll
    void startEmuHotkeyWatch();
    void stopEmuHotkeyWatch();
    void pollEmuExitHotkey();
    // Detect a standalone emulator that closes almost immediately (a failed boot — usually a missing BIOS/firmware,
    // which -batch-style launches exit silently on). Only warn when the user didn't close it themselves.
    QElapsedTimer emuRunClock_;
    QString emuDisplayName_;              // the running emulator's display name (from the launched signal)
    bool emuUserClosing_ = false;         // set when WE ask it to close (exit hotkey / force-close), to suppress the warning
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QFrame* mediaControls_ = nullptr; // floating transport overlay over the player
    QPushButton* videoBack_ = nullptr; // top-left "Back" overlay to exit the movie
    QPushButton* streamIssueBtn_ = nullptr; // top-left "Issue with Streaming" overlay (next to Back) for Allarr media
    bool currentNextSourceCapable_ = false; // the open media came from a file provider that can serve another source
    QLabel* playerNotice_ = nullptr;        // transient centred message over the player (works in full screen)
    QTimer* playerNoticeTimer_ = nullptr;
    QLabel* notice_ = nullptr;              // window-level download/resolve notice overlay (over any theme)
    QTimer* noticeTimer_ = nullptr;         // auto-hides the notice
    QVector<QPushButton*> playerButtons_; // transport buttons in Left/Right arrow-nav order
    QTimer* controlsHideTimer_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QSlider* seek_ = nullptr;
    QLabel* time_ = nullptr;
    QSlider* volume_ = nullptr;        // player volume (0..200; above 100% = software boost)
    QPushButton* muteBtn_ = nullptr;   // speaker / mute toggle
    QPushButton* speedBtn_ = nullptr;  // playback-speed cycle button (shows the current rate)
    void setPlaybackSpeed(double s);   // apply a speed + refresh the button label
    void cyclePlaybackSpeed(int dir);  // step to the next/previous preset speed
    bool muted_ = false;
    // Inline settings/panel page (replaces popup dialogs).
    QWidget* panelPage_ = nullptr;
    QScrollArea* panelScroll_ = nullptr;
    QLabel* panelTitle_ = nullptr;
    QPushButton* panelBack_ = nullptr;     // the panel header's Back button (arrow-key reachable from the top)
    QWidget* panelReturnTo_ = nullptr;     // the page to return to when the top-level panel's Back is hit
    QWidget* panelDialog_ = nullptr;       // an embedded dialog hosted in the panel (owns keyboard nav), or null
    std::function<void()> panelOnBack_;
    double duration_ = 0.0;
    bool sliderDown_ = false;
    bool focusedOnShow_ = false; // ensure we grab keyboard focus only once, on the first show
    bool forceClose_ = false;        // set once the exit push completes, so closeEvent stops deferring the quit
    bool startupChooseProfile_ = false; // show the profile picker inline on first show

    QStringList tracks_;     // current audio queue (absolute paths)
    int trackIndex_ = -1;    // index into tracks_, or -1 when not playing a queue
    QString resumePath_;     // the timed-media file (video/audio/audiobook) whose position we track, or empty
    double resumeSeek_ = 0.0;      // pending resume target applied once the file's duration is known
    double audioPos_ = 0.0;        // last reported playback position
    double lastSavedPos_ = -100.0; // throttle resume writes
};
