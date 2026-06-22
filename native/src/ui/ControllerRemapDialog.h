#pragma once
#include <QDialog>
#include <QHash>

class Gamepad;
class Keymap;
class QPushButton;
class QCheckBox;
class QLabel;
class QComboBox;
class QTimer;

// Remap each RetroPad button to a physical controller input and/or a keyboard key. The controller column
// has a separate profile per player port (1-4), selected by a combo; the keyboard column is player 1 only.
// Clicking a cell enters capture mode (press a button / press a key); the next input becomes the binding.
// Bindings persist (via Settings) and apply on Save.
class ControllerRemapDialog : public QDialog
{
    Q_OBJECT
public:
    static constexpr int kPlayers = 4;

    ControllerRemapDialog(Gamepad* pad, Keymap* keys, QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent*) override; // captures keys; Esc cancels an in-progress capture

private slots:
    void onTick();        // poll the controller; drive pad capture + live status
    void onPlayerChanged(int index);
    void save();
    void resetDefaults();

private:
    void beginPadCapture(int retroId);
    void beginKeyCapture(int retroId);
    void cancelCapture();
    void refreshRow(int retroId);

    Gamepad* pad_ = nullptr;
    Keymap*  keys_ = nullptr;
    QComboBox* playerCombo_ = nullptr;
    QComboBox* turboSpeed_ = nullptr;
    QTimer* timer_ = nullptr;
    QLabel* status_ = nullptr;
    QHash<int, QPushButton*> padButtons_; // retroId -> controller-binding button
    QHash<int, QPushButton*> keyButtons_; // retroId -> keyboard-binding button
    QHash<int, QCheckBox*>  turboChecks_; // retroId -> turbo/autofire checkbox
    int port_ = 0;                        // player port whose profile is being edited
    int workingPad_[kPlayers][16];        // [port][retroId] -> controller binding code being edited
    int workingKey_[kPlayers][16];        // [port][retroId] -> Qt key code being edited
    bool workingTurbo_[kPlayers][16];     // [port][retroId] -> autofire flag being edited
    int capturingPad_ = -1;               // retroId capturing a controller input, or -1
    int capturingKey_ = -1;               // retroId capturing a key, or -1 (keyboard is grabbed while >= 0)
    bool sawRelease_ = false;             // require controls released before binding a fresh pad press
};
