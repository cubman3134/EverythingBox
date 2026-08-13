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
#include <array>
#include <cstdint>
#include <vector>

#include "../input/Gamepad.h"   // physical controller (SDL2), reused verbatim from the libretro path; a no-op
                                // when SDL isn't compiled in, so the header needs no #ifdef.

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

    // Start a RetroPark-backend game. With a real ROM (romPath non-empty) this loads the DYNAMIC libretro shim
    // (FCEUmm / NES) from <coresDir>/libretro_shim and hands it the ROM; with no ROM it falls back to the static
    // driven reference core (the Slice-2a animated test pattern). The full identity set (title/systemId/gameKey)
    // is carried through for parity with RetroView. On failure *error is set (if non-null) and the view stays torn
    // down; the caller shows the message and does not switch to this page. coreOrId is the resolved libretro core
    // id (carried for identity; the driven shim path selects FCEUmm itself and does not consult it in 2b).
    //
    // presenting (Slice 3a): does the target run on a PRESENTING core (the heavy-app / Dolphin path, which renders
    // on the GPU itself and demands a headless VULKAN runtime) rather than a DRIVEN core (the refcore / shim, on
    // the proven headless D3D11 runtime)? The runtime's graphics API must be chosen at rp_runtime_create — BEFORE
    // load_core — so it cannot be read off the core; the caller states it here and openGame() maps it via
    // rpapi::runtimeApiForCore. No presenting GAME ships in 3a, so the default (false) keeps every current caller
    // byte-behaviourally on the D3D11 driven path.
    void openGame(const QString& coreOrId, const QString& romPath, const QString& title,
                  const QString& systemId, const QString& gameKey, QString* error = nullptr,
                  bool presenting = false);
    void stop();                              // tear down the runtime + timer; safe when not running (idempotent)
    bool running() const { return running_; }

signals:
    void exitRequested();   // the pause menu's "Exit" — the host stops this view + returns Home (RetroView parity)
    void gameStopped();     // a running game was torn down — emitted by stop() (RetroView parity)
    void statusMessage(const QString& text);  // save/load/rewind feedback — MainWindow shows it in the status bar

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;   // clears held NES keys (paired with keyPressEvent)
    void focusOutEvent(QFocusEvent*) override;    // drop all held keys so none stick when focus leaves

private:
    void tick();            // QTimer slot: rewind or feed-input+present into buf_, then update()
    void scheduleNextFrame();  // re-arm the single-shot timer at the core's real (fractional) frame period
    void feedInput();       // build port-0 rp_input_state from held keys + the pad, push via rp_runtime_set_input
    void clearHeldKeys();   // release every held NES key (pause / focus-out / stop)
    bool saveState(QString* error);   // serialize the running core to the RetroPark-namespaced state file
    bool loadState(QString* error);   // restore the running core from that file (if present)
    QString statePath() const;        // <dataDir>/states/retropark/<romBase>.rpstate — distinct from libretro's
    void buildMenu();       // the in-game pause overlay (Resume / Save / Load / Exit) — a QFrame child, like RetroView's
    void showMenu();        // pause (rp_runtime_pause + stop the timer) and raise the overlay
    void hideMenu();        // resume (rp_runtime_resume + start the timer) and hide the overlay
    void toggleMenu();

    rp_runtime* rt_ = nullptr;          // the RetroPark runtime handle (null unless a game is running)
    QTimer*     timer_ = nullptr;       // single-shot frame pacer (PreciseTimer); re-armed each tick via scheduleNextFrame()
    // Frame pacing (see RetroParkPace.h). frameIntervalMs_ is the loaded core's TRUE frame period in ms
    // (1000/fps); frameAccumMs_ carries the sub-ms remainder so the long-run average interval matches it.
    double      frameIntervalMs_ = 1000.0 / 60.0;   // reset per load (NES rate for real content, ~60 for the refcore)
    double      frameAccumMs_ = 0.0;
    std::vector<uint8_t> buf_;          // reused RGBA8 read-back target (rpW_*rpH_*4), wrapped as a QImage to paint
    uint32_t    rpW_ = 0, rpH_ = 0;     // the runtime's render geometry (buf_ + the QImage stride derive from it)
    bool        running_ = false;
    bool        realContent_ = false;   // a real ROM (dynamic shim) is loaded — only then is input fed to the core
    bool        presenting_ = false;    // the PRESENTING (Dolphin/GC) path — Vulkan runtime + heavy-app core (Slice 3b);
                                        // gates rewind off (savestates are ~94 MB) and, later, the abstract-pad input
    bool        rewindEnabled_ = false; // rp_runtime_set_rewind succeeded (serialize-capable core) — gates rewind
    bool        rewinding_ = false;     // the rewind key (R) is held — tick() steps back instead of advancing

    // Live input state. keyHeld_ is indexed by Win32 virtual-key code (the shim reads rp_input_state.keys[VK]);
    // key press/release events set/clear the NES-relevant entries, and each tick() ORs them with the physical pad.
    std::array<bool, 256> keyHeld_{};   // value-initialised: all false
    // Presenting (GameCube/Dolphin) keyboard input state — the abstract-pad analog of keyHeld_ above (Slice 3b).
    // The GC path reads the ABSTRACT PAD (pad_buttons/pad_axes), not keys[], and its keys are Qt key codes (not
    // indexable Win32 VKs), so held state is tracked as a pad-bit mask + the four arrow directions (which drive
    // the analog LEFT stick). Cleared alongside keyHeld_ in clearHeldKeys().
    uint16_t            padKeyButtons_ = 0;   // RP_PAD_* bits currently held from the keyboard
    std::array<bool, 4> padKeyArrows_{};      // [0]=Up [1]=Down [2]=Left [3]=Right -> analog left stick
    Gamepad pad_;                       // physical controller, polled each tick (single-player / port 0 in 2b)

    // Identity carried from the launcher. romPath_ is the loaded ROM (real-content path only) — its base name keys
    // the RetroPark-namespaced state file; the rest are kept for parity.
    QString title_, systemId_, gameKey_, romPath_;

    // Pause overlay (a styled QFrame child, mirroring RetroView::buildMenu — NOT a QDialog/QMessageBox).
    QFrame*      menu_ = nullptr;
    QLabel*      menuTitle_ = nullptr;   // "Paused" — also shows inline save/load feedback while the menu is up
    QPushButton* resumeBtn_ = nullptr;
    QPushButton* saveBtn_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* exitBtn_ = nullptr;
    std::vector<QPushButton*> menuButtons_;   // focus-cycle order for Up/Down navigation
};
