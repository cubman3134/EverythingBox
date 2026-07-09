// In-window overlays: the one way MMV shows anything "on top" — action menus, confirmations, prompts and
// the on-screen keyboard. An overlay is a CHILD of the main window (never a separate OS window, so no
// black flicker over the QML themed surface and no focus tug-of-war with the desktop), drawn as a dimmed
// scrim with a centred panel. Overlays stack LIFO; the topmost one owns all input (keyboard grab for
// physical keys, NavContext routing for controller keys). Back/B closes the top overlay and restores the
// selection that was live before it opened.
//
// Generalizes the HomeView game-menu pattern that proved out child-overlay rendering over the software
// QQuickWidget. Replaces: the top-level esc-menu window, QMessageBox.exec confirms, QInputDialog prompts.
#pragma once
#include <QFrame>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <functional>

class NavRing;
class QLabel;
class QLineEdit;
class QListWidget;
class QVBoxLayout;

class NavOverlay : public QWidget
{
    Q_OBJECT
public:
    // The overlay covers `window` (defaults to NavContext's main window) and shows immediately.
    explicit NavOverlay(QWidget* window = nullptr);
    ~NavOverlay() override;

    // Route a nav key to the topmost overlay. False when no overlay is open.
    static bool routeTopmost(int key);
    static NavOverlay* topmost();

    // Arrow-select a QLineEdit and press Enter: open the on-screen keyboard on it (implemented in
    // Osk.cpp). After commit the edit gets its text plus a synthetic Return, so returnPressed flows run.
    static void editLineEdit(QLineEdit* edit);
    // Same for a spinner (QSpinBox/QDoubleSpinBox/...): Enter opens the OSK on its value — arrows move the
    // selection instead of spinning it, so a row can't be changed just by walking over it.
    static void editSpinBox(class QAbstractSpinBox* spin);

    // Text-fit audit (used by the CI probe): every visible label/button/list row inside the panel must
    // fully fit its widget — nothing elided, wrapped text not cut, panel inside the window. Returns
    // human-readable offenders; empty = all good.
    QStringList clippedTexts() const;

    // Close, restoring input to whatever was focused before the overlay opened. `result` reaches the
    // closed() signal (and the sync helpers): -1 = backed out.
    void dismiss(int result = -1);

    // Human-readable "what's selected in this overlay" for the UI-test channel (menu row text, OSK
    // buffer, the focused button). Empty when there's nothing meaningful to report.
    virtual QString describe() const;

signals:
    void closed(int result);

protected:
    // A synthetic controller key (arrows / Return / Backspace / Escape). Default: geometric ring nav in
    // the panel, Return activates, Backspace/Escape dismiss. Subclasses override for custom behaviour.
    virtual bool handleNavKey(int key);

    void keyPressEvent(QKeyEvent* e) override;   // physical keys arrive here via the keyboard grab
    bool eventFilter(QObject* obj, QEvent* ev) override; // track the window's resizes
    void showEvent(QShowEvent* e) override;

    QFrame* panel() const { return panel_; }
    NavRing* ring() const { return ring_; }
    void relayoutPanel(); // re-fit + centre the panel (run automatically after show; call after edits)

private:
    QFrame* panel_ = nullptr;
    NavRing* ring_ = nullptr;
    QPointer<QWidget> prevFocus_;   // restored on dismiss
    int result_ = -1;
    bool dismissed_ = false;
    static QVector<QPointer<NavOverlay>> s_stack;
};

// A vertical action menu (the game menu / esc menu / cast picker shape): a title and a list of rows.
// onChosen(row) runs AFTER the overlay closes; Back closes with no call (or onChosen(-1) if backIsChoice).
class NavMenu : public NavOverlay
{
    Q_OBJECT
public:
    NavMenu(const QString& title, const QStringList& items,
            const std::function<void(int)>& onChosen, QWidget* window = nullptr);

    // Blocking picker (a controller-navigable QInputDialog::getItem): the chosen row, or -1 backed out.
    static int pick(const QString& title, const QStringList& items, QWidget* window = nullptr);

    QString describe() const override; // the highlighted row's text

protected:
    bool handleNavKey(int key) override;

private:
    QListWidget* list_ = nullptr;
    std::function<void(int)> onChosen_;
};

// A confirmation card: title + message + a row of buttons. `ask` blocks in a nested event loop and
// returns the chosen button index, or `cancelIndex` when backed out — a drop-in for QMessageBox::exec
// that stays in-window and controller-navigable.
class NavConfirm : public NavOverlay
{
    Q_OBJECT
public:
    NavConfirm(const QString& title, const QString& message, const QStringList& buttons,
               int focusIndex = 0, QWidget* window = nullptr);

    static int ask(const QString& title, const QString& message, const QStringList& buttons,
                   int focusIndex = 0, int cancelIndex = -1, QWidget* window = nullptr);
};
