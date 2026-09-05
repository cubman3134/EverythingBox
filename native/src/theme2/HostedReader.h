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
#include <QPointF>
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
    // book: re-read the stored reading preferences (family/size/spacing/margin/justify/theme) and apply them
    // live. The chrome writes those preferences and then asks for this, rather than reaching into the reader:
    // the reader already knows how to apply them, and there is exactly one place that decides what they mean.
    virtual void reloadTypography() {}
    virtual void zoomDelta(int) {}              // pdf/comic: zoom in(+) / out(-) one step
    virtual void fitWidth() {}                  // pdf/comic: fit-to-width
    virtual void setTwoUp(bool) {}              // comic: enable/disable the two-page spread
    virtual bool twoUp() const { return false; } // comic: is the two-up spread preference on
    virtual bool spreadActive() const { return false; } // comic: a two-page spread is on screen RIGHT NOW (fit
                                                        // + two-up + a next page exists) — the themed page label
                                                        // then reads a RANGE ("3–4 / 20"), matching the classic bar

    // Bookmarks (issue #136): the host captures the current spot into a ReaderAnchor and jumps back to one. A
    // kind implements only what its anchor needs — a book overrides itemKey/spineIndex/textOffset/gotoSpineOffset;
    // a pdf/comic overrides itemKey and jumps by page (its anchor is page-only, captured from currentPage()).
    virtual QString itemKey() const { return {}; }         // the book's stable natural key (its file path / addon id)
    virtual int  spineIndex() const { return 0; }          // book: current spine (chapter) index
    virtual int  textOffset() const { return 0; }          // book: current character offset (topTextPosition)
    virtual void gotoSpineOffset(int /*spine*/, int /*offset*/) {} // book: jump to a chapter + offset
    // Jump to a 0-based page. Every kind implements it: pdf/comic address their own pages directly, and a
    // book maps the BOOK-WIDE page (the number its chrome shows) back onto a chapter plus a page inside it.
    // Used by a bookmark jump and by the chrome's progress bar, which is a scale over exactly this number.
    virtual void gotoPage(int /*page0*/) {}

    // Read aloud (issue #145). A BOOK narrates; a pdf/comic does not, so every one of these keeps an inert
    // default and the chrome asks readAloudAvailable() before it draws a single control. That question is
    // answered false by three separate things — a kind that has no text, a build without the Qt TextToSpeech
    // module, and a platform with no speech engine — and the row is the row it always was in all three.
    virtual bool readAloudAvailable() const { return false; }
    virtual bool readAloudActive() const { return false; }
    virtual bool readAloudPaused() const { return false; }
    virtual void toggleReadAloud() {}                  // not narrating -> start here; narrating -> stop
    virtual void readAloudTogglePause() {}
    virtual void readAloudSkip(int /*paragraphs*/) {}  // paragraph back / forward
    virtual double readAloudSpeed() const { return 1.0; }   // the shared #140 per-book speed
    virtual void readAloudCycleSpeed() {}
    virtual QString readAloudVoiceName() const { return {}; }
    virtual void readAloudCycleVoice() {}

    // Text selection and highlights (issue #136). Selection is a MODE, entered on purpose from the reader's
    // menu, because the pad has no pointer to sweep a paragraph with — see ReaderSelection.h for the key map.
    // Only a kind with a TEXT LAYER can offer it: a comic has no text at all and a pdf is drawn, not flowed, so
    // both keep the inert defaults here and their chrome draws no selection control (the panel still lists
    // their bookmarks — highlights are simply not a verb they have).
    virtual bool selectionSupported() const { return false; }
    virtual bool cursorMode() const { return false; }   // is the caret live right now (the host stops arbitrating keys)
    virtual void beginCursorMode() {}
    // ...and the TOUCH way in: a finger held on a word opens the caret THERE. `pos` is in the reader widget's
    // own coordinates; false means the point was not over text (or the kind has none), and the host then falls
    // through to whatever the gesture would otherwise have meant.
    virtual bool beginCursorModeAt(const QPointF& /*pos*/) { return false; }
    virtual void endCursorMode() {}
    // Jump to a highlight: the same spine+offset jump a bookmark takes, plus landing the caret inside the
    // passage so the very next Enter offers recolour/remove — which is what "tap an existing highlight" means
    // when the tap is a D-pad.
    virtual void gotoHighlight(int /*spine*/, int /*offset*/) {}
};
