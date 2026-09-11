// RUNNING A VERB ON A BROWSE ROW A TURN LATER, ON A COPY OF IT (issue #365).
//
// The classic grid's "P" key, and the themed chooser's playlist row (HomeView::addBrowseItemToPlaylist), both
// open the playlist picker the same way, and the comment above the P-key arm says why at length. In short:
//
//   * QUEUED, because the picker is a nested event loop (NavMenu::pick, then Osk::getText on "New
//     playlist…"), and the press that asks for it is often still on the stack: the pad's poll slot, a live QML
//     delegate's emission, a NavMenu that has only just returned. A turn later nothing is under it.
//   * ON A COPY taken NOW, because addItemToPlaylistInteractive takes `const MediaItem&` and goes on reading it
//     after those loops. A reference into HomeView::items_ dangles the moment anything rebuilds that vector
//     during them, and a stale INDEX would file a different row without anybody noticing: this verb writes a
//     playlist entry.
//
// #365 put "Add to playlist" in the classic layout's two menus, which makes a third caller of that picker. This
// is the shape, stated once so it can be probed. The two existing callers keep their inline form (a gate in
// run-headless-probes.sh reads addBrowseItemToPlaylist's body for its QueuedConnection).
//
// `row` IS TAKEN BY VALUE, and that is the whole contract. Do not "optimise" it to `const MediaItem&`: the
// lambda would then capture whatever the caller's reference pointed at, which for a call made straight out of
// items_ is the very aliasing this file exists to remove. probe_leafroute §9 overwrites the source row after
// the call and before the turn, and fails if the verb sees the overwrite.
#pragma once
#include "../addons/AddonModels.h"   // MediaItem

#include <QMetaObject>
#include <QObject>
#include <utility>

namespace browse
{
    // Run `verb(row)` one event-loop turn from now, on the copy made when this was called. `context` owns the
    // queued call: if it is destroyed first, the verb never runs.
    template <class Verb>
    void queueOnRowCopy(QObject* context, MediaItem row, Verb verb)
    {
        QMetaObject::invokeMethod(context, [row = std::move(row), verb]() { verb(row); },
                                  Qt::QueuedConnection);
    }
}
