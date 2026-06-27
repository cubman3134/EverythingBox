// One half of the split screen. Hosts its OWN media engines - video/audio (libmpv), emulator (libretro),
// and the document readers - so each pane plays any kind of media independently of the other pane and of
// the app's normal full-screen views. A thin top bar shows the title, a play/pause + volume control, and a
// close button; clicking anywhere in the pane focuses it (the focused pane receives keyboard/controller).
#pragma once
#include <QWidget>

class QStackedWidget;
class QLabel;
class QSlider;
class QPushButton;
class MpvWidget;
class RetroView;
class EbookView;
class PdfView;
class ComicView;

class MediaPane : public QWidget
{
    Q_OBJECT
public:
    explicit MediaPane(QWidget* parent = nullptr);

    void openVideo(const QString& url, const QString& title); // also used for audio (mpv plays audio-only)
    void openGame(const QString& corePath, const QString& romPath, const QString& coreName);
    void openBook(const QString& path);
    void openPdf(const QString& path);
    void openComic(const QString& path);
    void clear();                       // stop everything, show the empty "＋ Open here" placeholder

    bool isEmpty() const { return kind_ == None; }
    void setFocused(bool on);           // visual highlight + route input to this pane's game

    RetroView* retro() const { return retro_; }

signals:
    void focusRequested();              // user interacted with this pane
    void openHereRequested();           // user clicked the empty pane's "＋ Open here"
    void closeRequested();              // user clicked ✕ (or a reader's Back/Home)

protected:
    bool eventFilter(QObject*, QEvent*) override; // click anywhere -> focusRequested

private:
    void showView(QWidget* w, const QString& title, bool hasAudio);
    void applyVolume();
    void togglePause();

    QStackedWidget* stack_ = nullptr;
    QWidget* emptyPage_ = nullptr;      // placeholder with "＋ Open here"
    MpvWidget* player_ = nullptr;
    RetroView* retro_ = nullptr;
    EbookView* book_ = nullptr;
    PdfView* pdf_ = nullptr;
    ComicView* comic_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QSlider* volume_ = nullptr;
    QPushButton* closeBtn_ = nullptr;

    bool focused_ = false;
    bool paused_ = false;
    int volPct_ = 100;
    enum Kind { None, Video, Game, Document } kind_ = None;
};
