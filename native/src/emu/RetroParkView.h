// The RetroPark play surface (Slice 2a). A sibling of RetroView — never a subclass, never an edit to it — that
// drives the RetroPark runtime's DRIVEN-core path: create a headless D3D11 runtime, load the static driven
// reference core, and pump rp_runtime_present into a QImage on a QTimer, painting the aspect-fit frame into
// this widget. It exposes the SAME openGame/stop shape and the SAME exitRequested/gameStopped signals RetroView
// does, so MainWindow reuses its page show/hide contract verbatim (a RetroPark game is just another content
// page). No shared textures / presenting cores (Slice 3), no input / audio / shaders (later slices): 2a is the
// live driven surface with pause / resume / exit, and nothing more.
//
// This class always COMPILES — the retropark runtime calls live behind EB_HAVE_RETROPARK (defined for the app
// only on desktop/Windows, where RetroPark's static lib is linked). Without it, openGame() fails gracefully with
// an error and the widget is an inert black rectangle, so the app still builds on platforms that have no
// RetroPark (Android/iOS keep the Libretro backend, which is the only reachable one there anyway).
#pragma once
#include <QWidget>
#include <QImage>
#include <QString>
#include <cstdint>
#include <vector>

class QTimer;
class QFrame;
class QLabel;
class QPushButton;

// Opaque forward decl of the runtime handle, so this header never has to pull in <retropark/retropark.h> (whose
// include dir is only on the app target's path under the desktop guard). The .cpp includes the real header.
struct rp_runtime;

class RetroParkView : public QWidget
{
    Q_OBJECT
public:
    explicit RetroParkView(QWidget* parent = nullptr);
    ~RetroParkView() override;

    // Start a RetroPark-backend game. In Slice 2a the ROM is IGNORED — the driven reference core (an animated
    // test pattern) is what loads — but the full signature is carried so the launcher passes the same identity
    // set it hands RetroView (title/systemId/gameKey), ready for a content-loading core in a later slice.
    // On failure *error is set (if non-null) and the view stays torn down; the caller shows the message and does
    // not switch to this page. coreOrId is the resolved core id (unused by the driven path in 2a).
    void openGame(const QString& coreOrId, const QString& romPath, const QString& title,
                  const QString& systemId, const QString& gameKey, QString* error = nullptr);
    void stop();                              // tear down the runtime + timer; safe when not running (idempotent)
    bool running() const { return running_; }

signals:
    void exitRequested();   // the pause menu's "Exit" — the host stops this view + returns Home (RetroView parity)
    void gameStopped();     // a running game was torn down — emitted by stop() (RetroView parity)

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void tick();            // QTimer slot: rp_runtime_present into buf_, then update()
    void buildMenu();       // the in-game pause overlay (Resume / Exit) — a QFrame child, like RetroView's
    void showMenu();        // pause (rp_runtime_pause + stop the timer) and raise the overlay
    void hideMenu();        // resume (rp_runtime_resume + start the timer) and hide the overlay
    void toggleMenu();

    rp_runtime* rt_ = nullptr;          // the RetroPark runtime handle (null unless a game is running)
    QTimer*     timer_ = nullptr;       // frame pacer; drives tick() at the driven core's ~60 fps
    std::vector<uint8_t> buf_;          // reused RGBA8 read-back target (rpW_*rpH_*4), wrapped as a QImage to paint
    uint32_t    rpW_ = 0, rpH_ = 0;     // the runtime's render geometry (buf_ + the QImage stride derive from it)
    bool        running_ = false;

    // Identity carried from the launcher (unused by the driven path in 2a; kept for parity + a later content core).
    QString title_, systemId_, gameKey_;

    // Pause overlay (a styled QFrame child, mirroring RetroView::buildMenu — NOT a QDialog/QMessageBox).
    QFrame*      menu_ = nullptr;
    QPushButton* resumeBtn_ = nullptr;
    QPushButton* exitBtn_ = nullptr;
};
