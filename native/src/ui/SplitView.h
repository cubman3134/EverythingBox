// Split-screen container: two MediaPanes side by side with a draggable divider. Each pane plays any media
// independently (both audio streams mix, each at its own pane volume). Exactly one pane is "focused" at a
// time and receives keyboard/controller input. Closing both panes leaves split mode.
#pragma once
#include <QWidget>

class QSplitter;
class MediaPane;

class SplitView : public QWidget
{
    Q_OBJECT
public:
    explicit SplitView(QWidget* parent = nullptr);

    MediaPane* paneA() const { return a_; }
    MediaPane* paneB() const { return b_; }
    MediaPane* focusedPane() const { return focused_; }
    void focusPane(MediaPane* p);
    bool bothEmpty() const;
    void clearAll(); // stop both panes (on leaving split mode)

signals:
    void exitRequested();                    // both panes closed -> leave split mode
    void openHereRequested(MediaPane* pane); // an empty pane wants something loaded into it

private:
    QSplitter* split_ = nullptr;
    MediaPane* a_ = nullptr;
    MediaPane* b_ = nullptr;
    MediaPane* focused_ = nullptr;
};
