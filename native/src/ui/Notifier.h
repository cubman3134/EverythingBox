#pragma once
#include <QObject>
#include <QString>
#include <functional>

class QLabel;
class QTimer;
class QWidget;

// The app's single user-visible feedback channel: a window-level notice (bottom-centre toast used for
// download/resolve progress + errors, over ANY view) and a transient centred message over the player.
// Every failure the user should hear about routes through here — no silent failures, no popup dialogs.
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QWidget* windowHost, QObject* parent = nullptr);

    void notify(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide)

    // Show a timed message over whatever is up, then put the previous message BACK if it was sticky.
    // It exists because there is exactly one label here: a phase note ("Fetching …", posted sticky because a
    // step that blocks for minutes must not blink out) and an innocent-looking three-second toast are the same
    // widget, so the toast overwrites the phase note and its own expiry then HIDES the label — turning a
    // visible three-minute wait into a blank one. Nothing at the call site hints at that, which is why the
    // repair lives here rather than in every caller that happens to fire while a phase is still running.
    // Snapshots only when the label is visible AND no auto-hide is pending — i.e. only a STICKY note is worth
    // restoring; a transient over a transient snapshots nothing and ends hidden, exactly like notify(). Opt-in
    // on purpose: several flows deliberately END on a timed error that replaces the phase note, and a blanket
    // restore would resurrect that stale phase text seconds later.
    void notifyOverSticky(const QString& text, int ms = 4500);

    void hideNotice();
    void reposition();                               // re-anchor both overlays (resize / move)

    // The centred transient message over the player surface (visible in full screen). topOffsetPx
    // supplies the y inset below the player's top-left controls, queried at show time.
    void setPlayerHost(QWidget* player, std::function<int()> topOffsetPx);
    void playerNotice(const QString& msg, int ms = 6000);
    void hidePlayerNotice();
    bool playerNoticeVisible() const;

private:
    void sizeNotice();                 // width/height for the current text: one line if it fits, else wrapped
    void positionNotice();
    QWidget* host_ = nullptr;          // the window's central area the notice floats over
    QLabel* notice_ = nullptr;         // objectName "mmvNotice"
    QTimer* noticeTimer_ = nullptr;
    QString stickyRestore_;            // the sticky note notifyOverSticky covered, put back when its timer fires
    bool restoreSticky_ = false;       // cleared by notify()/hideNotice(): a newer message owns the label now
    QWidget* player_ = nullptr;
    std::function<int()> playerTop_;
    QLabel* playerNotice_ = nullptr;   // objectName "mmvPlayerNotice"
    QTimer* playerNoticeTimer_ = nullptr;
};
