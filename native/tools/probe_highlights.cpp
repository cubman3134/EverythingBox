// probe_highlights — text SELECTION and HIGHLIGHTS in the reader (issue #136, the half after bookmarks).
//
// Four things are asserted, and they are the four places this feature can be wrong:
//
//   1. THE SELECTION MODEL (ReaderSelection.h). The D-pad key map in full — word left/right, line up/down,
//      Enter to anchor and Enter again to commit, Escape to drop the selection and Escape again to leave the
//      mode. Driven as KEYS, not as setters, because the bug a cursor mode has is never "the caret cannot be
//      set", it is "the second Enter did the wrong thing".
//   2. THE RANGE ANCHOR. A highlight is the bookmark anchor with its RESERVED endOffset filled in
//      (ReaderAnchor.h has said so since the bookmarks half shipped), so it must round-trip through the same
//      toJson/fromJson and sort through the same total order. No second position model.
//   3. THE STORE AND ITS MERGE RULE (HighlightStore). Add/list/recolour/remove with a delete tombstone, the
//      id being the POSITION and not the colour, and — the rule the issue asks for — overlapping highlights
//      becoming ONE, including the case that is easy to get wrong and impossible to see: two that merely
//      TOUCH, end offset to start offset, with nothing between them.
//   4. REPAGINATION. The claim the whole anchor design rests on: the same anchor finds the same WORDS at a
//      different font size. Asserted over a REAL laid-out QTextDocument at two sizes — and the probe first
//      proves the reflow genuinely happened (the line count changes), so "the text is the same" is not a
//      sentence about a document nobody re-laid.
//
// WHAT IS DELIBERATELY NOT HERE. The reader WIDGET (EbookView) is not linked: probe_readerbookmarks already
// owns the real-reader end of #136 over real files, and the pieces this probe pins are the ones a widget
// cannot make true or false. The pdf/comic side is asserted through the HostedReader interface itself — a
// stub implementing it — because "a comic offers no highlight verb" is a property of that interface's
// DEFAULTS, which is exactly where the bookmarks half's bug lived (a defaulted override nobody supplied).
//
// ORACLE IS INDEPENDENT OF THE CODE UNDER TEST: every expected offset, word and order below is written out by
// hand from the fixture sentence, never read back from the thing being asserted.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so HighlightStore opens
// an everythingbox.ini that starts empty and is removed at exit.
//
// Prints HIGHLIGHTS-OK on success; any failure prints HIGHLIGHTS-FAIL <cond> (line) and exits non-zero.
#include "ReaderSelection.h"
#include "ReaderAnnotations.h"
#include "ReaderAnchor.h"
#include "BookmarkStore.h"
#include "HighlightStore.h"
#include "ProfileStore.h"
#include "HostedReader.h"

#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QGuiApplication>
#include <QPointF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HIGHLIGHTS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---------------------------------------------------------------------------------------------------------
// Fixtures and helpers
// ---------------------------------------------------------------------------------------------------------

// A book range, spelled out once so every section below reads as an intent rather than as four ints.
static ReaderAnchor range(int spine, int from, int to)
{
    ReaderAnchor a;
    a.kind      = ReaderAnchor::Book;
    a.spine     = spine;
    a.offset    = from;
    a.endOffset = to;
    return a;
}

static ReaderAnchor point(int spine, int at)
{
    ReaderAnchor a;
    a.kind   = ReaderAnchor::Book;
    a.spine  = spine;
    a.offset = at;
    return a;   // endOffset stays -1: a bookmark
}

// Every line's start offset in a laid-out document — the same flattening BookPageWidget::lineStarts() does,
// written independently here so the probe's idea of a line is not the reader's own claim about one.
static QVector<int> lineStartsOf(const QTextDocument& doc)
{
    QVector<int> out;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
    {
        QTextLayout* tl = b.layout();
        if (!tl) continue;
        for (int i = 0; i < tl->lineCount(); ++i)
            out.push_back(b.position() + tl->lineAt(i).textStart());
    }
    return out;
}

