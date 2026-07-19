#pragma once
// HostedReader — the tiny interface ReaderChromeHost + ReaderBridge drive, implemented by EbookView / PdfView /
// ComicView so ONE themed-chrome host composes over any of the three (Plan B1, Tasks 3-5, VARIANT A). It is a
// plain (non-QObject) interface — the views are already QWidgets, so a second QObject base is illegal — and the
// host reaches the QObject side via asWidget(): it reparents/focuses/event-filters that widget and connects the
// page-change notification through the string-based SIGNAL(pageInfoChanged()) that EVERY implementer declares.
//
// A kind overrides only the settings-row commands its own chrome exposes (book: font + a chapter toc; pdf/comic:
// zoom + fit; comic additionally: a two-up spread toggle); every other method keeps a harmless default so a view
// never has to spell out a command it does not offer. The wrappers are thin — they call exactly what the reader's
// own bar buttons already call, so there is ZERO render/scroll-logic change behind this interface.
#include <QStringList>

class QWidget;

class HostedReader
{
public:
    virtual ~HostedReader() = default;

    virtual QWidget* asWidget() = 0;            // == this; the reparent / focus / event-filter target
    virtual void setHostedChrome(bool on) = 0;  // themed mode: suppress the reader's own bar(s)/overlays
    virtual int  currentPage() const = 0;       // 1-based page at the current spot
    virtual int  pageCount()  const = 0;
    virtual void nextPage() = 0;
    virtual void prevPage() = 0;
    virtual int  chromeTopReserve() const = 0;  // px the themed top strip aligns to (book's menu inset)

    // Settings-row commands — a kind overrides the ones its chrome offers; the rest stay inert.
    virtual void fontDelta(int) {}              // book: change the reading font by ±pt
    virtual int  fontPt() const { return 14; }  // book: current font size
    virtual QStringList tocTitles() const { return {}; } // book: chapter titles (empty ⇒ no toc)
    virtual void gotoTocIndex(int) {}           // book: jump to a chapter
    virtual void zoomDelta(int) {}              // pdf/comic: zoom in(+) / out(-) one step
    virtual void fitWidth() {}                  // pdf/comic: fit-to-width
    virtual void setTwoUp(bool) {}              // comic: enable/disable the two-page spread
    virtual bool twoUp() const { return false; } // comic: is the two-up spread preference on
};
