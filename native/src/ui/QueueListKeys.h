#pragma once
// The classic player's queue list owns its CONFIRM key (issue #415).
//
// A QListWidget handles Return/Enter by emitting activated() and then IGNORING the event
// (QAbstractItemView::keyPressEvent). Qt then hands the same key to the parent chain. On the player page that
// parent is MainWindow::keyPressEvent, and its Return/Enter/Select branch treats "no BUTTON focused" as
// "nothing focused, toggle play/pause". So one press on a queue row did two things: itemActivated reloaded
// that row, and togglePause() then paused it. The row played at 0:00 and never moved, and the same press on
// the playing row reloaded it at its resume point and stopped there. The pad and remote route
// (MainWindow::sendNavKey, QCoreApplication::sendEvent on the focused widget) propagates the same way. A mouse
// double-click was never affected, because it is not a key event.
//
// This is the whole fix: the list's event filter claims the confirm key itself. It activates the current row
// through the SAME itemActivated signal (so the one existing handler, with its plRowToTrack_ header-row map,
// stays the only place a row becomes a track) and accepts the key, so nothing above the list sees it. With no
// activatable row there is nothing for the key to choose, and it is left alone. Key_Select is included for
// the reason PlayerBarNav gives: a remote or a pad can arrive as Key_Select while a keyboard sends Return or
// Enter.
//
// Header-only and QtWidgets-only, so probe_queuekeys drives the real Qt key propagation around it.
#include <QEvent>
#include <QKeyEvent>
#include <QListWidget>

namespace eb {

inline bool isQueueConfirmKey(int key)
{
    return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Select;
}

// Call from the queue list's event filter. Returns true when the key was claimed (the filter must then return
// true); false leaves the event exactly as it was.
inline bool claimQueueConfirm(QListWidget* list, QEvent* event)
{
    if (!list || !event || event->type() != QEvent::KeyPress) return false;
    auto* ke = static_cast<QKeyEvent*>(event);
    // A modified Enter (Alt+Enter and the like) is not "choose this row"; leave it to whoever binds it.
    if ((ke->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) return false;
    if (!isQueueConfirmKey(ke->key())) return false;
    QListWidgetItem* item = list->currentItem();
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) return false;
    emit list->itemActivated(item);
    ke->accept();
    return true;
}

} // namespace eb
