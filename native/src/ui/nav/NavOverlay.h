// In-window overlays: the one way EB shows anything "on top" — action menus, confirmations, prompts and
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
#include <QVariantMap>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>

class NavGraph;
class NavRing;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
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

    // Themed styling (B2 Task 5): the active theme's `settingsPanel` color block (background/panel/row/
    // rowSelected/accent/text/dim/separator/…) so the Esc menu / action choosers / confirms match the theme
    // instead of hardcoded darks. MainWindow pushes it on theme apply; keys missing (or an empty map — classic
    // mode) fall back HARD to the original palette, so overlays always render. Mechanics are untouched — colors
    // only. Applies to EVERY NavOverlay subclass (NavMenu/NavConfirm) via the shared panel stylesheet.
    static void setThemeColors(const QVariantMap& colors);

    // One colour out of that block, or `fallback` when the theme does not define it (classic mode
    // pushes an empty map). The panel stylesheet's own lookup, exposed so a subclass styling a child
    // widget of its card — the confirm card's message scrollbar (#347) — matches the card it sits in.
    static QString themeColor(const char* key, const char* fallback);

    // Form-factor sizing (D1 Task 3): the panel body font sizes, pushed by MainWindow::applyFormFactorWidgets
    // (the ONE place the token math lives). Defaults are today's desktop-identity values (14px labels/buttons,
    // 16px list rows), so an overlay built before any push renders exactly as it always has. Read at
    // construction time by the shared panel stylesheet — every NavOverlay subclass picks them up.
    static void setPanelFontPx(int panelFontPx, int listFontPx);

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

    // Mirror this overlay as a level on a themed screen's NavGraph: show() (the ctor) has already run, so the
    // level is pushed here and its onPop dismisses this overlay (a no-op if it is already dismissing — the
    // guard makes the graph pop and the overlay's own Back compose without a double-dismiss). dismiss() pops
    // the level so the graph's depth tracks reality (keeping syncThemedLevels' bookkeeping clean). Focus
    // revival on close is handled uniformly by dismiss() itself (the mmvQuickRoot kick), wired or not; passing
    // nullptr (or never calling this) simply skips the level mirroring. Call once, right after construction.
    void setNavGraph(NavGraph* graph);

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
    void mousePressEvent(QMouseEvent* e) override; // tap/click on the scrim (outside the card) dismisses
    bool eventFilter(QObject* obj, QEvent* ev) override; // track the window's resizes
    void showEvent(QShowEvent* e) override;

    QFrame* panel() const { return panel_; }
    NavRing* ring() const { return ring_; }
    void relayoutPanel(); // re-fit + centre the panel (run automatically after show; call after edits)

    // Bound the panel's own content to the height it is allowed to take (issue #347). Called once per
    // relayout, AFTER the panel's width is fixed and its children have their real widths, and BEFORE
    // the panel's height is measured — the one moment at which a subclass can give each item an
    // explicit height measured at the width it is actually PAINTED at. Two things make that necessary,
    // and between them they were cutting nine shipped confirmations at every window size:
    //   * QLayout::heightForWidth measures at the widget's full width — the panel's 1px stylesheet
    //     border is invisible to it, so it wraps the text 2px wider than the label ever is; and
    //   * a label capped by maximumWidth is narrower still than the layout item it sits in, so a card
    //     made wide by its button row measured the message at 1552px and then painted it at 560px.
    // Either way the layout allocates fewer lines than the text needs and the surplus is painted
    // nowhere at all. `heightBudget` is the tallest the panel may become (relayoutPanel's own window
    // clamp). Default: do nothing, so NavMenu / the OSK / the passcode pad size exactly as they always
    // have.
    virtual void fitPanelContent(int heightBudget);

    // Arrange content whose shape depends on how wide the panel can be (issue #349). Called once per
    // relayout, BEFORE the panel's width is measured from its size hint, with the widest the panel may
    // become (relayoutPanel's own window clamp). Arranging here rather than in fitPanelContent is the
    // point: the width is then measured from what the arrangement really needs, and the arrangement is
    // decided from the content and the window alone — never from the width a previous arrangement left
    // the panel at, which would make the same card lay out differently the second time. Default: do
    // nothing, so every overlay but a confirmation sizes exactly as it always has.
    virtual void fitPanelWidth(int maxPanelWidth);

private:
    QFrame* panel_ = nullptr;
    NavRing* ring_ = nullptr;
    QPointer<QWidget> prevFocus_;   // restored on dismiss
    QPointer<NavGraph> graph_;      // themed screen's back stack this overlay mirrors as a level (null = classic)
    bool levelPushed_ = false;      // we pushed our mirror level onto graph_ (pop exactly once on dismiss)
    int result_ = -1;
    bool dismissed_ = false;
    static QVector<QPointer<NavOverlay>> s_stack;
    static QVariantMap s_themeColors;   // active theme's settingsPanel block (empty -> hardcoded fallbacks)
    static int s_panelFontPx;           // panel label/button font px (default 14 = desktop identity)
    static int s_listFontPx;            // panel list-row font px (default 16 = desktop identity)
};

// A vertical action menu (the game menu / esc menu / cast picker shape): a title and a list of rows.
// onChosen(row) runs AFTER the overlay closes; Back closes with no call (or onChosen(-1) if backIsChoice).
class NavMenu : public NavOverlay
{
    Q_OBJECT
public:
    // `initialRow` is where the highlight STARTS, clamped to the list. It defaults to the top, which is
    // every existing caller and every short fixed menu; it exists for the long ones, where "where you
    // already are" is a row somewhere in the middle and making somebody walk down to it is the whole
    // difference between a list and a jump (issue #139's chapter list is the first of those).
    NavMenu(const QString& title, const QStringList& items,
            const std::function<void(int)>& onChosen, QWidget* window = nullptr, int initialRow = 0);

