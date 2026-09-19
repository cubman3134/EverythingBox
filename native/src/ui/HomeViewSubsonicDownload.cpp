// #193 — WHICH BROWSE ROWS OFFER "DOWNLOAD" FOR A SUBSONIC SERVER, on both layouts.
//
// One question, asked by every door: the themed chooser (downloadOfferedFor -> downloadBrowseItem), the classic
// track menus (trackMenuForRow -> downloadBrowseItem) and the Start menu on either layout
// (browseSubsonicDownload). It is answered through browse::queueTargetFor — the ONE reading of what a music row
// names — and SubsonicDownload::targetFor, so a row that queues as a Subsonic track downloads as that track and
// a row that queues as a Subsonic album (or playlist) downloads as that record, and nothing else ever does.
#include "HomeView.h"

#include "../browse/LeafRoute.h"
#include "../core/SubsonicDownload.h"

#include <QListWidget>

bool HomeView::subsonicDownloadTargetOf(const MediaItem& it, int* kindOut, QString* refOut) const
{
    // Already saved: a Recent or a Downloaded row plays its file; a second download could only toast.
    if (atRecentsLevel() || atDownloadsLevel()) return false;
    const browse::QueueTarget q = browse::queueTargetFor(it);
    if (!q.ok()) return false;
    const SubsonicDownload::Target t = SubsonicDownload::targetFor(q.what == browse::QueueAdd::Track,
                                                                   q.what == browse::QueueAdd::Album,
                                                                   q.albumKey, q.trackPath);
    if (!t.ok()) return false;
    if (kindOut) *kindOut = int(t.kind);
    if (refOut)  *refOut = t.ref;
    return true;
}

bool HomeView::browseSubsonicDownload(int themedIndex, int* kindOut, QString* refOut, QString* titleOut,
                                      QString* thumbOut) const
{
    int row = -1;
    if (themedIndex < 0) { if (!grid_) return false; row = grid_->currentRow(); }
    else                 { if (themedIndex >= browseRowMap_.size()) return false; row = browseRowMap_[themedIndex]; }
    return subsonicDownloadForItemsRow(row, kindOut, refOut, titleOut, thumbOut);
}

bool HomeView::subsonicDownloadForItemsRow(int row, int* kindOut, QString* refOut, QString* titleOut,
                                           QString* thumbOut) const
{
    if (row < 0 || row >= items_.size()) return false;
    if (!subsonicDownloadTargetOf(items_[row], kindOut, refOut)) return false;
    if (titleOut) *titleOut = items_[row].title;
    if (thumbOut) *thumbOut = items_[row].thumbnailUrl;
    return true;
}
