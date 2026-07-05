// Runs a libretro core and paints its frames into this widget. Keyboard -> RetroPad input.
// Software blit (QImage) - fine for 2D cores; a GL/texture path comes with hardware cores later.
#pragma once
#include <QWidget>
#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <set>
#include <deque>
#include <vector>
#include <cstdint>
#include <QHash>
#include "LibretroCore.h"   // mymediavault_libretro PUBLIC include dir (src/libretro)
#include "../input/Gamepad.h"
#include "../input/Keymap.h"

class QTimer;
class QThread;
class QAudioSink;
class QIODevice;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QScrollArea;
class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class NetplaySession;

class RetroView : public QWidget
{
    Q_OBJECT
public:
    explicit RetroView(QWidget* parent = nullptr);
    ~RetroView() override;

    // coreName is the bare core id (e.g. "mgba") used to look up the user's saved per-core options.
    bool openGame(const QString& corePath, const QString& romPath,
                  const QString& coreName = QString(), QString* error = nullptr);
    void stop();
    bool running() const { return running_; }

    // Quick save/load (F2/F4) to the current slot under <app>/states. Return false (with *error set) if the
    // core can't serialize, nothing is running, or file I/O fails.
    bool saveState(QString* error = nullptr);
    bool loadState(QString* error = nullptr);
    // Save/load a specific numbered slot (1..kStateSlots). saveState also writes a PNG thumbnail of the frame.
    bool saveState(int slot, QString* error = nullptr);
    bool loadState(int slot, QString* error = nullptr);
    static constexpr int kStateSlots = 6;

    Gamepad* gamepad() { return &pad_; }  // for the controller-remapping UI
    Keymap*  keymap()  { return &keymap_; } // for the keyboard-remapping UI

    void setPaused(bool paused);          // freeze/resume emulation (used by the Esc menu)
    void setVolume(qreal v);              // 0.0..1.0 audio level (per-pane mixing in split screen)
    void setInputActive(bool active);     // when false, ignore controller/keyboard (unfocused split pane)
    void setAchievements(class Achievements* a) { ach_ = a; } // RetroAchievements (full-screen emulator only)
    // Run emulation on a dedicated worker thread instead of the GUI timer. Used for split-screen panes so the
    // game isn't throttled by the other pane's video rendering on the shared GUI thread. Call before openGame.
    void setThreaded(bool on) { threaded_ = on; }

signals:
    void statusMessage(const QString& text); // surfaced by the main window (save/load feedback)
    void exitRequested();                    // the Esc menu's "Exit" - main window stops + returns Home
    void gameStopped();                      // a running game was torn down (main window records playtime)

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

private slots:
    void tick();          // GUI-thread frame step (non-threaded mode)
    void stepWorker();    // worker-thread frame step (threaded mode; runs on emuThread_)
    void pollInput();     // GUI-thread input poll -> snapshot (threaded mode)

private:
    // Retro post-process filters drawn over the emulator image (a cached translucent overlay).
    enum VideoFilter { FilterOff, FilterScanlines, FilterCrt, FilterLcd };
    void loadVideoFilter();               // read the persisted choice into filter_
    void cycleVideoFilter();              // Off -> Scanlines -> CRT -> LCD -> Off, persisted + repainted
    QString videoFilterLabel() const;     // "Video Filter: <name>" for the menu button
    void applyVideoFilter(QPainter& p, const QRect& dst, int srcW, int srcH); // composite the overlay over dst
    static QImage buildFilterOverlay(QSize dst, int srcW, int srcH, VideoFilter f);

    void buildMenu();          // the in-game Esc overlay (Resume / Save / Load / Exit)
    bool runOneCoreFrame();    // advance the core one frame (hw or sw), returns false if it crashed + stopped
    void captureRewind();      // snapshot the current state into the rewind ring buffer (bounded by bytes)

