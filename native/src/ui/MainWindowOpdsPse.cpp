// OPDS-PSE: read a comic volume a page at a time off the server that holds it (#153).
//
// A separate translation unit rather than another thousand lines of MainWindow.cpp — the #186 direction,
// and the same shape the other per-feature MainWindow TUs use. Only the declarations live in MainWindow.h.
//
// WHAT THIS IS AND IS NOT. It is the THIRD SUPPLIER of the one page seam: a local archive opens through
// ComicView::openComic, an addon's `pages` resource opens through MainWindow::openImagePages (#188), and
// an OPDS-PSE server opens through that same openImagePages — the page list is simply built from a url
// TEMPLATE and a page count instead of arriving as JSON. It is not a second reader, not a second cache
// and not a second fetcher: the prefetch order, the per-item on-disk cache, the zoom, the two-up spread,
// the bookmarks and the resume are all the seam's, unchanged.
//
// WHAT IT ADDS on top of the seam, all of it through PageSupplyOptions:
//   * the fetch ORDER — the page the server says you reached, then the two after it, then the rest;
//   * refuse-a-partial — the packed CBZ is also the offline copy, so a volume with holes must not be
//     cached and served from cache for ever;
//   * where to land — pse:lastRead, because for a server-owned volume the server owns the position
//     (#153, the rule #83 set for Jellyfin: our own #136 sync keeps owning LOCAL items);
//   * who owns a failure — a stalled stream gets a retry and "download the volume instead", which is the
//     escape hatch the issue calls the feature.
//
// SAFETY. The catalog's device-local Basic auth rides on the item (HomeView attached it) and is put on
// every page request through the same NetHeaderApply path a feed fetch uses, so it is dropped on a
// cross-origin redirect. It is never logged — and neither is a page URL, because a PSE template can BE a
// credential (Kavita puts an apiKey in the query string). The log lines here name page NUMBERS and the
// server FAMILY, nothing else.
#include "MainWindow.h"

#include "FeedbackPolicy.h"           // kFeedbackLong — the one place a toast duration is decided
#include "HomeView.h"
#include "../comic/ComicView.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/NetHeaderApply.h"
#include "../ebook/OpdsPse.h"
#include "../ebook/OpdsPsePages.h"    // OpdsPse::pageList — the offer -> the seam's page list
#include "nav/NavOverlay.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWidget>

// One-line append to <app>/stream_debug.log — the same file MainWindow.cpp's own mwLog writes, which is
// static to that TU. Named apart so the two can never be confused for one function.
static void pseLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

// How wide a page to ask the server to render, when the template accepts a {maxWidth} hint. The reader's
// own viewport, because that is what the page is about to be scaled to — asking for a 4000px scan to
// display it 900px wide is exactly the bandwidth PSE exists to save. Clamped: a window mid-layout can
// report a width of 0 or 20, and a server told to render 20px wide would obey.
// The comic reader's own width only once it is ON SCREEN. Asking a hidden, never-laid-out widget gives a
// placeholder width (live against the fixture server: 100px, clamped up to the 480 floor, so the server was
// asked for 480px-wide pages to fill a 1280px window) — and this runs BEFORE the reader is presented, every
// time. The window is the honest answer at that moment, and it is the width the reader will fill.
static int pseViewportWidth(const ComicView* comic, const QWidget* win)
{
    int w = (comic && comic->isVisible()) ? comic->width() : 0;
    if (w <= 0 && win) w = win->width();
    return qBound(480, w > 0 ? w : 1280, 4096);
}

