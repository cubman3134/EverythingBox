// Runs a libretro core and paints its frames into this widget. Keyboard -> RetroPad input.
// Software blit (QImage) - fine for 2D cores; a GL/texture path comes with hardware cores later.
#pragma once
#include <QWidget>
#include <QByteArray>
#include <set>
#include "LibretroCore.h"   // goliath_libretro PUBLIC include dir (src/libretro)
#include "../input/Gamepad.h"
#include "../input/Keymap.h"

class QTimer;
class QAudioSink;
class QIODevice;

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

signals:
    void statusMessage(const QString& text); // surfaced by the main window (save/load feedback)

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;

private slots:
    void tick();

private:
    QString statePath() const; // <app>/states/<romBaseName>.state
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
    int portsMask_ = -1;      // bitmask of player ports currently enabled on the core (-1 = unset)

    int turboMask_[4] = { 0, 0, 0, 0 }; // per port: bit set = that RetroPad button auto-fires
    int turboHalfPeriod_ = 3; // frames the autofire stays on (and off) each cycle
    int turboCounter_ = 0;
    bool turboOn_ = true;     // current autofire phase (recomputed each frame)

    QAudioSink* audioSink_ = nullptr;
    QIODevice* audioIo_ = nullptr; // push-mode sink input (owned by audioSink_)
    QByteArray pendingAudio_;      // interleaved S16 stereo not yet written
    int audioBytesPerSec_ = 0;
};