    // ---- netplay (2-player LAN lockstep; full-screen only) ----
    void showNetplay();                 // pause-menu sub-page: Host / Join
    void startNetplay(bool asHost, const QString& hostAddr = QString());
    void netTick();                     // the frame loop while netplay is active
    quint16 captureLocalButtons();      // this peer's RetroPad button mask (port-0 controls)
    NetplaySession* net_ = nullptr;
    bool netActive_ = false;
    unsigned netLocalPort_ = 0, netRemotePort_ = 1;
    quint32 netFrame_ = 0, netGenFrame_ = 0;
    quint16 netCurLocal_ = 0, netCurRemote_ = 0;
    QHash<quint32, quint16> netLocalInputs_;
    static constexpr int kNetDelay = 3; // input-delay frames
    void startEmu();           // begin the frame loop after a game loads (GUI timer or worker thread)
    void stopEmu();            // stop the loop / tear down the worker thread
    void publishFrame();       // copy the core's frame for the GUI to paint (threaded mode)
    int16_t resolveInput(unsigned port, unsigned device, unsigned index, unsigned id); // raw input resolve (GUI)
    void toggleMenu();
    void showMenu();
    void hideMenu();
    void showMainMenu();                // pause menu: main page (Resume / Save / Load / Exit)
    void showStateSlots(bool saveMode); // pause menu: the slot grid, in save or load mode
    void showDisk();                    // pause menu: disk control (eject / insert / switch side)
    void showCoreOptions();             // pause menu: live libretro core options (cycle each value)
    void showCheats();                  // pause menu: the per-game cheat list (toggle / add / remove)
    void addCheatDialog();              // prompt for a new cheat code + description
    QString cheatsPath() const;         // <app>/cheats/<romBaseName>.json
    void loadCheats();                  // read this game's cheats from disk
    void saveCheats();                  // persist this game's cheats
    void applyCheats();                 // push the enabled cheats into the running core
    QImage currentFrameImage();         // a copy of the frame currently on screen, for a slot thumbnail
    QString captureScreenshot();        // save the current (clean, unscaled) frame to <app>/screenshots; "" on fail
    QString statePath() const;          // <app>/states/<romBaseName>.state  (legacy single slot)
    QString statePath(int slot) const;  // <app>/states/<romBaseName>.stateN
    QString thumbPath(int slot) const;  // <app>/states/<romBaseName>.stateN.png
    QString sramPath() const;  // <app>/saves/<romBaseName>.srm  (battery-backed in-game saves)
    void loadSram();           // restore battery RAM after a game loads
    void saveSram();           // persist battery RAM (on stop, exit, and periodically)
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    void updateControllerPorts(); // enable/disable core ports 0..3 as controllers come and go
    void loadTurbo();             // read turbo/autofire config from Settings
    void startAudio(int sampleRate);
    void stopAudio();
    void pushAudio(const int16_t* data, size_t frames);

    // ---- hardware (OpenGL) rendering: a GL core renders into an offscreen FBO, which we read back into hwImg_
    // and paint through the normal software path (keeps the compositor happy — no native GL child surface). ----
    void setupHwRender();     // create the offscreen GL context + FBO, wire the core's hooks, call context_reset
    void teardownHwRender();  // context_destroy + tear down the GL objects
    void readbackHwFrame();   // glReadPixels the FBO's used region into hwImg_ (flipped to top-down)
    bool hwMode_ = false;
    QOpenGLContext* glCtx_ = nullptr;
    QOffscreenSurface* glSurface_ = nullptr;
    QOpenGLFramebufferObject* glFbo_ = nullptr;
    QImage hwImg_;            // last HW frame read back from the FBO, ready to paint

    LibretroCore core_;
    Gamepad pad_;             // physical controller (SDL2); merged with the keyboard
    Keymap keymap_;           // keyboard -> RetroPad (remappable)
    QString romPath_;         // current content, for naming its save-state slot
    QString coreName_;        // the bare core id of the running game (for the netplay handshake)
    QTimer* timer_ = nullptr;
    std::set<int> pressedKeys_; // Qt key codes currently held (resolved per-port via keymap_)
    bool running_ = false;
    bool paused_ = false;
    bool inputActive_ = true; // false = a backgrounded split pane (no controller/keyboard)

    // ---- threaded mode (split-screen panes): emulation runs on emuThread_, painted on the GUI thread ----
    bool threaded_ = false;
    QThread* emuThread_ = nullptr;   // owns emuTimer_ + the audio sink; runs stepWorker()
    QTimer* emuTimer_ = nullptr;     // frame pacer, lives on emuThread_
    QTimer* inputTimer_ = nullptr;   // GUI: poll the pad + build the input snapshot
    QMutex frameMutex_;              // guards frameImg_ (worker writes, GUI paints)
    QImage frameImg_;                // last frame handed from the worker to the GUI
    QMutex inputMutex_;              // guards the input snapshot below
    int snapBtn_[4] = { 0, 0, 0, 0 };        // per-port RetroPad button bitmask (worker reads)
    int16_t snapAxis_[4][2][2] = {};         // per-port analog [index L/R][id X/Y]
    qreal volume_ = 1.0;      // audio mix level for this instance
    class Achievements* ach_ = nullptr; // set only on the full-screen emulator
    int sramAutosaveCounter_ = 0;       // frames since the last battery-RAM autosave
    int frameIntervalMs_ = 16;
    int portsMask_ = -1;      // bitmask of player ports currently enabled on the core (-1 = unset)