void MainWindow::readOpdsPse(const MediaItem& item)
{
    if (!item.pse.isValid())
    {
        // Should not happen — HomeView only offers the verb for a valid offer — but a row can be stale.
        notify(tr("This book can't be read online."), kFeedbackLong);
        return;
    }

    const OpdsPseLink& pse = item.pse;
    const int start0 = qMax(0, OpdsPse::lastReadIndex(pse));   // -1 ("no progress") means the start
    const int maxWidth = pseViewportWidth(comic_, this);

    // THE PAGE LIST — built by the pure rule in OpdsPsePages.h so a probe can hold it. Every page carries
    // the same Authorization header the feed fetch used; the seam puts it on the request through
    // NetHeaderApply, which drops it if a page redirects to another origin.
    const QVector<AddonPage> pages = OpdsPse::pageList(pse, maxWidth, item.requestHeaders);

    PageSupplyOptions opt;
    opt.fetchOrder      = OpdsPse::fetchOrder(pse.count, start0);
    opt.requireAllPages = true;    // the cached CBZ is also the offline copy: no holes in it
    opt.startPage0      = start0;
    opt.onIncomplete    = [this, item](int added, int expected) { offerPseFallback(item, added, expected); };

    // WHERE THE SEAM WILL HAVE PUT IT: the same key, the same SHA-1, the same cache folder it packs into.
    // The page-turn hook compares the open comic against this to tell THIS volume's reader from the next
    // comic the user opens, so it has to be known BEFORE the open — a cached volume opens synchronously
    // inside the call below, and a path set afterwards would arrive one page turn too late (and the first
    // page-turn signal, fired from inside openComic, would have disarmed the reporting instead).
    const QString key = item.id.isEmpty() ? item.url : item.id;

    // Arm the progress reporting for this volume BEFORE the open, so a reader that lands straight on the
    // resume page reports nothing new (pseSentPage_ starts at that page) rather than reporting it back.
    pseLink_       = pse;
    pseItem_       = item;
    pseComicPath_  = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                     + QStringLiteral("/manga/")
                     + QString::fromUtf8(QCryptographicHash::hash(key.toUtf8(),
                                                                  QCryptographicHash::Sha1).toHex())
                     + QStringLiteral(".cbz");
    pseSentPage_   = start0;
    pseWantPage_   = -1;
    // Nothing is reported until the reader has actually reached start0 (see MainWindow.h). A volume opening
    // at its first page needs no seek, so the open IS the landing.
    pseLanded_     = (start0 == 0);
    if (comic_)
        connect(comic_, &ComicView::pageInfoChanged, this, &MainWindow::onPseComicPageChanged,
                Qt::UniqueConnection);

    // The page count and the family of server, and nothing else: a page url can be a credential.
    pseLog(QStringLiteral("pse: read online \"%1\" — %2 page(s), from page %3, maxWidth %4")
               .arg(item.title).arg(pse.count).arg(start0 + 1).arg(maxWidth));

    // `key` is the item's own id, which is what keys the seam's on-disk CBZ cache — so re-opening this
    // volume is instant and works with the server unreachable. An empty run: an OPDS volume has no
    // captured list of neighbours, so a boundary press has nowhere to go, exactly as it does for a
    // downloaded one.
    openImagePages(item.title, key, pages, ChapterRun(),
                   /*landOnLastPage*/ false, /*handoffGen*/ -1, opt);
}

// A streamed read that could not be completed. The issue is explicit that this is the feature and not an
// edge case: on a slow link the escape hatch has to be offered inline, not left for the user to find.
//
// Deferred a turn because the card blocks in a nested event loop and we are standing inside a
// QNetworkReply::finished delivery — the #28 / #211 family. The item is captured BY VALUE for the same
// reason: nothing about the row is guaranteed to still exist a turn later.
void MainWindow::offerPseFallback(const MediaItem& item, int added, int expected)
{
    pseLog(QStringLiteral("pse: incomplete — %1 of %2 page(s) arrived").arg(added).arg(expected));
    QMetaObject::invokeMethod(this, [this, item, added, expected] {
        const QString title = item.title.isEmpty() ? tr("this volume") : item.title;
        const QString msg = added == 0
            ? tr("No pages of “%1” arrived. The server may be unreachable, or the connection too slow.")
                  .arg(title)
            : tr("Only %1 of %2 pages of “%3” arrived, so it wasn't kept — a volume with pages missing "
                 "would be read from the cache for ever.").arg(added).arg(expected).arg(title);
        const int pick = NavConfirm::ask(tr("Couldn't finish reading online"), msg,
                                         { tr("Try again"), tr("Download the volume instead"),
                                           tr("Not now") },
                                         /*focusIndex*/ 0, /*cancelIndex*/ 2, this);
        if (pick == 0) { readOpdsPse(item); return; }
        if (pick == 1)
        {
            // The existing download — the very route "Read online" was offered BESIDE, reached with the
            // same item, the same url and the same device-local auth header it already carries. The PSE
            // offer is stripped off the copy so the route cannot loop back into the picker.
            MediaItem dl = item;
            dl.pse = OpdsPseLink();
            if (dl.url.isEmpty())
            {
                notify(tr("This server offers “%1” for reading only, not for download.").arg(title),
                       kFeedbackLong);
                return;
            }
            pseLog(QStringLiteral("pse: falling back to the volume download"));
            openLibraryItem(dl);
        }
    }, Qt::QueuedConnection);
}

