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

    // Point this view at the app's ONE shared Gamepad (RetroView owns it; MainWindow passes retro_->gamepad()).
    // RetroParkView must NOT own a second Gamepad: SDL has a single global event queue, and Gamepad::poll() drains
    // it — including the SDL_CONTROLLERDEVICEADDED hot-plug event, which only ONE instance can consume. A second
    // instance therefore misses the ADDED event for a controller plugged in AFTER launch and never opens it, so no
    // pad input ever reaches the in-process Dolphin. Sharing the front-end's instance (which already opened the
    // controller) fixes that; while RetroPark runs, RetroView is stopped and MainWindow's menu-nav poll is
    // suppressed, so this view is the sole poller. Null until set (feedInput guards it).
    void setGamepad(Gamepad* shared) { sharedPad_ = shared; }

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
    // Self-heal a libretro-shim directory at the moment of use (idempotent + non-destructive) and return its path
    // (empty on failure, *err set). Ensures <coresDir>/<subdir> holds LibretroShim.dll + core.json (naming
    // coreDllName as the libretro core) + a fresh copy of EB's own <ebCoreId> DLL as <coreDllName>. Never
    // overwrites an existing core.json (protects the build-staged NES manifest). Shared by the NES (fceumm) and
    // N64 (mupen64plus_next) driven-shim load paths in openGame().
    QString ensureShimDir(const QString& subdir, const QString& ebCoreId,
                          const QString& coreDllName, QString* err);
    void clearHeldKeys();   // release every held NES key (pause / focus-out / stop)
    bool saveState(QString* error);   // serialize the running core to the RetroPark-namespaced state file
    bool loadState(QString* error);   // restore the running core from that file (if present)
    QString statePath() const;        // <dataDir>/states/retropark/<romBase>.rpstate — distinct from libretro's
    void buildMenu();       // the in-game pause overlay (Resume / Save / Load / Exit) — a QFrame child, like RetroView's
    void showMenu();        // pause (rp_runtime_pause) and raise the overlay — timer_ KEEPS running so tick() can drive the menu
    void hideMenu();        // resume (rp_runtime_resume) and hide the overlay
    void toggleMenu();
    void handleMenuPad();   // while the menu is up: poll sharedPad_ and drive the selection (Up/Down move, A confirm, B back)
    int  menuPadMask() const; // held state of the menu-relevant pad buttons (bit1=Up bit2=Down bit4=A bit8=B), port 0

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
    Gamepad* sharedPad_ = nullptr;      // the app's ONE physical controller (owned by RetroView), set via setGamepad();
                                        // polled + read each tick. NOT owned here — see setGamepad() for why a second
                                        // Gamepad instance would miss the SDL hot-plug event and read no input.
    bool     exitComboHeld_ = false;    // debounce state for the Start+Select pause-menu combo (rising-edge in feedInput);
                                        // deliberately NOT cleared by clearHeldKeys() so a held combo can't re-open the
                                        // menu the instant the user resumes — only a release-then-press re-fires.

    // Identity carried from the launcher. romPath_ is the loaded ROM (real-content path only) — its base name keys
    // the RetroPark-namespaced state file; the rest are kept for parity.
    QString title_, systemId_, gameKey_, romPath_;

    // Core-option identity (Task B4), the RetroPark twin of RetroView's coreName_/overrideToken_. coreName_ is the
    // libretro core name that namespaces the persisted option keyspace (opt/<core>/* + the optdesc/<core> cache);
    // resolved from Settings::coreFor(systemId_) with a SystemCatalog default-core fallback. overrideToken_ is the
    // per-game token (Settings::gameToken) keying the per-game option delta. Both set in openGame(); after a
    // driven core's content loads, the persisted effective values are pushed into the running runtime.
    QString coreName_, overrideToken_;

    // Pause overlay (a styled QFrame child, mirroring RetroView::buildMenu — NOT a QDialog/QMessageBox).
    QFrame*      menu_ = nullptr;
    QLabel*      menuTitle_ = nullptr;   // "Paused" — also shows inline save/load feedback while the menu is up
    QPushButton* resumeBtn_ = nullptr;
    QPushButton* saveBtn_ = nullptr;
    QPushButton* loadBtn_ = nullptr;
    QPushButton* exitBtn_ = nullptr;
    std::vector<QPushButton*> menuButtons_;   // focus-cycle order for Up/Down navigation
    int          menuPadPrev_ = 0;   // previous-frame menuPadMask(), for rising-edge menu-pad nav (see handleMenuPad)
};
