#pragma once
#include <QMainWindow>

#include <QStringList>
#include <QVector>
#include <QColor>
#include <memory>
#include <functional>
#include "../addons/AddonModels.h"

class MpvWidget;
class RetroView;
class EbookView;
class PdfView;
class ComicView;
class LibraryView;
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
    // Download (for keeps) a resolved item to <app>/downloads and add it to Recent. enqueueDownload queues;
    // startNextDownload runs them one at a time. Fed by HomeView's downloadItem signal (single item or a crawl).
    void enqueueDownload(const MediaItem& item);
    void startNextDownload();
    // A manga chapter resolves to a list of page image URLs; download them, pack into a cached CBZ,
    // then hand it to the comic reader (which gives natural page order + resume for free).
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls);
    void openSettingsHub();   // centralized "Settings" area (emulator + input)
    void openGeneralSettings(); // general playback options (subtitle defaults)
    void openCloudSync();     // Google Drive sign-in + sync panel
    void openCloudClientSetup(); // inline form to paste the Google OAuth client id/secret
    void cloudSyncNow();      // pull (if newer) then push the current state
    void openThemes();        // pick a colour theme (with a "Browse Themes…" registry button)
    void openDebug();         // diagnostic log viewer (refresh / clear / open file location)
    void openRetroAchievements(); // RetroAchievements sign-in panel
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
    void keyPressEvent(QKeyEvent* event) override;          // Esc leaves full screen
    void showEvent(QShowEvent* event) override;             // grab keyboard focus on first show
    void closeEvent(QCloseEvent* event) override;           // push state to Drive on exit

private:
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
    // Systems flagged as external (GameCube/Wii via Dolphin) run in a standalone emulator launched as a
    // child process: ensure it's installed (auto-download), boot the ROM, and show a wait page until it exits.
    void launchExternalGame(const GameSystem* sys, const QString& rom, const QString& title,
                            const QString& thumb, const QString& key);
    // Run a standalone emulator: stop our playback, show the wait page, minimise, and launch (auto-installing
    // if needed). rom empty => open the emulator's own UI (e.g. TeknoParrot, or another emulator for setup).
    void runEmulator(const ExternalEmulator& em, const QString& rom = QString(), const QString& title = QString(),
                     const QString& thumb = QString(), const QString& key = QString());
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

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    EbookView* book_ = nullptr;
    PdfView* pdf_ = nullptr;
    ComicView* comic_ = nullptr;
    LibraryView* library_ = nullptr;
    HomeView* home_ = nullptr;

    // Themed (QML) home, opt-in via "themedHome/enabled" (default off). showHomeScreen() routes Home to it
    // or the classic HomeView. The themed-home methods are no-ops in builds without the QML engine.
    void showHomeScreen();
    bool themedHomeEnabled() const;
    void showThemedHome();
    void openAppearance();
    QWidget* themedHome_ = nullptr;

    class SplitView* splitView_ = nullptr;   // two-pane split screen (its own engines per pane)
    class MediaPane* splitTarget_ = nullptr; // the pane the next opened item loads into (split "Open here")
    bool splitMode_ = false;                 // currently showing the split screen
    class Achievements* ach_ = nullptr;      // RetroAchievements client (full-screen emulator)
    QVector<MediaItem> downloadQueue_;       // pending library downloads (processed one at a time)
    bool downloadBusy_ = false;
    std::unique_ptr<AddonManager> addons_;
    std::unique_ptr<CloudSync> cloud_;
    QNetworkAccessManager* docNam_ = nullptr; // lazily created: fetches remote CBZ/EPUB/PDF to a cache file

    // External (standalone) emulators: install/run manager + the in-app wait page shown while one runs.
    EmulatorManager* emu_ = nullptr;
    QWidget* emuPage_ = nullptr;
    QLabel* emuLabel_ = nullptr;
    QPushButton* emuStopBtn_ = nullptr;
    QString pendingEmuRom_, pendingEmuTitle_, pendingEmuThumb_, pendingEmuKey_; // Recent entry, added on launch
    Qt::WindowStates emuReturnState_ = Qt::WindowNoState; // our window state to restore after the emulator exits
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QFrame* mediaControls_ = nullptr; // floating transport overlay over the player
    QPushButton* videoBack_ = nullptr; // top-left "Back" overlay to exit the movie
    QPushButton* streamIssueBtn_ = nullptr; // top-left "Issue with Streaming" overlay (next to Back) for Allarr media
    bool currentNextSourceCapable_ = false; // the open media came from a file provider that can serve another source
    QLabel* playerNotice_ = nullptr;        // transient centred message over the player (works in full screen)
    QTimer* playerNoticeTimer_ = nullptr;
    QVector<QPushButton*> playerButtons_; // transport buttons in Left/Right arrow-nav order
    QTimer* controlsHideTimer_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QSlider* seek_ = nullptr;
    QLabel* time_ = nullptr;
    QSlider* volume_ = nullptr;        // player volume (0..200; above 100% = software boost)
    QPushButton* muteBtn_ = nullptr;   // speaker / mute toggle
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
