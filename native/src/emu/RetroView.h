// Runs a libretro core and paints its frames into this widget. Keyboard -> RetroPad input.
// Software blit (QImage) - fine for 2D cores; a GL/texture path comes with hardware cores later.
#pragma once
#include <QWidget>
#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <set>
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

    // Quick save/load to a per-game slot under <app>/states. Return false (with *error set) if the core
    // can't serialize, nothing is running, or file I/O fails.
    bool saveState(QString* error = nullptr);
    bool loadState(QString* error = nullptr);

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
    void buildMenu();          // the in-game Esc overlay (Resume / Save / Load / Exit)
    void startEmu();           // begin the frame loop after a game loads (GUI timer or worker thread)
    void stopEmu();            // stop the loop / tear down the worker thread
    void publishFrame();       // copy the core's frame for the GUI to paint (threaded mode)
    int16_t resolveInput(unsigned port, unsigned device, unsigned index, unsigned id); // raw input resolve (GUI)
    void toggleMenu();
    void showMenu();
    void hideMenu();
    QString statePath() const; // <app>/states/<romBaseName>.state
    QString sramPath() const;  // <app>/saves/<romBaseName>.srm  (battery-backed in-game saves)
    void loadSram();           // restore battery RAM after a game loads
    void saveSram();           // persist battery RAM (on stop, exit, and periodically)
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    void updateControllerPorts(); // enable/disable core ports 0..3 as controllers come and go
    void loadTurbo();             // read turbo/autofire config from Settings
    void startAudio(int sampleRate);
    void stopAudio();
    void pushAudio(const int16_t* data, size_t frames);

    LibretroCore core_;
    Gamepad pad_;             // physical controller (SDL2); merged with the keyboard
    Keymap keymap_;           // keyboard -> RetroPad (remappable)
    QString romPath_;         // current content, for naming its save-state slot
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
    QLabel* menuStatus_ = nullptr;  // save/load feedback inside the menu
    QVector<QPushButton*> menuButtons_; // Resume/Save/Load/Exit, in order, for arrow-key + Enter navigation
    int menuPadPrev_ = 0;               // previous frame's menu d-pad/confirm mask (edge detection)
    bool menuComboPrev_ = false;        // previous frame's Start+Select state (toggles the menu)
    int menuPadMask() const;            // bit0=Up bit1=Down bit2=confirm(A/B), held across any connected pad
    void handleMenuPad();               // drive the pause menu from the controller while it's open

    int turboMask_[4] = { 0, 0, 0, 0 }; // per port: bit set = that RetroPad button auto-fires
    int turboHalfPeriod_ = 3; // frames the autofire stays on (and off) each cycle
    int turboCounter_ = 0;
    bool turboOn_ = true;     // current autofire phase (recomputed each frame)

    QAudioSink* audioSink_ = nullptr;
    QIODevice* audioIo_ = nullptr; // push-mode sink input (owned by audioSink_)
    QByteArray pendingAudio_;      // interleaved S16 stereo not yet written
    int audioBytesPerSec_ = 0;
    // Resample the core's (often odd, e.g. SNES 32040 Hz) output to the device's native rate; feeding an
    // unsupported rate to QAudioSink on Windows produces static. Linear interp with carried state.
    int audioSrcRate_ = 0;         // the core's reported sample rate
    int audioOutRate_ = 0;         // the QAudioSink's rate (device native)
    double rsStep_ = 1.0;          // input frames per output frame (src/out)
    double rsPos_ = 0.0;           // carried fractional read position
    int16_t rsPrev_[2] = { 0, 0 }; // last input frame from the previous push (for cross-buffer interpolation)
    void resampleAppend(const int16_t* in, size_t frames); // src-rate -> out-rate, appends to pendingAudio_
};
