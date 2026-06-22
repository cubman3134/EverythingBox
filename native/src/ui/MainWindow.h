#pragma once
#include <QMainWindow>

#include <QStringList>
#include <memory>
#include "../addons/AddonModels.h"

class MpvWidget;
class RetroView;
class EbookView;
class PdfView;
class LibraryView;
class AddonManager;
class QStackedWidget;
class QSlider;
class QLabel;
class QListWidget;

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
    void openLibrary();
    void openLibraryItem(const MediaItem& item); // route an addon catalog item to the right view
    void openSettings();
    void nextTrack();
    void prevTrack();
    void onTrackEnded();
    void onDuration(double seconds);
    void onPosition(double seconds);
    void onSeekReleased();

private:
    static QString fmt(double seconds);
    void setAudioQueue(const QStringList& files, int startIndex);
    void playTrack(int index);
    void clearAudioQueue();   // leave audio mode (video/game/doc)

    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    EbookView* book_ = nullptr;
    PdfView* pdf_ = nullptr;
    LibraryView* library_ = nullptr;
    std::unique_ptr<AddonManager> addons_;
    QListWidget* playlist_ = nullptr; // track list, shown only in audio mode
    QWidget* playerPage_ = nullptr;   // playlist + libmpv surface (stack page 0)
    QStackedWidget* stack_ = nullptr;
    QSlider* seek_ = nullptr;
    QLabel* time_ = nullptr;
    double duration_ = 0.0;
    bool sliderDown_ = false;

    QStringList tracks_;     // current audio queue (absolute paths)
    int trackIndex_ = -1;    // index into tracks_, or -1 when not playing a queue
};
