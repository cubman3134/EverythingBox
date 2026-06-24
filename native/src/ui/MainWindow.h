#pragma once
#include <QMainWindow>

#include <QStringList>
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

// Minimal media-hub window: a stacked surface holding the libmpv video view and the libretro game view,
// with Open Video / Open Game and a transport bar. The shell the rest of the hub grows from.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override; // out-of-line so unique_ptr<AddonManager> is destroyed where it's complete

private slots:
    void openFile();
    void openAudio();
    void openGame();
    void openDocument(); // ebooks (.epub) + PDFs (.pdf), dispatched by extension
    void openHome();
    void onRequestOpenFile(const QString& kind); // from Home's "open a file" item
    void openRecent(const QString& path, const QString& kind); // re-open a Home "Recent" entry
    void onSwitchProfile();                      // pick/create a profile from the Home profile button
    void onThemeChanged(const QColor& background, const QColor& accent); // match the home view's theme
    void openLibrary();
    void openLibraryItem(const MediaItem& item); // route an addon catalog item to the right view
    void openSettingsHub();   // centralized "Settings" area (emulator + input)
    void openGeneralSettings(); // general playback options (subtitle defaults)
    void openCloudSync();     // Google Drive sign-in + sync panel
    void cloudSyncNow();      // pull (if newer) then push the current state
    void openThemes();        // pick a colour theme (with a "Browse Themes…" registry button)
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
    void openGamePath(const QString& path);
    void openDocumentPath(const QString& path); // .epub / .pdf by extension
    void setAudioQueue(const QStringList& files, int startIndex);
    void playTrack(int index);
    void clearAudioQueue();   // leave audio mode (video/game/doc)
    void persistAudiobook();  // save the current .m4b playback position (resume)
    void toggleFullScreen();
    void leaveFullScreen();   // restore windowed: status bar + cursor
    void revealMediaControls();
    void positionMediaControls();
    // Show an in-window panel page (Settings/Theme/Cloud/General are embedded here, no popup windows).
    void showPanel(const QString& title, const std::function<void(QVBoxLayout*)>& build,
                   const std::function<void()>& onBack);

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    EbookView* book_ = nullptr;
    PdfView* pdf_ = nullptr;
    ComicView* comic_ = nullptr;
    LibraryView* library_ = nullptr;
    HomeView* home_ = nullptr;
    std::unique_ptr<AddonManager> addons_;
    std::unique_ptr<CloudSync> cloud_;
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QFrame* mediaControls_ = nullptr; // floating transport overlay over the player
    QPushButton* videoBack_ = nullptr; // top-left "Back" overlay to exit the movie
    QTimer* controlsHideTimer_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QSlider* seek_ = nullptr;
    QLabel* time_ = nullptr;
    // Inline settings/panel page (replaces popup dialogs).
    QWidget* panelPage_ = nullptr;
    QScrollArea* panelScroll_ = nullptr;
    QLabel* panelTitle_ = nullptr;
    QWidget* panelReturnTo_ = nullptr;     // the page to return to when the top-level panel's Back is hit
    std::function<void()> panelOnBack_;
    double duration_ = 0.0;
    bool sliderDown_ = false;
    bool focusedOnShow_ = false; // ensure we grab keyboard focus only once, on the first show

    QStringList tracks_;     // current audio queue (absolute paths)
    int trackIndex_ = -1;    // index into tracks_, or -1 when not playing a queue
    QString audiobookPath_;  // the .m4b currently playing (for resume), or empty
    double audiobookResume_ = 0.0; // pending seek target applied once the file loads
    double audioPos_ = 0.0;        // last reported playback position
    double lastSavedPos_ = -100.0; // throttle resume writes
};