int main(int argc, char** argv)
{
    // A platform of our own before QGuiApplication exists — §4 lays out a real QTextDocument, which needs a
    // font database and therefore a platform. The runner launches every probe bare, so on a DISPLAY-less
    // Linux runner the default plugin would abort inside the constructor (the scar probe_readerbookmarks and
    // probe_books both carry). Set only when unset, so an explicit -platform still wins.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ProfileStore::setCurrent(QStringLiteral("hltest"));

    // The fixture sentence. Offsets into it are counted by hand below; nothing reads them back off the model.
    //          0123456789...
    const QString text = QStringLiteral("The quick brown fox jumps over the lazy dog.");
    //                                   0   4     10    16  20    26   31  35   40
    CHECK(text.mid(4, 5) == QStringLiteral("quick"));
    CHECK(text.mid(10, 5) == QStringLiteral("brown"));
    CHECK(text.mid(16, 3) == QStringLiteral("fox"));

    // ---- 1. The selection model: word/line movement and the whole key map ---------------------------------
    {
        // 1a. Word movement, forwards and back, in the units a reader thinks in. Right from inside a word
        // lands on the START of the next one; Left from a word start lands on the start of the PREVIOUS one.
        CHECK(ReaderSelection::nextWordStart(text, 0) == 4);    // "The " -> "quick"
        CHECK(ReaderSelection::nextWordStart(text, 4) == 10);   // "quick" -> "brown"
        CHECK(ReaderSelection::nextWordStart(text, 6) == 10);   // from INSIDE "quick", still "brown"
        CHECK(ReaderSelection::prevWordStart(text, 10) == 4);   // "brown" -> "quick"
        CHECK(ReaderSelection::prevWordStart(text, 12) == 10);  // from inside "brown" -> its own start
        CHECK(ReaderSelection::prevWordStart(text, 0) == 0);    // clamped, never wrapped
        CHECK(ReaderSelection::nextWordStart(text, text.size()) == text.size());
        // An apostrophe is INSIDE a word: "don't" is one word, not two. (A caret that stops on every
        // apostrophe makes a page of dialogue unusable on a pad.)
        const QString dialogue = QStringLiteral("I don't know");
        CHECK(ReaderSelection::nextWordStart(dialogue, 2) == 8);   // "don't" -> "know", not -> "t"

        // 1b. Line movement keeps the COLUMN and clamps at both ends — never wraps to the other end of the
        // chapter, which on a pad is how a caret gets lost.
        const QVector<int> lines{ 0, 10, 20, 30 };
        CHECK(ReaderSelection::moveLine(lines, 44, 13, +1) == 23);   // column 3 on the next line
        CHECK(ReaderSelection::moveLine(lines, 44, 23, -1) == 13);   // and back
        CHECK(ReaderSelection::moveLine(lines, 44, 3, -1) == 3);     // first line: no move
        CHECK(ReaderSelection::moveLine(lines, 44, 33, +1) == 33);   // last line: no move
        // A long column lands on the last character of the target line, NOT on the first character of the
        // line after it — otherwise Down would jump two lines whenever the target line is short.
        CHECK(ReaderSelection::moveLine(lines, 44, 8, +1) == 18);

        // 1c. THE KEY MAP, driven as keys. This is the sequence a person performs: enter the mode, walk to a
        // word, press Enter to start selecting, walk on, press Enter to finish.
        ReaderSelection::Model m;
        CHECK(!m.active);
        CHECK(m.key(Qt::Key_Right, text, lines) == ReaderSelection::Result::Ignored);  // inert until entered
        m.enter(0);
        CHECK(m.active);
        CHECK(!m.hasSelection());
        CHECK(m.key(Qt::Key_Right, text, lines) == ReaderSelection::Result::Moved);
        CHECK(m.caret == 4);                                     // on "quick"
        CHECK(m.key(Qt::Key_Return, text, lines) == ReaderSelection::Result::Anchored);
        CHECK(m.selecting());
        CHECK(!m.hasSelection());                                // anchored, but nothing covered yet
        CHECK(m.key(Qt::Key_Right, text, lines) == ReaderSelection::Result::Moved);
        CHECK(m.key(Qt::Key_Right, text, lines) == ReaderSelection::Result::Moved);
        CHECK(m.caret == 16);                                    // "quick brown " covered, caret on "fox"
        CHECK(m.hasSelection());
        CHECK(m.selStart() == 4 && m.selEnd() == 16);
        CHECK(m.key(Qt::Key_Return, text, lines) == ReaderSelection::Result::Committed);

        // 1d. The committed range is TRIMMED. Walking right by word leaves the caret on the first letter of
        // the NEXT word, so the raw span carries the space before it — and an untrimmed highlight draws a
        // trailing gap and stores an excerpt with a hanging space.
        const ReaderAnchor sel = m.toAnchor(2, text);
        CHECK(sel.kind == ReaderAnchor::Book);
        CHECK(sel.spine == 2);
        CHECK(sel.offset == 4);
        CHECK(sel.endOffset == 15);                              // 16 raw -> 15 trimmed: no trailing space
        CHECK(sel.isRange());
        CHECK(ReaderSelection::textOf(text, sel) == QStringLiteral("quick brown"));

        // 1e. Escape drops the selection but KEEPS the mode and the caret; a second Escape leaves the mode.
        // (Back that leaves the book from inside cursor mode is the trap — you cancel a selection far more
        // often than you close a book.)
        CHECK(m.key(Qt::Key_Escape, text, lines) == ReaderSelection::Result::Cleared);
        CHECK(m.active);
        CHECK(m.caret == 16);
        CHECK(!m.selecting());
        CHECK(m.key(Qt::Key_Escape, text, lines) == ReaderSelection::Result::Exited);
        CHECK(!m.active);

        // 1f. A selection of nothing but space is not a highlight — it is a point anchor, i.e. a bookmark's
        // shape, so a caller that stores it unconditionally cannot write a zero-width highlight.
        ReaderSelection::Model blank;
        blank.enter(3);          // "The| quick": the space
        blank.anchor = 3;
        blank.caret  = 4;
        const ReaderAnchor nothing = blank.toAnchor(0, text);
        CHECK(!nothing.isRange());
        CHECK(nothing.endOffset == -1);
    }

    // ---- 2. The range anchor is the bookmark anchor, with its reserved end filled in ----------------------
    {
        const ReaderAnchor a = range(1, 40, 60);
        CHECK(a.isRange());
        CHECK(!point(1, 40).isRange());
        // Round-trips EXACTLY through the format the bookmarks half already ships — the reserved endOffset is
        // carried, which is the whole reason the format never had to change to gain ranges.
        const ReaderAnchor back = ReaderAnchor::fromJson(a.toJson());
        CHECK(back == a);
        CHECK(back.endOffset == 60);
        // ...and the SAME total order sorts both kinds, so one list can hold them: a point at an offset comes
        // before any range starting there (endOffset -1 sorts first).
        CHECK(ReaderAnchor::inReadingOrder(point(1, 40), range(1, 40, 60)));
        CHECK(!ReaderAnchor::inReadingOrder(range(1, 40, 60), point(1, 40)));
    }

    // ---- 3. The store: identity, colour, removal, and the MERGE rule ---------------------------------------
    {
        const QString book = QStringLiteral("/library/Dune.epub");

        // 3a. Identity is the POSITION, not the colour. Two devices that mark the same passage in different
        // colours must converge on ONE row; if the colour were in the id they would keep two.
        CHECK(HighlightStore::idFor(book, range(0, 10, 20)) == HighlightStore::idFor(book, range(0, 10, 20)));
        CHECK(HighlightStore::idFor(book, range(0, 10, 20)) != HighlightStore::idFor(book, range(0, 10, 21)));
        CHECK(HighlightStore::idFor(QString(), range(0, 10, 20)).isEmpty());
        // ...and it is NOT the bookmark's id for the same spot: a point anchor and a range are different
        // positions, so a bookmark at offset 10 and a highlight starting at 10 are two rows, not one.
        CHECK(HighlightStore::idFor(book, range(0, 10, 20)) != BookmarkStore::idFor(book, point(0, 10)));

        // 3b. A point anchor is refused outright — a highlight with no range is a bookmark, and this store is
        // not where bookmarks live.
        CHECK(HighlightStore::add(book, point(0, 10), 0, QStringLiteral("x")).id.isEmpty());
        CHECK(HighlightStore::add(QString(), range(0, 10, 20), 0, QStringLiteral("x")).id.isEmpty());
        CHECK(HighlightStore::list(QString()).isEmpty());

        // 3c. Add / list in reading order.
        const HighlightStore::Highlight h2 = HighlightStore::add(book, range(1, 100, 120), 1, QStringLiteral("later"));
        const HighlightStore::Highlight h1 = HighlightStore::add(book, range(0, 10, 20), 0, QStringLiteral("earlier"));
        CHECK(!h1.id.isEmpty() && !h2.id.isEmpty());
        QVector<HighlightStore::Highlight> l = HighlightStore::list(book);
        CHECK(l.size() == 2);
        if (l.size() == 2)
        {
            CHECK(l.at(0).id == h1.id);            // spine 0 before spine 1, whatever order they were added in
            CHECK(l.at(0).text == QStringLiteral("earlier"));
            CHECK(l.at(1).color == 1);
        }

        // 3d. Recolour keeps the row (same id, new colour) — the "tap an existing highlight to recolor" verb.
        HighlightStore::setColor(h1.id, 3);
        l = HighlightStore::list(book);
        CHECK(l.size() == 2);                       // still two rows, not three
        CHECK(!l.isEmpty() && l.at(0).id == h1.id);
        CHECK(!l.isEmpty() && l.at(0).color == 3);
        // A forged/out-of-range colour reads back as the default rather than as an index nothing can draw.
        HighlightStore::setColor(h1.id, 99);
        CHECK(HighlightStore::list(book).at(0).color == 0);
        CHECK(HighlightStore::normalizeColor(-1) == 0);
        CHECK(HighlightStore::colorCount() == 4);   // the issue's "a choice of a few colors", pinned
        CHECK(HighlightStore::colorHex(0).startsWith(QLatin1Char('#')));

        // 3e. Which highlight is a spot inside — the lookup the D-pad's "this one" is built on. The range is
        // half-open, so the end offset is NOT inside it (otherwise two abutting highlights would both claim
        // the same character).
        CHECK(HighlightStore::at(book, 0, 15).id == h1.id);
        CHECK(HighlightStore::at(book, 0, 10).id == h1.id);   // the first character IS inside
        CHECK(HighlightStore::at(book, 0, 20).id.isEmpty());  // the end offset is not
        CHECK(HighlightStore::at(book, 1, 15).id.isEmpty());  // right offset, wrong chapter

        // 3f. Remove, and it stays removed.
        HighlightStore::remove(h2.id);
        CHECK(HighlightStore::list(book).size() == 1);
        HighlightStore::remove(h2.id);                        // idempotent
        CHECK(HighlightStore::list(book).size() == 1);
        HighlightStore::remove(h1.id);
        CHECK(HighlightStore::list(book).isEmpty());

        // 3g. Two books do not bleed into each other.
        HighlightStore::add(book, range(0, 10, 20), 0, QStringLiteral("a"));
        HighlightStore::add(QStringLiteral("/library/Neuromancer.epub"), range(0, 10, 20), 0, QStringLiteral("b"));
        CHECK(HighlightStore::list(book).size() == 1);
        CHECK(HighlightStore::list(QStringLiteral("/library/Neuromancer.epub")).size() == 1);
        HighlightStore::remove(HighlightStore::list(book).at(0).id);
        HighlightStore::remove(HighlightStore::list(QStringLiteral("/library/Neuromancer.epub")).at(0).id);
    }

    // ---- 4. THE MERGE RULE, pure -------------------------------------------------------------------------
    // "Overlapping highlights merge into one when they touch." Pinned against planMergeIn, which takes the
    // list it is handed — so every case below is a statement about the RULE and not about a store's state.
    {
        QVector<HighlightStore::Highlight> existing;
        auto put = [&](const QString& id, int spine, int from, int to) {
            HighlightStore::Highlight h;
            h.id = id; h.bookKey = QStringLiteral("/library/Dune.epub"); h.anchor = range(spine, from, to);
            existing.push_back(h);
        };

        // 4a. Overlapping.
        put(QStringLiteral("A"), 0, 10, 20);
        HighlightStore::MergePlan p = HighlightStore::planMergeIn(existing, range(0, 15, 30));
        CHECK(p.merged.offset == 10 && p.merged.endOffset == 30);
        CHECK(p.absorbed == (QStringList{QStringLiteral("A")}));

        // 4b. EXACTLY TOUCHING — the case the issue calls out and the one nothing else would catch. The end of
        // one range IS the start of the next, with nothing between them: on the page that is one band, so it
        // is one highlight. A rule written with '<' instead of '<=' passes every other case in this file.
        p = HighlightStore::planMergeIn(existing, range(0, 20, 30));
        CHECK(p.merged.offset == 10 && p.merged.endOffset == 30);
        CHECK(p.absorbed == (QStringList{QStringLiteral("A")}));
        // ...and touching from the other side, which is a different comparison.
        p = HighlightStore::planMergeIn(existing, range(0, 4, 10));
        CHECK(p.merged.offset == 4 && p.merged.endOffset == 20);

        // 4c. A gap of ONE character is not touching. (The half-open range [10,20) covers up to 19; a range
        // starting at 21 leaves character 20 unhighlighted between them, and two bands is what the page shows.)
        p = HighlightStore::planMergeIn(existing, range(0, 21, 30));
        CHECK(p.merged.offset == 21 && p.merged.endOffset == 30);
        CHECK(p.absorbed.isEmpty());

        // 4d. A different chapter never merges, however the offsets line up.
        p = HighlightStore::planMergeIn(existing, range(1, 15, 30));
        CHECK(p.absorbed.isEmpty());

        // 4e. TRANSITIVE. Absorbing one neighbour can bring the union up against the next, so the merge has to
        // repeat: a single pass would leave two rows where the page shows one band.
        existing.clear();
        put(QStringLiteral("A"), 0, 0, 10);
        put(QStringLiteral("B"), 0, 20, 30);
        put(QStringLiteral("C"), 0, 60, 70);              // far away: must NOT be swallowed
        p = HighlightStore::planMergeIn(existing, range(0, 8, 22));
        CHECK(p.merged.offset == 0 && p.merged.endOffset == 30);
        CHECK(p.absorbed.size() == 2);
        CHECK(p.absorbed.contains(QStringLiteral("A")) && p.absorbed.contains(QStringLiteral("B")));
        CHECK(!p.absorbed.contains(QStringLiteral("C")));

        // 4f. An inverted range is normalised rather than refused — a selection made backwards is still a
        // selection, and the caret can legitimately end up before the anchor.
        p = HighlightStore::planMergeIn(QVector<HighlightStore::Highlight>(), range(0, 30, 10));
        CHECK(p.merged.offset == 10 && p.merged.endOffset == 30);

        // 4g. ...and through the real store: adding an overlapping passage leaves ONE row, carrying the union
        // and the NEW colour (the most recent statement about the passage wins, the same rule the ts merge
        // follows). This is the end-to-end shape of "tap an existing highlight to extend".
        const QString book = QStringLiteral("/library/Merge.epub");
        HighlightStore::add(book, range(0, 10, 20), 0, QStringLiteral("first"));
        HighlightStore::add(book, range(0, 20, 30), 2, QStringLiteral("first and second"));
        const QVector<HighlightStore::Highlight> merged = HighlightStore::list(book);
        CHECK(merged.size() == 1);
        if (merged.size() == 1)
        {
            CHECK(merged.at(0).anchor.offset == 10);
            CHECK(merged.at(0).anchor.endOffset == 30);
            CHECK(merged.at(0).color == 2);
            CHECK(merged.at(0).text == QStringLiteral("first and second"));
            CHECK(merged.at(0).id == HighlightStore::idFor(book, range(0, 10, 30)));
            HighlightStore::remove(merged.at(0).id);
        }
    }

    // ---- 5. The panel: one list, document order, both kinds ------------------------------------------------
    {
        QVector<BookmarkStore::Bookmark> bms;
        QVector<HighlightStore::Highlight> hls;
        auto bm = [&](int spine, int at, const QString& label) {
            BookmarkStore::Bookmark b; b.id = QStringLiteral("bm%1").arg(at); b.anchor = point(spine, at);
            b.label = label; bms.push_back(b);
        };
        auto hl = [&](int spine, int from, int to, const QString& words, int colour) {
            HighlightStore::Highlight h; h.id = QStringLiteral("hl%1").arg(from); h.anchor = range(spine, from, to);
            h.text = words; h.color = colour; hls.push_back(h);
        };
        // Fed in a deliberately WRONG order, so the sort is doing the work rather than the input.
        hl(1, 50, 60, QStringLiteral("late words"), 2);
        bm(0, 90, QStringLiteral("Chapter One"));
        hl(0, 30, 40, QStringLiteral("early words"), 0);
        bm(1, 10, QStringLiteral("Chapter Two"));

        const QVector<ReaderAnnotations::Entry> panel = ReaderAnnotations::merged(bms, hls);
        CHECK(panel.size() == 4);
        if (panel.size() == 4)
        {
            // Document order across BOTH kinds: chapter 0 offset 30 (highlight), chapter 0 offset 90
            // (bookmark), chapter 1 offset 10 (bookmark), chapter 1 offset 50 (highlight).
            CHECK(panel.at(0).isHighlight() && panel.at(0).anchor.offset == 30);
            CHECK(!panel.at(1).isHighlight() && panel.at(1).anchor.offset == 90);
            CHECK(!panel.at(2).isHighlight() && panel.at(2).anchor.spine == 1);
            CHECK(panel.at(3).isHighlight() && panel.at(3).anchor.offset == 50);
            // A highlight is listed by the WORDS it covers (the excerpt the issue asks for); a bookmark by
            // its label. A highlight also carries its colour, so the panel can draw a swatch; a bookmark
            // carries none, which is how the two are told apart without a second list.
            CHECK(panel.at(0).excerpt == QStringLiteral("early words"));
            CHECK(panel.at(0).color == 0);
            CHECK(panel.at(1).excerpt == QStringLiteral("Chapter One"));
            CHECK(panel.at(1).color == -1);
            CHECK(ReaderAnnotations::rowLabel(panel.at(1)).contains(QStringLiteral("Chapter One")));
        }

        // A COMIC or a PDF: bookmarks, and no highlights at all — the list still behaves, which is the half of
        // "comics and PDFs are unchanged" that is easy to forget until the panel is empty or crashes.
        QVector<BookmarkStore::Bookmark> comicMarks;
        BookmarkStore::Bookmark c1; c1.id = QStringLiteral("c1"); c1.anchor.kind = ReaderAnchor::Comic;
        c1.anchor.page = 7; c1.label = QStringLiteral("Page 8 / 20"); comicMarks.push_back(c1);
        const QVector<ReaderAnnotations::Entry> comicPanel =
            ReaderAnnotations::merged(comicMarks, QVector<HighlightStore::Highlight>());
        CHECK(comicPanel.size() == 1);
        CHECK(!comicPanel.isEmpty() && !comicPanel.at(0).isHighlight());
        CHECK(!comicPanel.isEmpty() && comicPanel.at(0).excerpt == QStringLiteral("Page 8 / 20"));
        // An unlabelled bookmark is still a row a person can press — never a blank line.
        BookmarkStore::Bookmark c2; c2.id = QStringLiteral("c2"); c2.anchor.kind = ReaderAnchor::Comic;
        c2.anchor.page = 1; comicMarks.push_back(c2);
        const QVector<ReaderAnnotations::Entry> two =
            ReaderAnnotations::merged(comicMarks, QVector<HighlightStore::Highlight>());
        CHECK(two.size() == 2);
        CHECK(!two.isEmpty() && !two.at(0).excerpt.isEmpty());
    }

    // ---- 6. A kind with no text layer offers no highlight verb --------------------------------------------
    // Asserted against the INTERFACE's defaults, because that is where the bookmarks half's bug lived: a
    // defaulted override nobody supplied, which made a whole feature silently inert in two of three readers.
    // A stub is the right instrument here precisely because it supplies nothing.
    {
        struct SilentReader : HostedReader
        {
            QWidget* asWidget() override { return nullptr; }
            void setHostedChrome(bool) override {}
            int  currentPage() const override { return 1; }
            int  pageCount() const override { return 1; }
            void nextPage() override {}
            void prevPage() override {}
            int  chromeTopReserve() const override { return 0; }
        };
        SilentReader comic;
        CHECK(!comic.selectionSupported());   // no text layer -> the chrome draws no Select control
        CHECK(!comic.cursorMode());
        CHECK(!comic.beginCursorModeAt(QPointF(10, 10)));   // a long press over a comic is not a selection
        comic.beginCursorMode();                            // and the verbs are inert rather than absent
        CHECK(!comic.cursorMode());
        comic.gotoHighlight(0, 0);
        comic.endCursorMode();
    }

    // ---- 7. REPAGINATION: the same anchor finds the same words at a different font size ---------------------
    // The claim the whole anchor design rests on, over a REAL laid-out document. The reflow is proved to have
    // HAPPENED first (the line count changes), so what follows is not a sentence about a document nobody
    // re-laid — which is exactly how a "stable across repagination" test can be green and mean nothing.
    {
        const QString html = QStringLiteral(
            "<html><body><p>The quick brown fox jumps over the lazy dog. "
            "Pack my box with five dozen liquor jugs. "
            "How vexingly quick daft zebras jump.</p></body></html>");

        auto layOut = [&](int pt) {
            QTextDocument* d = new QTextDocument();
            d->setDocumentMargin(0);
            QFont f = d->defaultFont();
            f.setPointSize(pt);
            d->setDefaultFont(f);
            d->setHtml(html);
            d->setTextWidth(300.0);
            d->documentLayout()->documentSize();   // force the layout
            return d;
        };

        QTextDocument* small = layOut(9);
        QTextDocument* large = layOut(24);

        // The two really are laid out differently: more lines at the larger size, in the same column width.
        const QVector<int> smallLines = lineStartsOf(*small);
        const QVector<int> largeLines = lineStartsOf(*large);
        CHECK(smallLines.size() >= 1 && largeLines.size() >= 1);
        CHECK(largeLines.size() > smallLines.size());     // the reflow HAPPENED — the rest is not vacuous

        // ...and the text they carry is identical, character for character. That is the whole reason a
        // character offset is the anchor and a page index is not.
        const QString a = small->toPlainText();
        const QString b = large->toPlainText();
        CHECK(a == b);

        // A highlight taken at the small size finds the same WORDS at the large one.
        const int from = a.indexOf(QStringLiteral("five dozen liquor"));
        CHECK(from > 0);
        const ReaderAnchor taken = range(0, from, from + int(QStringLiteral("five dozen liquor").size()));
        CHECK(ReaderSelection::textOf(a, taken) == QStringLiteral("five dozen liquor"));
        CHECK(ReaderSelection::textOf(b, taken) == QStringLiteral("five dozen liquor"));
        // ...and it is on a DIFFERENT line at the two sizes, which is the thing that would break a
        // line-or-page-indexed anchor and leaves this one untouched.
        auto lineOf = [](const QVector<int>& starts, int pos) {
            int idx = 0;
            for (int i = 0; i < starts.size(); ++i) { if (starts.at(i) <= pos) idx = i; else break; }
            return idx;
        };
        CHECK(lineOf(smallLines, from) != lineOf(largeLines, from));

        // The caret model walks the LARGE layout's lines without leaving the text — the same movement the
        // reader drives, over line starts that came from a real layout rather than from a round number.
        ReaderSelection::Model m;
        m.enter(from);
        m.key(Qt::Key_Down, b, largeLines);
        CHECK(m.caret >= 0 && m.caret <= b.size());
        CHECK(m.caret != from);                       // it moved: there IS a line below at this size
        m.key(Qt::Key_Up, b, largeLines);
        CHECK(m.caret == from);                       // and back to the column it started in

        delete small;
        delete large;
    }

    if (failures == 0) { std::puts("HIGHLIGHTS-OK"); return 0; }
    std::fprintf(stderr, "HIGHLIGHTS: %d check(s) failed\n", failures);
    return 1;
}
