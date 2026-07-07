// The app-wide controller/arrow-key navigation kit. One selection engine + one overlay system, so every
// screen behaves the same way out of the box:
//   - NavRing:    a container's focusable widgets become one geometric "ring". Arrow keys move the Qt focus
//                 between them by position (works for vertical lists, button rows, grids, two-column panels
//                 alike), clamped at the edges (no wrap). The ring guarantees a selection: if the focused
//                 widget is deleted/hidden, the nearest survivor is focused instead — the selector can never
//                 be "lost". Text inputs (QLineEdit) become selectable rows: arrows move OVER them; Enter
//                 opens the on-screen keyboard (physical typing still works when one is focused directly).
//   - NavContext: the routing kernel. sendNavKey() hands every synthetic key here first; overlays get it
//                 before the page behind them, and an unconsumed Back triggers the current screen's
//                 registered back action, so B/Backspace works on every screen without per-screen wiring.
//                 Also runs the focus watchdog (the "guaranteed selection" half of the contract).
// See NavOverlay.h for the in-window overlay system (menus/confirms/OSK) that replaces popup windows.
#pragma once
#include <QObject>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <functional>

class QLineEdit;

// One navigable surface: the focusable widgets inside `container`, stepped with arrow keys by geometry.
// Create it once per screen/panel/overlay; it re-collects lazily so dynamically built content just works.
class NavRing : public QObject
{
    Q_OBJECT
public:
    explicit NavRing(QWidget* container, QObject* parent = nullptr);

    // Handle a nav key (arrows / Return / Backspace). Returns true when consumed. Arrows move focus
    // geometrically; Return activates the focused widget (buttons click; line edits open the OSK);
    // Backspace is NOT consumed here (it belongs to the screen's back action) unless a line edit is
    // being edited through the OSK.
    bool handleKey(int key);

    // The widgets of the ring, re-collected fresh: visible, enabled, focus-accepting descendants of the
    // container, in geometry order (top-to-bottom, then left-to-right).
    QVector<QWidget*> widgets() const;

    // Make sure SOMETHING in the ring has focus. If the focused widget died or is outside the ring, focus
    // the remembered selection, else the widget nearest the last selection's position, else the first.
    // Safe to call any time; no-op when focus is already valid. Returns the focused widget (or null when
    // the ring is empty).
    QWidget* ensureSelection();

    // Remember/restore the selection across a screen swap (Back returns you to the row you left).
    // The key is the widget's objectName when set, else its text/geometry signature.
    QString rememberSelection() const;
    void restoreSelection(const QString& key);

    // Step focus in a direction (Qt::Key_Up/Down/Left/Right). Returns false at an edge (not consumed, so
    // the screen can e.g. move focus to another zone).
    bool step(int key);

    QWidget* container() const { return container_; }

    // Geometric pick: from `from`'s centre, the best widget among `candidates` in direction `key`.
    // Exposed for reuse (overlays, tests).
    static QWidget* pickNext(QWidget* from, const QVector<QWidget*>& candidates, int key);

private:
    void activate(QWidget* w);          // Return pressed on w
    QPointer<QWidget> container_;
    mutable QPointer<QWidget> lastFocus_;  // last ring widget that had focus (for ensureSelection)
    mutable QPoint lastCenter_;            // its centre, so recovery lands near where the user was
};

// The routing kernel + focus watchdog. One instance, owned by MainWindow.
class NavContext : public QObject
{
    Q_OBJECT
public:
    static NavContext* instance();
    explicit NavContext(QWidget* mainWindow);
    ~NavContext() override;

    QWidget* window() const { return window_; }

    // Route a synthetic nav key. Order: topmost overlay -> the active screen's ring (if registered) ->
    // not consumed (caller falls through to its legacy delivery). Returns true when consumed.
    bool routeKey(int key);

    // The current screen's back action, run when a Backspace/Escape reaches the bottom of the routing
    // chain unconsumed. Screens register on show; empty = "no back here" (e.g. the home root).
    void setBackAction(const std::function<void()>& back) { backAction_ = back; }
    bool runBackAction();

    // The active screen's ring (the panel/page currently on top). Registering also arms the focus
    // watchdog for it. Pass null when a screen manages its own focus (QML themed views, readers).
    void setActiveRing(NavRing* ring);
    NavRing* activeRing() const { return activeRing_; }

    // True while a synthetic (controller-origin) key is being delivered — lets widgets tell a pad "Back"
    // press (navigate) apart from a physical Backspace (delete a character in a line edit).
    static bool syntheticKey() { return syntheticKey_; }
    struct SyntheticScope
    {
        SyntheticScope()  { NavContext::syntheticKey_ = true; }
        ~SyntheticScope() { NavContext::syntheticKey_ = false; }
    };

    // Focus watchdog: if the app's focus widget is gone/hidden/disabled while we're on a ring-managed
    // screen (and no text entry is live), restore a valid selection. Called on a timer + after routing.
    void ensureFocus();

private:
    static NavContext* s_instance;
    static bool syntheticKey_;
    QPointer<QWidget> window_;
    QPointer<NavRing> activeRing_;
    std::function<void()> backAction_;
};
