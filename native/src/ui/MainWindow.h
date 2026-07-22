#pragma once
#include <QMainWindow>

#include <QStringList>
#include <QVector>
#include <QHash>
#include <QVariantMap>
#include <QColor>
#include <QPointer>
#include <QPointF>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include "../addons/AddonModels.h"
#include "../core/LifecyclePolicy.h"

class MpvWidget;
class RetroView;
class EbookView;
class ReaderChromeHost;
class ThemedPanelHost;
class PdfView;
class ComicView;
class LibraryView;
class BackgroundMusic;
class HomeView;
class AddonManager;
class CatalogPrefetcher;
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
    // Themed Downloads: a job's Progress row is activated to open a NavMenu action chooser (Pause/Resume/Retry/
    // Cancel/Remove per the SAME state logic classic uses for its per-job buttons), mirrored on the panel graph.
    // Themed-only (its body uses the QML panel host); guarded so moc emits no metacall for it in a no-QML build.
#ifdef MMV_HAVE_QML
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
    QVariantMap settingsPanelStyle() const; // the active theme's `settingsPanel` block (themed panels; B2)
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
    mmv::LifecyclePolicy lifecycle_;    // sticky pause/resume decision core

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
    void openStreamUrl(const QString& url, const QString& resumeKey = QString(),
                       const QString& title = QString()); // route an http(s) link (or .m3u/.m3u8) to libmpv
    void playStream(const QString& url, const QString& resumeKey = QString(),
                    const QString& title = QString());    // play a single resolved link via libmpv
    // Stream an http(s) audiobook/audio link in the now-playing audio view (playlist + transport). Resume +
    // Recent key on resumeKey (the stable item id) since a debrid URL is re-resolved fresh each open.
    void openAudioStream(const QString& url, const QString& resumeKey, const QString& title,
                         const QString& thumbnailUrl = QString());
    bool openDocumentPath(const QString& path); // .epub / .pdf / .cbz by extension; true if it opened
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
    void presentBook(); // show book_ themed (wrapped in readerHost_) or classic (direct), per themedHomeEnabled
    void presentPdf();   // show pdf_ themed (wrapped in pdfHost_) or classic, per themedHomeEnabled (Task 4)
    void presentComic(); // show comic_ themed (wrapped in comicHost_) or classic, per themedHomeEnabled (Task 4)
    void captureReaderOrigin(); // record the launch surface into readerOrigin_ (skips a reader-to-reader re-open)

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
    void presentProfilePicker(bool mustChoose);                  // reset()+present() the root list (also for startup, pre-home)
    void presentProfileList(bool mustChoose, bool replace);      // (re)build the profile list rows; replace = in place
    void editProfilePanel(const QString& id, bool mustChoose);   // nested name(TextField)+icon(Choice) picker; id "" = create
    void profileRowMenu(const QString& profileId, bool mustChoose); // Switch/Edit/Delete chooser for a profile row
    void confirmDeleteProfile(const QString& profileId, bool mustChoose);
    void chooseProfile(const QString& id);                       // setCurrent + openHome (the finish for both variants)
    void quitConfirmFromStartup();                               // mustChoose Back: confirm quit, or re-present the list

    // ---- Themed core picker (B2 Task 5): SettingsDialog surface on the Nav Contract. ----
    void presentEmulatorCorePicker();                            // per-system core Choice rows (nested on the hub)
    void editCoreOptions(const QString& systemId);               // per-core options page as a nested panel level

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
    int     remapPort_ = 0; // player port whose profile is being edited

    QWidget* firstPanelRow() const;     // the first focusable row in the current panel content (or null)
    QVector<QWidget*> panelNavRing() const; // Back + the panel's focusable rows, top-to-bottom (arrow/Tab nav)

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
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
    void updateThemedNowPlaying();      // push the current BGM track name into the themed home (Triple theme)
    void applyThemeMusic(const QString& themeDir); // theme.json "music" -> BGM default track (out-of-box music)
    HomeView* home_ = nullptr;

    // Themed (QML) home, gated by "themedHome/enabled" (default ON as of B2 Task 6 — absent key = themed; an
    // explicit stored `false` still selects classic). showHomeScreen() routes Home to it or the classic
    // HomeView. The themed-home methods are no-ops in builds without the QML engine.
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
    bool warnedNoThemes_ = false;  // "no themes installed" fallback notice shown once per run
    QStringList themedXmbCatKeys_;  // XMB: category index -> bucket key ("video"/.../"settings")
    QVariantList themedXmbCatalogs_; // XMB: the current bucket's catalog list (the column when not drilled in)
    bool themedXmbInCatalog_ = false; // XMB: column shows a catalog's live items (true) vs the catalog list (false)
    bool themedXmbAutoOpened_ = false; // XMB: the bucket's single catalog was opened directly (its contents ARE the root)
    int themedXmbCatalogIndex_ = 0;    // XMB: which catalog in the list we opened, so Back re-selects it
    QTimer* themedMetaTimer_ = nullptr; // XMB: debounce the live-metadata addon fetch to the settled row
    int themedMetaWant_ = -1;           // XMB: the browse index that pending fetch is for
    void refreshThemedMeta(int browseIndex); // XMB: set the panel's skeleton for a row + queue the addon enrich
    // The themed DETAIL view (on the Nav Contract, replacing the retired classic info page): open it for the
    // current selection (browseIndex < 0 = the themed root's currentIndex), run one of its action-row verbs on
    // the item it was opened for, and (grid browse) open it for an info-page leaf on Enter.
    void openThemedDetail(int browseIndex);
    bool openThemedDetailForInfoLeaf(int browseIndex); // true if it opened detail (a movie/book/… leaf), else drill
    void runThemedDetailAction(const QString& verb);   // play/download/favorite/playlist on themedDetailIndex_
    int themedDetailIndex_ = -1;             // the browse index the themed detail view is currently showing

    // The themed AUDIO now-playing view (Task 5): in themed mode, audio opens (openAudioPath/openAudioStream/
    // audio queue) route HERE instead of the classic player page — mpv plays invisibly while this QML page is
    // the surface. Following the detail mechanism: a `nowplayingAudio` currentView on the current themed
    // surface, with a pushed "nowplaying" nav level. See MainWindow.cpp for the transport-verb bridge.
    void showThemedAudioPage();                        // switch the current themed surface to the audio page
    void leaveThemedAudioPage(QWidget* surface, const QString& returnView); // the "nowplaying" level's onPop
    QWidget* themedAudioHost() const;                  // the current themed surface (home/browse), or null
    void runThemedAudioTransport(const QString& verb); // play/pause/seek/chapter/track/speed on the live player
    void updateThemedAudioProgress();                  // push the throttled position/duration into the QML props
    void pushThemedAudioQueue();                       // push the session queue titles + current row into the QML
    bool themedAudioSession_ = false;   // the current queue is a themed-mode AUDIO session (route to the page)
    bool themedAudioPaused_ = false;    // our tracked play/pause state for the transport button (reset on a new file)
    QVariantMap themedAudioData_ = {};  // the now-playing item's `selected`-shaped data (art/title/subtitle)
    QStringList themedAudioQueue_ = {}; // the session queue titles (mirrored into the page's queue list)
    int themedAudioCurrent_ = 0;        // the playing row in the queue
    int themedAudioPushSec_ = -1;       // last whole-second position pushed to the page (progress-bar throttle)
    class QFileSystemWatcher* themeWatcher_ = nullptr; // hot-reload: rebuild the themed home on theme.json edits

    class SplitView* splitView_ = nullptr;   // two-pane split screen (its own engines per pane)
    class MediaPane* splitTarget_ = nullptr; // the pane the next opened item loads into (split "Open here")
    // Context the split branch's async core + BIOS fetches are parented to: child of the target pane (a closed
    // pane cancels the pending load), QPointer so deleting a stale one is safe, recreated per split game open.
    QPointer<QObject> splitLaunchCtx_;
    bool splitMode_ = false;                 // currently showing the split screen
    class Achievements* ach_ = nullptr;      // RetroAchievements client (full-screen emulator)
    std::unique_ptr<AddonManager> addons_;
    CatalogPrefetcher* prefetcher_ = nullptr; // background catalog warmer (QObject child of this); kicked post-paint
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
    // Resume key of the currently-playing media (raw, unhashed — SyncOffsets hashes internally). Set by each
    // play path beside its beginResume() so the card's sync controls read/write the right per-file offsets;
    // empty when nothing is playing (cleared on queueCleared, i.e. whenever we leave the media).
    QString syncKey_;

    // External (standalone) emulators: the launch pipeline + process lifecycle lives in GameLauncher; this window
    // keeps only the in-app "playing in <emulator>" wait page it drives via signals, and the state to restore
    // after the emulator exits.
    GameLauncher* launcher_ = nullptr;
    QWidget* emuPage_ = nullptr;
    QLabel* emuLabel_ = nullptr;
    QPushButton* emuStopBtn_ = nullptr;
    Qt::WindowStates emuReturnState_ = Qt::WindowNoState; // our window state to restore after the emulator exits
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QFrame* mediaControls_ = nullptr; // floating transport overlay over the player
    QPushButton* videoBack_ = nullptr; // top-left "Back" overlay to exit the movie
    QPushButton* streamIssueBtn_ = nullptr; // top-left "Issue with Streaming" overlay (next to Back) for Allarr media
    bool currentNextSourceCapable_ = false; // the open media came from a file provider that can serve another source
    class Notifier* notifier_ = nullptr;    // the app's single user-feedback channel (window notice + player notice)
    class StreamResolver* streams_ = nullptr; // .m3u/.m3u8 playlist + stream-link classification (see connect block)
    class PlaybackSession* session_ = nullptr; // audio-queue + resume state machine (see connect block)
    QVector<QPushButton*> playerButtons_; // transport buttons in Left/Right arrow-nav order
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
};