    QFrame* menu_ = nullptr;        // Esc pause menu overlay
    QLabel* menuTitle_ = nullptr;   // "Paused" / "Save State" / "Load State"
    QLabel* menuStatus_ = nullptr;  // save/load feedback inside the menu
    QWidget* mainPage_ = nullptr;   // the Resume/Save/Load/Exit page
    QWidget* slotsPage_ = nullptr;  // the state-slot grid page (rebuilt each time it's shown)
    QVBoxLayout* menuBody_ = nullptr; // holds mainPage_ then slotsPage_
    bool slotsMode_ = false;        // true while the slot grid (not the main page) is showing
    int currentSlot_ = 1;           // slot F2/F4 act on; follows the last slot used in the visual menu
    QVector<QPushButton*> mainButtons_; // Resume/Save/Load/Filter/Exit (fixed, on the main page)
    QVector<QPushButton*> menuButtons_; // the current page's buttons, in order, for arrow-key + Enter navigation
    QPushButton* filterBtn_ = nullptr;  // the "Video Filter: X" cycle button on the main page
    QPushButton* diskBtn_ = nullptr;    // "Disk" entry, shown only when the core has a disk-control interface
    QPushButton* optBtn_ = nullptr;     // "Core Options" entry, shown only when the core exposes options
    QScrollArea* subScroll_ = nullptr;  // the scroll area of a scrollable sub-page (core options), for focus-follow

    struct Cheat { QString desc; QString code; bool enabled = true; }; // one per-game cheat
    QVector<Cheat> cheats_;             // this game's cheats (persisted per ROM)

    VideoFilter filter_ = FilterOff;    // active retro filter
    QImage crtOverlay_;                 // cached filter overlay (rebuilt on size/source/filter change)
    QString crtKey_;                    // cache key for crtOverlay_ (dst size + source dims + filter)
    QImage bezel_;                      // bezel/border art drawn behind the game (empty = none/disabled)
    int menuPadPrev_ = 0;               // previous frame's menu d-pad/confirm mask (edge detection)
    bool menuComboPrev_ = false;        // previous frame's Start+Select state (toggles the menu)
    int menuPadMask() const;            // bit0=Up bit1=Down bit2=confirm(A/B), held across any connected pad
    void handleMenuPad();               // drive the pause menu from the controller while it's open
    bool menuComboHeld();               // Start+Select held on any connected pad (opens/closes the menu)

    int turboMask_[4] = { 0, 0, 0, 0 }; // per port: bit set = that RetroPad button auto-fires
    int turboHalfPeriod_ = 3; // frames the autofire stays on (and off) each cycle
    int turboCounter_ = 0;
    bool turboOn_ = true;     // current autofire phase (recomputed each frame)

    // Fast-forward (hold Tab / pad Select+R2): run several core frames per tick. Rewind (hold R / pad
    // Select+L2): replay states from a bounded ring buffer captured each frame. Both are full-screen only
    // (disabled in threaded/split-pane mode, like save states).
    bool ffKey_ = false, rewindKey_ = false;   // keyboard hold state
    bool fastForward_ = false, rewinding_ = false; // resolved each frame from keyboard + pad
    std::deque<std::vector<uint8_t>> rewindBuf_;   // recent states, oldest at front
    size_t rewindBytes_ = 0;                       // total bytes held in rewindBuf_
    static constexpr size_t kRewindMaxBytes = 96u * 1024 * 1024; // ~96 MB cap (fewer seconds for big-state cores)
    static constexpr int kFfSpeed = 4;             // fast-forward multiplier

    QAudioSink* audioSink_ = nullptr;
    QIODevice* audioIo_ = nullptr; // push-mode sink input (owned by audioSink_)
    QByteArray pendingAudio_;      // interleaved S16 stereo not yet written
    int audioBytesPerSec_ = 0;
    // Resample the core's (often odd, e.g. SNES 32040 Hz) output to the device's native rate; feeding an
    // unsupported rate to QAudioSink on Windows produces static. Linear interp with carried state.
    int audioSrcRate_ = 0;         // the core's reported sample rate
    int audioOutRate_ = 0;         // the QAudioSink's rate (device native)
    double rsStep_ = 1.0;          // input frames per output frame (src/out); nudged by dynamic rate control
    double rsStepBase_ = 1.0;      // the nominal ratio (src/out); rsStep_ oscillates around this
    double rsPos_ = 0.0;           // carried fractional read position
    int16_t rsPrev_[2] = { 0, 0 }; // last input frame from the previous push (for cross-buffer interpolation)
    void resampleAppend(const int16_t* in, size_t frames); // src-rate -> out-rate, appends to pendingAudio_
};
