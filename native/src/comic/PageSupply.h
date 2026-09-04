// WHAT A PAGE SUPPLIER MAY ASK OF THE ONE PAGE SEAM.
//
// There is exactly one way a remote comic reaches the reader: MainWindow::openImagePages takes a list of
// page images, fetches them, packs them into a CBZ cached under the item's id, and opens that in
// ComicView — so prefetch, the disk cache, zoom, two-up, bookmarks and resume all come along unchanged.
// #188 made that list a REMOTE one (an addon's `pages` resource); #153 adds a second supplier of the same
// list (an OPDS-PSE server's page template). Three suppliers, one seam.
//
// The suppliers do differ in three small ways, and this struct is those three, kept off the signature so
// a fourth supplier does not mean a fifth parameter. Every field's default is EXACTLY what the seam did
// before this struct existed, so a caller that passes nothing is byte-for-byte the old behaviour.
//
// Pure: no widgets, no network, no disk. Held by value.
#pragma once
#include <QVector>

#include <functional>

struct PageSupplyOptions
{
    // THE ORDER THE PAGES ARE SUBMITTED IN — not which pages, and not the order they are packed in.
    // Empty (or the wrong length) means plain reading order, which is what the seam always did.
    //
    // Why an order is a prefetch: every page is fetched through one QNetworkAccessManager, which runs a
    // handful of requests per host at a time and queues the rest IN SUBMISSION ORDER. Submitting the page
    // the reader will actually land on first — then the two after it — is the difference between that
    // page arriving in the first round trip and arriving after two hundred others.
    QVector<int> fetchOrder;

    // REFUSE A VOLUME WITH HOLES IN IT. The seam's default is forgiving: a page that failed to download
    // is skipped and the chapter opens without it, because a manga scanlation with one dead image is
    // still worth reading. A STREAMED volume is different — its packed CBZ is also its offline copy, so
    // a partial one would be cached and then served from cache for ever, and the missing page would
    // never come back. When this is set, a short volume is thrown away instead of cached, and
    // `onIncomplete` is told.
    bool requireAllPages = false;

    // LAND HERE, 0-based, once the reader is open; -1 leaves the reader wherever it would have landed.
    // It exists because for a server-owned volume the SERVER owns the reading position (#153 / the #83
    // rule), and that answer arrives with the feed rather than from this device's resume store.
    int startPage0 = -1;

    // WHO SAYS WHAT A FAILED FETCH MEANS. Unset (the default) leaves the seam's own toast in place. Set,
    // it replaces that toast and is handed (pages that arrived, pages expected) so the supplier can offer
    // something better — #153 offers a retry and "download the volume instead", because a stalled page
    // stream has a perfectly good escape hatch and a toast does not mention it.
    std::function<void(int added, int expected)> onIncomplete;
};
