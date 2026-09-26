// OverflowBar + ElidedLabel — a classic reader's control bar that can NEVER force its window wider (issue #136
// follow-up).
//
// THE ROOT CAUSE this exists for. A QHBoxLayout's minimum width is the SUM of its children's minimum widths, a
// QPushButton's minimum is its whole caption, and a QLabel's minimum is its whole TEXT (a QLabel never elides).
// The comic bar holds fourteen buttons and a "<file name>  —  Pages 1–2 / 3" label, so its minimum was already
// ~1350 px on a 1280 window before #136 added "Export notes" (~1465 px after) — and a top-level window whose
// layout minimum exceeds its width is RESIZED to that minimum by Qt the moment the reader is shown. The bar
// asked for more room than the screen had, and got it.
//
// THE RULE. The bar's minimum width counts only what must always be on it: its ESSENTIAL widgets (Back, Home,
// Prev, Next, ...), the "More…" button, and a label that can shrink to an ellipsis. Everything registered as
// COLLAPSIBLE is shown only while it fits, highest priority first; whatever does not fit moves, in bar order,
// into the "More…" menu — a nav-kit NavMenu, so it is reachable by pad and keyboard exactly as the bar is. The
// label (ElidedLabel) keeps its full text in a tooltip and draws as much of it as the room allows, eliding the
// MIDDLE so both the file name's start and the page count survive.
//
// The owner keeps calling setVisible() on a collapsible control to say whether it WANTS it (the comic's mode
// buttons come and go with the reading mode); the bar tracks that wish separately from its own fit decision,
// so an owner that re-shows a control the bar had moved into "More…" does not overflow the bar again.
#pragma once
#include <QLabel>
#include <QPushButton>
#include <QVector>
#include <QWidget>

class QHBoxLayout;

// A label whose minimum width is an ellipsis, not its text. setText() SHADOWS QLabel::setText (which is not
// virtual): call it through an ElidedLabel*, never through a QLabel*.
class ElidedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    void setText(const QString& text);
    void clear() { setText(QString()); }
    QString fullText() const { return full_; }
    QSize sizeHint() const override;          // the whole text: what it would like
    QSize minimumSizeHint() const override;   // an ellipsis: what it can live with
protected:
    void resizeEvent(QResizeEvent* e) override;
private:
    void relabel();
    QString full_;
};

class OverflowBar : public QWidget
{
    Q_OBJECT
public:
    explicit OverflowBar(QWidget* parent = nullptr);

    QHBoxLayout* row() const { return row_; }
    // The "More…" button. The owner places it in the row wherever it belongs (after the controls it collects).
    QPushButton* more() const { return more_; }
    // A control that may move into "More…". Higher `priority` stays on the bar longer; equal priorities keep
    // bar order (earlier stays longer). The control must already be in row().
    void addCollapsible(QPushButton* control, int priority);

    // What is in "More…" right now, in bar order (for the menu, the UI-test snapshot and the probe).
    QVector<QPushButton*> overflowed() const;
    // Would the owner show this control, whether or not it fits? (The bar's own hiding does not change it.)
    bool wanted(QPushButton* control) const;

    QSize minimumSizeHint() const override;
    // Re-decide what fits the current width. Idempotent; called on resize, show and any child size change.
    void refit();
    // Open the "More…" menu (the button's click; public so a key binding or a probe can drive the same path).
    void openMore();

protected:
    bool event(QEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    struct Item { QPushButton* w = nullptr; int priority = 0; int order = 0; bool wanted = true; };
    int  labelReserve() const;       // the room the (elided) label keeps before a control is let back on
    int  essentialWidth() const;     // margins + spacing + every always-on widget's natural width
    QHBoxLayout* row_ = nullptr;
    QPushButton* more_ = nullptr;
    QVector<Item> items_;
    bool applying_ = false;          // the bar itself is changing visibility: not the owner's wish
    bool refitting_ = false;
};