// A page turn while a streamed volume is open. Coalesced: a reader flicking through twenty pages should
// send one report, not twenty, and the LAST page is the true answer either way.
void MainWindow::onPseComicPageChanged()
{
    if (!pseLink_.isValid() || !comic_) return;
    // Is this still the streamed volume? The reader is reused for every comic, and pageInfoChanged fires
    // for all of them — including from inside openComic() for the NEXT one. Comparing the open file's own
    // key is the identity check; a mismatch disarms, so a local comic can never report progress to a
    // server (the #83 rule, from the other direction).
    if (comic_->itemKey() != pseComicPath_)
    {
        pseLink_ = OpdsPseLink();
        pseItem_ = MediaItem();
        pseComicPath_.clear();
        pseLanded_ = false;
        return;
    }
    const int page0 = comic_->currentPage() - 1;    // currentPage() is 1-based
    if (page0 < 0) return;
    // Before the landing, a page change is the OPEN, not the reader. ComicView emits this from inside
    // openComic() on page 1, before the seek to pse:lastRead has happened — reporting that would tell the
    // server you had gone BACK to page one every single time you opened a volume.
    if (!pseLanded_)
    {
        if (page0 != pseSentPage_) return;   // still pre-seek
        pseLanded_ = true;                   // the landing itself is not new progress either
        return;
    }
    if (page0 == pseSentPage_ || page0 == pseWantPage_) return;
    pseWantPage_ = page0;
    if (!pseProgressTimer_)
    {
        pseProgressTimer_ = new QTimer(this);
        pseProgressTimer_->setSingleShot(true);
        connect(pseProgressTimer_, &QTimer::timeout, this, &MainWindow::sendPseProgress);
    }
    pseProgressTimer_->start(1500);
}

// Tell the server where the reader got to — but ONLY when the feed's own page template names a server
// whose progress API this client actually knows (OpdsPse::progressReport). #153 is explicit that an
// endpoint is not to be invented: an unrecognised server simply keeps its progress its own way, and we
// send nothing at all rather than guessing a url at it.
void MainWindow::sendPseProgress()
{
    if (!pseLink_.isValid() || pseWantPage_ < 0 || pseWantPage_ == pseSentPage_) return;
    const int page0 = pseWantPage_;
    const OpdsPse::PseProgress rep = OpdsPse::progressReport(pseLink_, page0);
    if (!rep.isValid())
    {
        pseSentPage_ = page0;   // nothing to send, and nothing to keep retrying
        return;
    }
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    QNetworkRequest rq{ QUrl(rep.url) };
    rq.setHeader(QNetworkRequest::ContentTypeHeader, rep.contentType);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    rq.setTransferTimeout(15000);
    // The catalog's own Authorization header, through NetHeaderApply so it is scoped and so a redirect
    // off-origin drops it — the same guard the page and feed fetches get. The headers are DECLARED for
    // the catalog's own url (the volume's, else the page template's) rather than for the progress url:
    // declaring them for the request being made would make the scoping trivially true, and the whole
    // point is that a progress endpoint on some other host gets none of them.
    const QString declaredFor = pseItem_.url.isEmpty() ? pseLink_.hrefTemplate : pseItem_.url;
    QNetworkReply* reply = NetHeaderApply::send(docNam_, rq, rep.method.toUtf8(), rep.body,
                                                pseItem_.requestHeaders, declaredFor);
    if (!reply) return;
    const int page1 = page0 + 1;
    const QString server = rep.server;
    connect(reply, &QNetworkReply::finished, this, [reply, page1, server] {
        reply->deleteLater();
        // The exception, never the request: rep.url can carry an apiKey and the headers carry a password.
        if (reply->error() != QNetworkReply::NoError)
            pseLog(QStringLiteral("pse: %1 progress report for page %2 refused (%3)")
                       .arg(server).arg(page1).arg(int(reply->error())));
        else
            pseLog(QStringLiteral("pse: %1 progress reported — page %2").arg(server).arg(page1));
    });
    pseSentPage_ = page0;
}
