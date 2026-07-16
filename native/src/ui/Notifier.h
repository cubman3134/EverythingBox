#pragma once
#include <QObject>
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
    void hideNotice();
    void reposition();                               // re-anchor both overlays (resize / move)

    // The centred transient message over the player surface (visible in full screen). topOffsetPx
    // supplies the y inset below the player's top-left controls, queried at show time.
    void setPlayerHost(QWidget* player, std::function<int()> topOffsetPx);
    void playerNotice(const QString& msg, int ms = 6000);
    void hidePlayerNotice();
    bool playerNoticeVisible() const;

private:
    void positionNotice();
    QWidget* host_ = nullptr;          // the window's central area the notice floats over
    QLabel* notice_ = nullptr;         // objectName "mmvNotice"
    QTimer* noticeTimer_ = nullptr;
    QWidget* player_ = nullptr;
    std::function<int()> playerTop_;
    QLabel* playerNotice_ = nullptr;   // objectName "mmvPlayerNotice"
    QTimer* playerNoticeTimer_ = nullptr;
};