    // Blocking picker (a controller-navigable QInputDialog::getItem): the chosen row, or -1 backed out.
    static int pick(const QString& title, const QStringList& items, QWidget* window = nullptr,
                    int initialRow = 0);

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
//
// EVERY BUTTON SHOWS ITS WHOLE LABEL (issue #349). The row of buttons is measured against the widest the
// card can be, and when the buttons do not all fit on one line the row WRAPS onto as many lines as it
// needs; the card is then as wide as its widest line. Nothing is squeezed and nothing is elided, because
// a button that cannot say what it does is a button nobody can press on purpose. While the row is wrapped, Left/Right walk the buttons in order
// (across the line break, which a geometric step cannot do), so every action stays reachable with a pad.
// A row that fits on one line is untouched: same layout, same widths, same keys.
//
// A MESSAGE TOO LONG FOR THE CARD SCROLLS (issue #347). The card never silently drops a sentence: it
// gives the message exactly the height its text needs, and when that is more than the window allows,
// the message area becomes a scrolling one — with its scrollbar always shown, so the card SAYS there
// is more — and Up/Down scroll it with the pad. The buttons are pinned below and stay on the card at
// every size; if a title long enough to eat the card leaves the message under two lines, the title
// scrolls with it rather than being clipped. A message that fits builds no scroll area at all, so a
// short card is the same widget tree, at the same size, that it has always been.
class NavConfirm : public NavOverlay
{
    Q_OBJECT
public:
    NavConfirm(const QString& title, const QString& message, const QStringList& buttons,
               int focusIndex = 0, QWidget* window = nullptr);

    static int ask(const QString& title, const QString& message, const QStringList& buttons,
                   int focusIndex = 0, int cancelIndex = -1, QWidget* window = nullptr);

    // Live-update the message text (used by NavCountdown to relabel the remaining-seconds line each tick). No-op
    // if the card was built with an empty message (no label was created).
    void setMessage(const QString& message);

    // True when the message does not fit and the card is scrolling it (Up/Down move the text rather
    // than the button selection). False for every card that fits, which is nearly all of them.
    bool messageScrolls() const { return scrollable_; }

    // Adds " [scroll v/max]" while the message is scrolling, so a UI-test drive can prove it read the
    // whole message rather than the first screenful.
    QString describe() const override;

    // True when the button row has been wrapped onto more than one line to keep every label whole.
    bool buttonRowWraps() const { return buttonLines_.size() > 1; }

protected:
    QLabel* message_ = nullptr; // the message label, or null when the card was built message-less

    // #347: give the title and the message the height their text needs at the width they are painted
    // at, and move the message into a scrolling viewport when that is more than the card may have.
    void fitPanelContent(int heightBudget) override;
    // #349: pack the button row against the widest the card can be, before the card's width is measured.
    void fitPanelWidth(int maxPanelWidth) override;
    // Up/Down scroll the message while it is scrolling; Left/Right walk the buttons in order while the
    // row is wrapped (#349), because a geometric step cannot cross a line break in a right-aligned row.
    bool handleNavKey(int key) override;

private:
    void ensureScrollArea();               // build the viewport the first time a message overflows
    void setTitleScrolls(bool inside);     // move the title in/out of that viewport
    void fitButtonRow(int rowWidth);       // #349: pack the buttons into lines no wider than rowWidth

    QLabel* title_ = nullptr;
    QVBoxLayout* buttonArea_ = nullptr;    // one QHBoxLayout per line of buttons (usually exactly one)
    QVector<QPushButton*> buttons_;        // in the order they were given: the row's reading order
    QStringList buttonLabels_;             // the labels AS GIVEN — measured, never re-measured
    QVector<int> buttonLines_;             // buttons per line: the packing currently laid out
    QScrollArea* scroll_ = nullptr;        // null until a message first overflows; never torn down again
    QWidget* body_ = nullptr;              // what scrolls: the message, and the title when even it must
    bool titleInBody_ = false;
    bool scrollable_ = false;
};

// A NavConfirm that counts down: the message is relabeled once a second and the card auto-accepts (dismisses
// with `acceptIndex`) when the timer reaches zero. Timers fire inside ask()'s nested event loop (the same loop
// NavConfirm blocks in), so the live relabel + auto-dismiss work in-window with no separate OS window. Channel
// mode's between-items interstitial uses it: "Next: <title> — starting in N s", {Cancel, Play now}.
class NavCountdown : public NavConfirm
{
    Q_OBJECT
public:
    // messageTmpl must contain a single `%1` placeholder for the remaining whole seconds.
    NavCountdown(const QString& title, const QString& messageTmpl, const QStringList& buttons,
                 int seconds, int acceptIndex, int focusIndex = 0, QWidget* window = nullptr);

    // Blocks in a nested loop (like NavConfirm::ask) and returns the chosen button index, `acceptIndex` on
    // timeout, or `cancelIndex` when backed out (Cancel / Back).
    static int ask(const QString& title, const QString& messageTmpl, const QStringList& buttons,
                   int seconds, int acceptIndex, int focusIndex, int cancelIndex, QWidget* window = nullptr);
};
