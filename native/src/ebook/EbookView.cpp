#include "EbookView.h"
#include "../core/AppBrand.h"
#include "EpubBook.h"
#include "MobiBook.h"
#include "PdfTextBook.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include "../ui/PlayerIcons.h"   // the drawn warning mark (a colour emoji font ignores the chip's ink)
#include "../core/Settings.h"
#include "ReadAloud.h"                 // the pure divider/stripper/position map (issue #145)
#ifdef EB_HAVE_TTS
#  include "ReadAloudController.h"      // compiled only when the build has Qt TextToSpeech
#endif

#include <QFile>
#include <QListWidget>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QPainter>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTextOption>
#include <QColor>
#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QImage>
#include <QSettings>
#include <QCryptographicHash>
#include <QUrl>
#include <QFileInfo>

// ---- BookPageWidget: paints a single, line-clean page of a QTextDocument -------------------------------

namespace {

// A document that can load a chapter's images from disk (relative to the chapter's folder). QTextDocument's
// default loadResource doesn't fetch local image files, so EPUB pictures would be blank without this.
class BookDocument : public QTextDocument
{
public:
    explicit BookDocument(QObject* parent = nullptr) : QTextDocument(parent) {}
    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type == QTextDocument::ImageResource)
        {
            const QUrl resolved = name.isRelative() ? baseUrl().resolved(name) : name;
            const QString local = resolved.isLocalFile() ? resolved.toLocalFile() : resolved.path();
            QImage img(local);
            if (!img.isNull()) return img;
        }
        return QTextDocument::loadResource(type, name);
    }
};

} // namespace

BookPageWidget::BookPageWidget(QWidget* parent) : QWidget(parent)
{
    doc_ = new BookDocument(this);
    doc_->setDocumentMargin(0); // we paint our own paper margin
    doc_->setDefaultStyleSheet(QStringLiteral(
        "body{margin:0;} p{margin:0 0 0.7em 0;} img{max-width:100%;}"));
    QFont f = doc_->defaultFont();
    f.setPointSize(fontPt_);
    doc_->setDefaultFont(f);

    setMouseTracking(true);            // so we get moves without a button held -> reveal the menu
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);       // keep keyboard focus on EbookView for arrow paging
    setCursor(Qt::PointingHandCursor);
}

void BookPageWidget::setContent(const QString& html, const QString& baseDir)
{
    doc_->setBaseUrl(QUrl::fromLocalFile(baseDir + QStringLiteral("/")));
    doc_->setHtml(html);   // replaces the document, so the per-block line spacing has to be re-applied below
    QFont f = doc_->defaultFont();
    f.setPointSize(fontPt_);
    if (!fontFamily_.isEmpty()) f.setFamily(fontFamily_);
    doc_->setDefaultFont(f);
    applyDocFormatting();  // line spacing + justification onto the freshly-set chapter (#135)
    topPos_ = 0;
    relayout();
    update();
}

void BookPageWidget::setFontPointSize(int pt)
{
    fontPt_ = pt;
    QFont f = doc_->defaultFont();
    f.setPointSize(pt);
    if (!fontFamily_.isEmpty()) f.setFamily(fontFamily_);
    doc_->setDefaultFont(f);
    relayout();
    update();
}

// Reader typography (issue #135): apply every resolved knob in one pass. Font family + size go on the default
// font; line spacing + justification onto the document; the page margin is stored (sideMargin() derives px from
// it) and the theme's two colours become the widget palette's Base (paper) and Text (ink), which paintEvent and
// the footer already read — so text and paper change together, never one repaint apart. topPos_ is left alone;
// the owner captures and restores it around this call so the reader stays on the same words.
void BookPageWidget::setTypography(const ReaderTypography::Resolved& r)
{
    fontFamily_     = r.fontFamily;
    fontPt_         = r.sizePt;
    lineSpacingPct_ = r.lineSpacingPct;
    marginPct_      = r.marginPct;
    justify_        = r.justify;

    QFont f = doc_->defaultFont();
    f.setPointSize(fontPt_);
    // An empty family means "no override": fall back to the document's own default family rather than forcing a
    // named one, mirroring ReaderTypography's empty-family contract.
    f.setFamily(fontFamily_.isEmpty() ? BookDocument().defaultFont().family() : fontFamily_);
    doc_->setDefaultFont(f);

    QPalette pal = palette();
    pal.setColor(QPalette::Base, QColor(r.background));
    pal.setColor(QPalette::Window, QColor(r.background));
    pal.setColor(QPalette::Text, QColor(r.textColor));
    setPalette(pal);

    applyDocFormatting();
    relayout();
    update();
}

// Apply the current line spacing + justification to every block of the live document. Line height is set
// proportionally so it scales with the font; justification goes on the DEFAULT text option (not per block) so a
// publisher's explicitly-centred heading keeps its alignment rather than being flattened. Character offsets are
// unchanged by either, so the reading anchor (topPos_) survives.
void BookPageWidget::applyDocFormatting()
{
    QTextOption opt = doc_->defaultTextOption();
    opt.setAlignment(justify_ ? Qt::AlignJustify : Qt::AlignLeft);
    doc_->setDefaultTextOption(opt);

    QTextCursor cur(doc_);
    cur.beginEditBlock();
    for (QTextBlock b = doc_->begin(); b.isValid(); b = b.next())
    {
        cur.setPosition(b.position());
        QTextBlockFormat fmt = cur.blockFormat();
        fmt.setLineHeight(lineSpacingPct_, QTextBlockFormat::ProportionalHeight);
        cur.setBlockFormat(fmt);
    }

    // Force the CHOSEN reading font onto the text itself. setDefaultFont only supplies a fallback, and a book
    // that names its own font in markup — most of them do — overrides it, so picking a typeface changed
    // nothing on screen. Size survived that because books size text in relative units far more often than they
    // leave the family unstated.
    //
    // Only when a family was actually chosen: an empty one means "the book decides", which is the whole
    // meaning of the Default entry, and forcing anything then would flatten a publisher's deliberate
    // monospace for code or a display face for headings.
    if (!fontFamily_.isEmpty())
    {
        QTextCharFormat ff;
        ff.setFontFamilies({ fontFamily_ });
        QTextCursor all(doc_);
        all.select(QTextCursor::Document);
        all.mergeCharFormat(ff);   // merge, so weight/italics/relative size are left as the book set them
    }
    cur.endEditBlock();
}

void BookPageWidget::setTopInset(int px)
{
    topMargin_ = qMax(0, px);
    relayout();
    update();
}

void BookPageWidget::setFooter(const QString& s)
{
    if (footer_ == s) return;
    footer_ = s;
    update();
}

// Re-lay the document at the current width and rebuild the line table. topPos_ (a document offset) is kept,
// then snapped to the start of whatever line now holds it - so the same words stay at the top of the page.
// Text fills the available width (no column cap) so it gets as big as the window.
// Page margin (issue #135): marginPct_ of the current width, with a small floor so text never kisses the edge
// even at 0%. Derived (not stored in px) so it tracks a resize automatically.
qreal BookPageWidget::sideMargin() const { return qMax(12.0, marginPct_ / 100.0 * qreal(width())); }
qreal BookPageWidget::contentW() const { return qMax(1.0, qreal(width()) - 2 * sideMargin()); }
qreal BookPageWidget::contentLeft() const { return sideMargin(); }

// Re-lay the document at the current width and rebuild the line table. Crucially, topPos_ (the reading
// anchor - a document offset) is NOT touched here: a resize must not move the reading position. The page
// just re-derives which line that offset is on. (Re-snapping on every resize is what used to let a drag
// accumulate drift.)
void BookPageWidget::relayout()
{
    doc_->setTextWidth(contentW());
    rebuildLines();
    buildPageTops();
    topPos_ = qBound(0, topPos_, lines_.last().pos); // keep it valid, but don't snap it to a line start
    recomputeCurrentPage();
    emit layoutChanged();
}

void BookPageWidget::rebuildLines()
{
    lines_.clear();
    QAbstractTextDocumentLayout* lay = doc_->documentLayout();
    for (QTextBlock b = doc_->begin(); b.isValid(); b = b.next())
    {
        QTextLayout* tl = b.layout();
        const qreal blockTop = lay->blockBoundingRect(b).top();
        for (int i = 0; i < tl->lineCount(); ++i)
        {
            const QTextLine line = tl->lineAt(i);
            lines_.push_back({ blockTop + line.y(), line.height(), b.position() + line.textStart() });
        }
    }
    if (lines_.isEmpty()) lines_.push_back({ 0.0, 1.0, 0 });
}

// Group lines into pages from the start, each holding as many whole lines as fit the content height. Used
// only to total/number pages for the footer; the on-screen page flows from topPos_, not this grid.
void BookPageWidget::buildPageTops()
{
    pageTops_.clear();
    int i = 0;
    while (i < lines_.size())
    {
        pageTops_.push_back(lines_[i].pos);
        i = lastFittingLine(i) + 1;
    }
    if (pageTops_.isEmpty()) pageTops_.push_back(0);
}

int BookPageWidget::lineIndexForPos(int pos) const
{
    // Last line whose start is <= pos (lines_ is ordered by position).
    int lo = 0, hi = lines_.size() - 1, ans = 0;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (lines_[mid].pos <= pos) { ans = mid; lo = mid + 1; }
        else                          hi = mid - 1;
    }
    return ans;
}

int BookPageWidget::lastFittingLine(int startLine) const
{
    const qreal ph = contentH();
    const qreal y0 = lines_[startLine].y;
    int m = startLine;
    while (m + 1 < lines_.size() && (lines_[m + 1].y + lines_[m + 1].h - y0) <= ph)
        ++m;
    return m; // always >= startLine, so paging makes progress even if one line exceeds the page
}

void BookPageWidget::recomputeCurrentPage()
{
    // Which from-start page holds topPos_ (largest pageTop <= topPos_).
    int lo = 0, hi = pageTops_.size() - 1, ans = 0;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (pageTops_[mid] <= topPos_) { ans = mid; lo = mid + 1; }
        else                             hi = mid - 1;
    }
    curPage_ = ans;
}

bool BookPageWidget::atFirst() const { return lineIndexForPos(topPos_) <= 0; }

bool BookPageWidget::atLast() const
{
    return lastFittingLine(lineIndexForPos(topPos_)) + 1 >= lines_.size();
}

void BookPageWidget::showFirstPage()
{
    topPos_ = lines_.first().pos;
    recomputeCurrentPage();
    update();
}

void BookPageWidget::showLastPage()
{
    topPos_ = pageTops_.last(); // the from-start grid's final page top shows the chapter's tail
    recomputeCurrentPage();
    update();
}

bool BookPageWidget::pageForward()
{
    const int next = lastFittingLine(lineIndexForPos(topPos_)) + 1;
    if (next >= lines_.size()) return false; // nothing more in this chapter
    topPos_ = lines_[next].pos;
    recomputeCurrentPage();
    update();
    return true;
}

bool BookPageWidget::pageBackward()
{
    const int start = lineIndexForPos(topPos_);
    if (start <= 0) return false;
    // The previous page ends just above the current top: walk back from start-1 while the lines still fit.
    const qreal endBottom = lines_[start - 1].y + lines_[start - 1].h;
    int s = start - 1;
    while (s - 1 >= 0 && (endBottom - lines_[s - 1].y) <= contentH()) --s;
    topPos_ = lines_[s].pos;
    recomputeCurrentPage();
    update();
    return true;
}

void BookPageWidget::setProgress(double f)
{
    const int p = pageTops_.size() > 1 ? int(f * (pageTops_.size() - 1) + 0.5) : 0;
    topPos_ = pageTops_[qBound(0, p, int(pageTops_.size()) - 1)];
    recomputeCurrentPage();
    update();
}

void BookPageWidget::scrollToTextPosition(int pos)
{
    topPos_ = qBound(0, pos, lines_.last().pos); // exact offset - the page is rendered starting at this word
    recomputeCurrentPage();
    update();
}

// ---- Read aloud (issue #145): the three things narration asks of the page ----------------------------------

// The chapter's text in DOCUMENT coordinates. toPlainText() is length-preserving - it substitutes the block
// separator with a single newline character rather than removing it - so an offset here is an offset that
// topTextPosition and scrollToTextPosition speak, and the divider's ranges need no translation at all.
QString BookPageWidget::plainText() const
{
    return doc_ ? doc_->toPlainText() : QString();
}

void BookPageWidget::setSpokenRange(int start, int end)
{
    const int s = (end > start) ? start : -1;
    const int e = (end > start) ? end   : -1;
    if (s == spokenStart_ && e == spokenEnd_) return;
    spokenStart_ = s;
    spokenEnd_   = e;
    update();
}

// Turn the page only when the spoken spot has left it. Checking first is what keeps the text still while a long
// paragraph is read: re-anchoring on every utterance would shuffle the page under the reader's eye even when
// nothing needed to move.
void BookPageWidget::ensurePosVisible(int pos)
{
    if (lines_.isEmpty() || !doc_) return;
    const int startLine = lineIndexForPos(topPos_);
    const int endLine   = lastFittingLine(startLine);
    const int pageEnd   = (endLine + 1 < lines_.size()) ? lines_[endLine + 1].pos : doc_->characterCount();
    if (pos >= topPos_ && pos < pageEnd) return;   // already on the page being shown
    topPos_ = lines_[lineIndexForPos(qBound(0, pos, lines_.last().pos))].pos;
    recomputeCurrentPage();
    update();
}

int BookPageWidget::countPages(const QString& html, const QString& baseDir) const
{
    // Count whole-line pages for a chapter in a throwaway document with the live view's font and width -
    // without touching the on-screen document.
    BookDocument d;
    d.setDocumentMargin(0);
    d.setDefaultStyleSheet(doc_->defaultStyleSheet());
    d.setBaseUrl(QUrl::fromLocalFile(baseDir + QStringLiteral("/")));
    d.setHtml(html);
    d.setDefaultFont(doc_->defaultFont());
    // Mirror the live document's typography so an off-screen chapter's page count matches what it will paginate
    // to on-screen (#135): same justification (default text option) and same per-block line spacing.
    d.setDefaultTextOption(doc_->defaultTextOption());
    {
        QTextCursor cur(&d);
        cur.beginEditBlock();
        for (QTextBlock b = d.begin(); b.isValid(); b = b.next())
        {
            cur.setPosition(b.position());
            QTextBlockFormat fmt = cur.blockFormat();
            fmt.setLineHeight(lineSpacingPct_, QTextBlockFormat::ProportionalHeight);
            cur.setBlockFormat(fmt);
        }
        cur.endEditBlock();
    }
    d.setTextWidth(doc_->textWidth());

    const qreal ph = contentH();
    QAbstractTextDocumentLayout* lay = d.documentLayout();
    QVector<LineGeom> ls;
    for (QTextBlock b = d.begin(); b.isValid(); b = b.next())
    {
        QTextLayout* tl = b.layout();
        const qreal blockTop = lay->blockBoundingRect(b).top();
        for (int i = 0; i < tl->lineCount(); ++i)
            ls.push_back({ blockTop + tl->lineAt(i).y(), tl->lineAt(i).height(), 0 });
    }
    if (ls.isEmpty()) return 1;

    int pages = 0, i = 0;
    while (i < ls.size())
    {
        ++pages;
        const qreal y0 = ls[i].y;
        int m = i;
        while (m + 1 < ls.size() && (ls[m + 1].y + ls[m + 1].h - y0) <= ph) ++m;
        i = m + 1;
    }
    return qMax(1, pages);
}

void BookPageWidget::resizeEvent(QResizeEvent*)
{
    relayout(); // keeps topPos_ - the first word doesn't move
    update();
}

void BookPageWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (!doc_ || lines_.isEmpty()) return;

    const qreal cw = contentW(), cl = contentLeft();
    const int startLine = lineIndexForPos(topPos_);
    const int endLine = lastFittingLine(startLine);
    const qreal y0 = lines_[startLine].y;
    const qreal firstH = lines_[startLine].h;

    // If the anchor fell mid-line (after a reflow), shift the first line left so the anchored word sits at
    // the left edge - so the exact first word stays put. anchorX is 0 when the anchor is a line start.
    const qreal anchorX = anchorXInLine();

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = palette();                  // text drawn in QPalette::Text

    // The spoken paragraph (issue #145), as a document SELECTION on the one PaintContext both draws below
    // share - so the highlight is clipped, shifted and page-broken by exactly the same arithmetic as the text
    // it sits behind, instead of a rectangle drawn over it that a reflow could leave stranded. The tint is the
    // reading theme's own ink at low alpha, so it lands legibly on paper, sepia, dark and true black alike.
    if (spokenEnd_ > spokenStart_ && spokenStart_ >= 0)
    {
        const int last = qMax(0, doc_->characterCount() - 1);
        QTextCursor sc(doc_);
        sc.setPosition(qBound(0, spokenStart_, last));
        sc.setPosition(qBound(0, spokenEnd_, last), QTextCursor::KeepAnchor);
        QColor tint = palette().color(QPalette::Text);
        tint.setAlpha(46);
        QAbstractTextDocumentLayout::Selection sel;
        sel.cursor = sc;
        sel.format.setBackground(tint);
        ctx.selections.append(sel);
    }

    // First line: drawn shifted by anchorX; the words before the anchor fall left of the clip and vanish.
    p.save();
    p.setClipRect(QRectF(cl, topMargin_, cw, firstH));
    p.translate(cl - anchorX, topMargin_ - y0);
    ctx.clip = QRectF(anchorX, y0, cw, firstH);
    doc_->documentLayout()->draw(&p, ctx);
    p.restore();

    // The remaining whole lines, drawn normally below the first. Clip to the bottom of the LAST whole line
    // (not to the full content height): QTextDocumentLayout draws a whole paragraph if any of it is in view,
    // so without a tight clip the next line would bleed partway into the leftover space at the bottom.
    if (endLine > startLine)
    {
        const qreal y1 = lines_[startLine + 1].y;
        const qreal pageBottom = lines_[endLine].y + lines_[endLine].h - y0; // height down to last whole line
        p.save();
        p.setClipRect(QRectF(cl, topMargin_ + firstH, cw, pageBottom - firstH));
        p.translate(cl, topMargin_ - y0);
        ctx.clip = QRectF(0, y1, cw, pageBottom - (y1 - y0));
        doc_->documentLayout()->draw(&p, ctx);
        p.restore();
    }

    // Page-number footer, centered in the bottom margin in a muted colour.
    if (!footer_.isEmpty())
    {
        QColor ink = palette().color(QPalette::Text);
        ink.setAlpha(140);
        p.setPen(ink);
        p.drawText(QRectF(0, height() - botMargin_, width(), botMargin_),
                   Qt::AlignHCenter | Qt::AlignVCenter, footer_);
    }
}

// X (document coords) of the anchor character within its line - i.e. how far the first line must shift left
// so the anchored word starts at the left edge. 0 when the anchor sits at the start of its line.
qreal BookPageWidget::anchorXInLine() const
{
    const int startLine = lineIndexForPos(topPos_);
    if (lines_.isEmpty() || topPos_ <= lines_[startLine].pos) return 0.0;
    const QTextBlock blk = doc_->findBlock(topPos_);
    if (QTextLayout* tl = blk.layout())
    {
        const QTextLine ln = tl->lineForTextPosition(topPos_ - blk.position());
        if (ln.isValid()) return ln.cursorToX(topPos_ - blk.position());
    }
    return 0.0;
}

void BookPageWidget::mousePressEvent(QMouseEvent* e)
{
    const QPoint pos = e->pos();

    // An in-book hyperlink (footnote / cross-reference) takes priority over the page-turn zones.
    if (!lines_.isEmpty())
    {
        const int startLine = lineIndexForPos(topPos_);
        const qreal y0 = lines_[startLine].y, firstH = lines_[startLine].h;
        // The first line is shifted by anchorX; rows below it are not.
        const qreal shift = (pos.y() < topMargin_ + firstH) ? anchorXInLine() : 0.0;
        const QPointF docPos(pos.x() - contentLeft() + shift, pos.y() - topMargin_ + y0);
        const QString href = doc_->documentLayout()->anchorAt(docPos);
        if (!href.isEmpty()) { emit anchorClicked(href); return; }
    }

    if (pos.y() < topMargin_)           { emit menuRequested(); return; } // the strip the menu lives in
    if (pos.x() < width() / 2)            emit prevRequested();
    else                                 emit nextRequested();
}

void BookPageWidget::mouseMoveEvent(QMouseEvent*)
{
    emit menuRequested(); // any movement wakes the menu (it re-arms its own auto-hide)
}

// ---- EbookView: chapter flow, persistence, overlays ---------------------------------------------------

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-book settings prefix, e.g. "ebook/<hash>/".
static QString bookKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("ebook/") + QString::fromLatin1(h) + QStringLiteral("/");
}

// Sniff the file and create the matching parser. MOBI is detected by the PalmDB signature at offset 60
// (works even when an Allarr book was cached under a ".epub" name); a "%PDF-" header is read as a reflowable
// text book; anything else is treated as EPUB.
static std::unique_ptr<EbookSource> makeSource(const QString& path)
{
    QFile f(path);
    QByteArray head;
    if (f.open(QIODevice::ReadOnly)) { head = f.read(68); f.close(); }
    const QByteArray sig = head.mid(60, 8);
    if (sig == QByteArray("BOOKMOBI") || sig == QByteArray("TEXtREAd"))
        return std::make_unique<MobiBook>();
    if (head.startsWith("%PDF-"))
        return std::make_unique<PdfTextBook>();
    return std::make_unique<EpubBook>();
}

EbookView::EbookView(QWidget* parent) : QWidget(parent)
{
    book_ = std::make_unique<EpubBook>(); // a valid (closed) source until a book is opened

    page_ = new BookPageWidget(this);
    page_->setTopInset(kMenuHeight); // reserve the menu's strip up top so it never covers text
    connect(page_, &BookPageWidget::nextRequested, this, &EbookView::nextPage);
    connect(page_, &BookPageWidget::prevRequested, this, &EbookView::prevPage);
    connect(page_, &BookPageWidget::menuRequested, this, &EbookView::revealMenu);
    connect(page_, &BookPageWidget::anchorClicked, this, &EbookView::onAnchorClicked);
    connect(page_, &BookPageWidget::layoutChanged, this, &EbookView::updatePageLabel);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(page_);

    // Contents panel (overlay, hidden until "Contents" is pressed).
    tocList_ = new QListWidget(this);
    tocList_->setVisible(false);
    tocList_->setFocusPolicy(Qt::NoFocus);
    connect(tocList_, &QListWidget::itemClicked, this, &EbookView::onTocActivated);

    // Auto-hiding top menu (overlay).
    menu_ = new QFrame(this);
    menu_->setVisible(false);
    menu_->setAutoFillBackground(true);
    menu_->setFrameShape(QFrame::StyledPanel);
    // Compact controls just for the reader's menu (overriding the app's roomier defaults), so the strip
    // reserved at the top of the page can be shorter - leaving more of it as reading margin.
    menu_->setStyleSheet(QStringLiteral(
        "QPushButton{min-height:20px;padding:2px 9px;font-size:12px;} QLabel{font-size:12px;}"));
    auto* bar = new QHBoxLayout(menu_);
    bar->setContentsMargins(8, 3, 8, 3);
    bar->setSpacing(5);
    auto* backBtn = new QPushButton(tr("‹ Back"), menu_);
    streamIssueBtn_ = new QPushButton(tr("Issue with Streaming"), menu_);
    // Drawn beside the label rather than typed into it: as ⚠ the mark came out of the colour emoji
    // font, which no stylesheet can tint — the same chip in the video player draws this one.
    streamIssueBtn_->setIcon(PlayerIcons::icon(PlayerIcons::Warning, 16,
                                               streamIssueBtn_->palette().color(QPalette::ButtonText)));
    streamIssueBtn_->setIconSize(QSize(16, 16));
    streamIssueBtn_->setToolTip(tr("Bad or wrong file? Try the next available source."));
    streamIssueBtn_->setVisible(false);
    auto* homeBtn  = new QPushButton(tr("Home"), menu_);
    auto* contents = new QPushButton(tr("Contents"), menu_);
    auto* smaller  = new QPushButton(tr("A−"), menu_);
    auto* bigger   = new QPushButton(tr("A+"), menu_);
    auto* prev     = new QPushButton(tr("‹ Prev"), menu_);
    auto* next     = new QPushButton(tr("Next ›"), menu_);
    pageLabel_ = new QLabel(menu_);
    pageLabel_->setAlignment(Qt::AlignCenter);
    for (QPushButton* b : { backBtn, streamIssueBtn_, homeBtn, contents, smaller, bigger, prev, next })
        b->setFocusPolicy(Qt::NoFocus); // keep arrow-key focus on the view, not a button

    connect(backBtn,  &QPushButton::clicked, this, &EbookView::backRequested);
    connect(homeBtn,  &QPushButton::clicked, this, &EbookView::homeRequested);
    connect(contents, &QPushButton::clicked, this, &EbookView::toggleContents);
    connect(smaller,  &QPushButton::clicked, this, &EbookView::smallerFont);
    connect(bigger,   &QPushButton::clicked, this, &EbookView::biggerFont);
    connect(prev,     &QPushButton::clicked, this, &EbookView::prevPage);
    connect(next,     &QPushButton::clicked, this, &EbookView::nextPage);
    connect(streamIssueBtn_, &QPushButton::clicked, this, &EbookView::streamIssueRequested);

    bar->addWidget(backBtn);
    bar->addWidget(streamIssueBtn_);
    bar->addWidget(homeBtn);
    bar->addWidget(contents);
    bar->addWidget(smaller);
    bar->addWidget(bigger);

    // Read aloud (issue #145), on the classic bar. Built ONLY when this build carries the Qt TextToSpeech
    // module AND the platform offers an engine: a control that cannot speak is worse than no control, and a
    // build without the module shows exactly the bar it always showed.
#ifdef EB_HAVE_TTS
    if (ReadAloudController::engineAvailable())
    {
        readAloud_  = new ReadAloudController(this, this);
        raBtn_      = new QPushButton(tr("Read aloud"), menu_);
        raPauseBtn_ = new QPushButton(tr("Pause"), menu_);
        raSpeedBtn_ = new QPushButton(QStringLiteral("1×"), menu_);
        raVoiceBtn_ = new QPushButton(tr("Voice"), menu_);
        for (QPushButton* b : { raBtn_, raPauseBtn_, raSpeedBtn_, raVoiceBtn_ })
        {
            b->setFocusPolicy(Qt::NoFocus);   // arrow keys stay with the view, like every other bar button
            bar->addWidget(b);
        }
        connect(raBtn_,      &QPushButton::clicked, this, &EbookView::toggleReadAloud);
        connect(raPauseBtn_, &QPushButton::clicked, this, &EbookView::readAloudTogglePause);
        connect(raSpeedBtn_, &QPushButton::clicked, this, &EbookView::readAloudCycleSpeed);
        connect(raVoiceBtn_, &QPushButton::clicked, this, &EbookView::readAloudCycleVoice);
        syncReadAloudButtons();
    }
#endif

    bar->addStretch(1);
    bar->addWidget(prev);
    bar->addWidget(pageLabel_, 1);
    bar->addWidget(next);

    menuTimer_ = new QTimer(this);
    menuTimer_->setSingleShot(true);
    connect(menuTimer_, &QTimer::timeout, this, &EbookView::hideMenuIfIdle);

    // Resizing changes the page geometry, so the book-wide page total has to be re-tallied; debounce it so a
    // drag doesn't re-paginate every chapter on every pixel.
    repagTimer_ = new QTimer(this);
    repagTimer_->setSingleShot(true);
    connect(repagTimer_, &QTimer::timeout, this, [this] { recomputeBookPages(); updatePageLabel(); });

    setFocusPolicy(Qt::StrongFocus);
}

bool EbookView::openBook(const QString& path, QString* error)
{
    if (readAloudActive()) toggleReadAloud();   // stop narrating the book we are leaving (issue #145)
    persist(); // save the book we're leaving, if any

    book_ = makeSource(path); // EPUB / MOBI / PDF, by file content
    if (!book_->open(path, error)) return false;

    tocList_->clear();
    for (const EpubTocEntry& e : book_->toc())
    {
        auto* item = new QListWidgetItem(e.title, tocList_);
        item->setData(Qt::UserRole, e.href);
    }
    tocList_->setVisible(false);

    restoreState(); // sets fontPt_, the chapter to resume at, and the resume offset/fraction
    // Apply the stored reader typography (font/size/spacing/margin/justify/theme) BEFORE the chapter is laid out
    // so the very first page renders in the reader's chosen look, not the default one (#135). fontPt_ is taken
    // from the same "ebook/fontSize" key restoreState already read, so the A+/A− stepper and the settings size
    // stay one value.
    const ReaderTypography::Resolved typo = ReaderTypography::resolve(Settings::readerTypography());
    fontPt_ = typo.sizePt;
    page_->setTypography(typo);
    loadChapter(chapter_ >= 0 ? chapter_ : 0);
    if (restorePos_ >= 0)        page_->scrollToTextPosition(restorePos_); // exact spot, size-independent
    else if (restoreFrac_ >= 0)  page_->setProgress(restoreFrac_);         // legacy save
    restorePos_ = -1; restoreFrac_ = -1.0;
    recomputeBookPages(); // tally the book's pages, then show "page x / y"
    updatePageLabel();
#ifdef EB_HAVE_TTS
    // Read aloud (issue #145): this book's stored speed and the voice, resolved NOW so the controls say what
    // pressing them would do rather than only finding out once narration starts.
    if (readAloud_) readAloud_->adoptBook();
#endif
    revealMenu();      // flash the controls so they're discoverable, then auto-hide
    setFocus();
    return true;
}

void EbookView::setStreamIssueVisible(bool on)
{
    streamVisible_ = on;
    if (streamIssueBtn_) streamIssueBtn_->setVisible(on && !hosted_); // hosted chrome surfaces this itself
}

void EbookView::restoreState()
{
    fontPt_ = qBound(8, store().value(QStringLiteral("ebook/fontSize"), 14).toInt(), 40);
    const QString k = bookKey(book_->sourcePath());
    chapter_ = store().value(k + QStringLiteral("chapter"), 0).toInt();
    if (chapter_ < 0 || chapter_ >= book_->chapterFiles().size()) chapter_ = 0;

    // Resume by document offset (stable across repagination). Older saves only have a page fraction, so
    // keep it as a fallback when there's no stored offset.
    const QVariant pos = store().value(k + QStringLiteral("pos"));
    restorePos_  = pos.isValid() ? pos.toInt() : -1;
    restoreFrac_ = restorePos_ < 0
        ? store().value(k + QStringLiteral("scroll"), -1.0).toDouble()
        : -1.0;
}

// posOverride (-1 = "wherever the page starts") is how read-aloud records the SPOKEN PARAGRAPH as the reading
// position (issue #145) without disturbing the page: a paragraph already on screen must not re-anchor the page
// under the reader's eye, but it is still the place to come back to. Everything else passes -1 and the stored
// position is the top of the page, exactly as before.
void EbookView::persist(int posOverride)
{
    if (!book_->isOpen() || chapter_ < 0) return;
    store().setValue(QStringLiteral("ebook/fontSize"), fontPt_);
    const QString k = bookKey(book_->sourcePath());
    store().setValue(k + QStringLiteral("chapter"), chapter_);
    store().setValue(k + QStringLiteral("pos"),
                     posOverride >= 0 ? posOverride : page_->topTextPosition()); // where in the text, not which page
    store().setValue(k + QStringLiteral("title"), book_->title());
    store().sync();
}

void EbookView::loadChapter(int index, bool toLast)
{
    const QStringList& files = book_->chapterFiles();
    if (files.isEmpty()) return;
    chapter_ = qBound(0, index, files.size() - 1);

    QFile f(files[chapter_]);
    QString html;
    if (f.open(QIODevice::ReadOnly)) { html = QString::fromUtf8(f.readAll()); f.close(); }
    page_->setContent(html, QFileInfo(files[chapter_]).absolutePath());
    if (toLast) page_->showLastPage(); else page_->showFirstPage();
    updatePageLabel();

    // Reflect the current chapter in the contents list (best-effort by file name).
    const QString href = QFileInfo(files[chapter_]).fileName();
    for (int i = 0; i < tocList_->count(); ++i)
        if (tocList_->item(i)->data(Qt::UserRole).toString().compare(href, Qt::CaseInsensitive) == 0)
        { tocList_->setCurrentRow(i); break; }

    persist();
}

void EbookView::nextPage()
{
    if (!book_->isOpen()) return;
    // While narrating, the page follows the narrator, so a page key is a PARAGRAPH move - the reader's twin of
    // #140's jump controls (issue #145). Paging by hand here would be undone by the very next utterance.
    if (readAloudActive()) { readAloudSkip(+1); return; }
    if (page_->pageForward())                             { /* moved within the chapter */ }
    else if (chapter_ < book_->chapterFiles().size() - 1)   loadChapter(chapter_ + 1);
    else                                                    return;
    updatePageLabel();
    accrueReadingProgress();
    persist();
}

// Consumption stats: accrue at the forward page/chapter advance only (NOT updatePageLabel, which also fires on
// font repagination that would inflate the global page). High-water keyed by the book path + title; the store
// ignores any revisit/regression. Factored out of nextPage() so a page turned BY THE NARRATOR (issue #145)
// accrues through exactly this reckoning - which is what #136's furthest-read carries - rather than a second
// one beside it that could drift.
void EbookView::accrueReadingProgress()
{
    if (!book_ || !book_->isOpen()) return;
    ConsumptionStats::addPagesRead(book_->sourcePath(), globalPage(), book_->title());
    emit readingProgress(book_->title(),
                         tr("Reading · p. %1 of %2").arg(globalPage()).arg(pageCount()));
}

void EbookView::prevPage()
{
    if (!book_->isOpen()) return;
    if (readAloudActive()) { readAloudSkip(-1); return; }   // paragraph back while narrating (issue #145)
    if (page_->pageBackward())   { /* moved within the chapter */ }
    else if (chapter_ > 0)         loadChapter(chapter_ - 1, /*toLast*/ true);
    else                           return;
    updatePageLabel();
    persist();
}

void EbookView::biggerFont()  { fontDelta(+2); }
void EbookView::smallerFont() { fontDelta(-2); }

// Change the reading font by dPt points, clamped to 8..40, keeping the reading position across the reflow.
// The whole book repaginates, so the book-wide "page x / y" total is re-tallied. (This is exactly what the
// old biggerFont/smallerFont did in ±2 steps — no pagination-logic change, just a parameterised delta.)
void EbookView::fontDelta(int dPt)
{
    if (dPt == 0) return;
    const int pos = page_->topTextPosition();
    const int prev = fontPt_;
    fontPt_ = qBound(8, fontPt_ + dPt, 40);
    if (fontPt_ == prev) return;      // already at the clamp — nothing repaginated
    page_->setFontPointSize(fontPt_);
    page_->scrollToTextPosition(pos); // stay on the same text after the reflow
    recomputeBookPages();             // the whole book just repaginated
    updatePageLabel();
    persist();
}

// Reader typography changed in Settings (issue #135): re-read the stored preferences and apply them live. Every
// knob (font family + size, line spacing, margin, justification, reading theme) reflows the page, so we capture
// the reading anchor (a document character offset, stable across repagination) BEFORE the apply and restore it
// AFTER — the reader stays on exactly the same words. fontPt_ mirrors the resolved size so the A+/A− stepper
// continues from there. The whole book repaginates, so the book-wide "page x / y" total is re-tallied, and the
// page-chrome background is set to match the theme so no gap flashes the old paper colour.
void EbookView::applyReaderTypography()
{
    if (!page_) return;
    const int pos = page_->topTextPosition();
    const ReaderTypography::Resolved typo = ReaderTypography::resolve(Settings::readerTypography());
    fontPt_ = typo.sizePt;
    page_->setTypography(typo);
    page_->scrollToTextPosition(pos);   // same words after the reflow (repagination honesty)

    // The chrome behind/around the page (visible in any gap during a resize) tracks the theme paper colour too.
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(typo.background));
    pal.setColor(QPalette::Base,   QColor(typo.background));
    setPalette(pal);
    setAutoFillBackground(true);

    recomputeBookPages();
    updatePageLabel();
    persist();
}

int EbookView::currentPage() const { return globalPage(); }

int EbookView::pageCount() const
{
    return totalPages_ > 0 ? totalPages_ : (page_ ? page_->pageCount() : 1);
}

QStringList EbookView::tocTitles() const
{
    QStringList out;
    if (book_) for (const EpubTocEntry& e : book_->toc()) out << e.title;
    return out;
}

void EbookView::gotoTocIndex(int i)
{
    if (!book_ || !book_->isOpen()) return;
    const QVector<EpubTocEntry> toc = book_->toc();
    if (i < 0 || i >= toc.size()) return;
    const int idx = book_->chapterIndexForHref(toc[i].href);
    if (idx >= 0) loadChapter(idx);
}

// ---- Bookmarks (issue #136): the anchor a book bookmark captures + restores -------------------------------
// The stable natural key is the source path — the same basis resume keys on (bookKey() hashes it), so a book's
// resume position and its bookmarks name one identity. Empty when no book is open.
QString EbookView::itemKey() const { return book_ ? book_->sourcePath() : QString(); }

// The current top character offset — the #135 repagination-stable anchor, so a bookmark taken at one font size
// lands on the same words at another.
int EbookView::textOffset() const { return page_ ? page_->topTextPosition() : 0; }

// Jump to a stored bookmark: load its chapter (spine) if we are not already on it, then scroll to the exact
// character offset. Mirrors loadChapter + the resume restore in openBook (scrollToTextPosition), then re-labels
// (which re-emits pageInfoChanged so the themed chrome mirrors the jump) and persists the new position.
void EbookView::gotoSpineOffset(int spine, int offset)
{
    if (!book_ || !book_->isOpen()) return;
    const int n = book_->chapterFiles().size();
    if (spine < 0 || spine >= n) return;
    if (spine != chapter_) loadChapter(spine);
    if (offset >= 0) page_->scrollToTextPosition(offset);
    updatePageLabel();   // re-emits pageInfoChanged() for the hosted chrome
    persist();
}

// Jump to a 0-based BOOK-WIDE page — the number the themed chrome's progress bar is a scale over, so a click
// on that bar lands where it points. chapterStart_ holds the cumulative page offset of every chapter, so the
// target picks its chapter by the last start at or below it; inside that chapter the page is addressed as a
// fraction, which is the one route BookPageWidget offers to a page index. Before the book-wide tally exists
// (recomputeBookPages has not run at this geometry yet) chapterStart_ is empty and pageCount() is the CURRENT
// chapter's count — so the same arithmetic falls back to paging within this chapter, which is exactly what the
// bar is drawing at that moment.
void EbookView::gotoPage(int page0)
{
    if (!book_ || !book_->isOpen() || !page_) return;
    const int target = qBound(0, page0, pageCount() - 1);

    int ch = chapter_;
    if (totalPages_ > 0)
        for (int i = 0; i < chapterStart_.size(); ++i)
        {
            if (chapterStart_[i] > target) break;
            ch = i;
        }
    if (ch != chapter_) loadChapter(ch);

    const int base  = (ch >= 0 && ch < chapterStart_.size() && totalPages_ > 0) ? chapterStart_[ch] : 0;
    const int pages = page_->pageCount();
    const int local = qBound(0, target - base, pages - 1);
    page_->setProgress(pages > 1 ? double(local) / double(pages - 1) : 0.0);

    updatePageLabel();   // re-emits pageInfoChanged() for the hosted chrome
    persist();
}

// Hosted mode: the themed ReaderChromeHost owns all chrome, so suppress our own widget menu, contents panel
// and stream-issue button, and stop the auto-hide timer. revealMenu() short-circuits while hosted so mouse
// movement / a top-band click never flashes the raster menu over the themed strips.
void EbookView::setHostedChrome(bool on)
{
    hosted_ = on;
    if (on)
    {
        if (menuTimer_) menuTimer_->stop();
        if (menu_)      menu_->setVisible(false);
        if (tocList_)   tocList_->setVisible(false);
        if (streamIssueBtn_) streamIssueBtn_->setVisible(false);
    }
    else if (streamIssueBtn_)
    {
        streamIssueBtn_->setVisible(streamVisible_); // classic mode: restore its remembered visibility
    }
}

void EbookView::toggleContents()
{
    layoutOverlays();
    tocList_->setVisible(!tocList_->isVisible());
    if (tocList_->isVisible()) { tocList_->raise(); revealMenu(); }
}

void EbookView::onTocActivated()
{
    QListWidgetItem* item = tocList_->currentItem();
    if (!item) return;
    const int idx = book_->chapterIndexForHref(item->data(Qt::UserRole).toString());
    if (idx >= 0) loadChapter(idx);
    tocList_->setVisible(false);
}

void EbookView::onAnchorClicked(const QString& href)
{
    const QUrl url(href);
    if (url.path().isEmpty()) return; // a within-chapter fragment - can't position to it in paged mode
    const QString file = QFileInfo(url.path()).fileName();
    const int idx = book_->chapterIndexForHref(file);
    if (idx >= 0) loadChapter(idx); // external / unmatched links are ignored rather than navigating away
}

void EbookView::revealMenu()
{
    if (hosted_) return; // the themed chrome owns reveal/hide; never flash the raster menu while hosted
    layoutOverlays();
    menu_->setVisible(true);
    menu_->raise();
    if (tocList_->isVisible()) tocList_->raise();
    menuTimer_->start(3500);
}

void EbookView::hideMenuIfIdle()
{
    // Keep the menu up while the pointer is over it (or the contents panel) so it's usable.
    if (menu_->underMouse() || (tocList_->isVisible() && tocList_->underMouse()))
    { menuTimer_->start(1500); return; }
    menu_->setVisible(false);
}

void EbookView::layoutOverlays()
{
    // Fixed height kept in lock-step with the page's top inset, so the menu sits exactly over the reserved
    // strip and never overlaps the text.
    menu_->setGeometry(0, 0, width(), kMenuHeight);
    tocList_->setGeometry(0, kMenuHeight, qMin(340, width() / 3), height() - kMenuHeight);
}

void EbookView::resizeEvent(QResizeEvent*)
{
    layoutOverlays();
    if (book_->isOpen()) repagTimer_->start(180); // re-tally the book's pages once the drag settles
}

void EbookView::recomputeBookPages()
{
    const QStringList& files = book_->chapterFiles();
    chapterStart_.resize(files.size());
    if (files.isEmpty() || page_->width() <= 0 || page_->height() <= 0) { totalPages_ = 0; return; }

    int acc = 0;
    for (int i = 0; i < files.size(); ++i)
    {
        chapterStart_[i] = acc;
        if (i == chapter_)
        {
            acc += page_->pageCount(); // current chapter is already laid out - reuse it
            continue;
        }
        QFile f(files[i]);
        QString html;
        if (f.open(QIODevice::ReadOnly)) { html = QString::fromUtf8(f.readAll()); f.close(); }
        acc += page_->countPages(html, QFileInfo(files[i]).absolutePath());
    }
    totalPages_ = acc;
}

int EbookView::globalPage() const
{
    const int base = (chapter_ >= 0 && chapter_ < chapterStart_.size()) ? chapterStart_[chapter_] : 0;
    return base + page_->currentPage() + 1;
}

void EbookView::updatePageLabel()
{
    if (!book_->isOpen()) { pageLabel_->clear(); page_->setFooter(QString()); return; }

    // Bottom-of-page footer: book-wide "page x / y" (falls back to the chapter's own count if the tally
    // isn't ready yet).
    const int total = totalPages_ > 0 ? totalPages_ : page_->pageCount();
    const int cur   = totalPages_ > 0 ? globalPage() : page_->currentPage() + 1;
    page_->setFooter(tr("%1 / %2").arg(cur).arg(total));

    // Menu label keeps the title + chapter context.
    pageLabel_->setText(tr("%1  —  Ch %2/%3")
                            .arg(book_->title())
                            .arg(chapter_ + 1).arg(book_->chapterFiles().size()));

    emit pageInfoChanged(); // hosted chrome mirrors page/chapter/font — refresh it on every label update
}

// ---- Read aloud (issue #145) ---------------------------------------------------------------------------------
// The commands both layouts fire. Every one is a no-op when read-aloud is unavailable, which is the state of a
// build without the Qt TextToSpeech module (the controller type does not even exist there) and of a platform
// with no speech engine. Nothing below reaches into narration's own decisions: what to speak and where that
// leaves the reader are ReadAloud's and ReadAloudController's, and this is the wiring between them and a bar.

bool EbookView::readAloudAvailable() const
{
#ifdef EB_HAVE_TTS
    return readAloud_ != nullptr;
#else
    return false;
#endif
}

bool EbookView::readAloudActive() const
{
#ifdef EB_HAVE_TTS
    return readAloud_ && readAloud_->active();
#else
    return false;
#endif
}

bool EbookView::readAloudPaused() const
{
#ifdef EB_HAVE_TTS
    return readAloud_ && readAloud_->paused();
#else
    return false;
#endif
}

void EbookView::toggleReadAloud()
{
#ifdef EB_HAVE_TTS
    if (readAloud_ && book_ && book_->isOpen()) readAloud_->toggle();
#endif
}

void EbookView::readAloudTogglePause()
{
#ifdef EB_HAVE_TTS
    if (readAloud_) readAloud_->togglePause();
#endif
}

void EbookView::readAloudSkip(int paragraphs)
{
#ifdef EB_HAVE_TTS
    if (readAloud_) readAloud_->skip(paragraphs);
#else
    Q_UNUSED(paragraphs);
#endif
}

double EbookView::readAloudSpeed() const
{
#ifdef EB_HAVE_TTS
    return readAloud_ ? readAloud_->speed() : 1.0;
#else
    return 1.0;
#endif
}

void EbookView::readAloudCycleSpeed()
{
#ifdef EB_HAVE_TTS
    if (readAloud_) readAloud_->cycleSpeed();
#endif
}

QString EbookView::readAloudVoiceName() const
{
#ifdef EB_HAVE_TTS
    return readAloud_ ? readAloud_->voiceNames().value(readAloud_->voiceIndex()) : QString();
#else
    return QString();
#endif
}

void EbookView::readAloudCycleVoice()
{
#ifdef EB_HAVE_TTS
    if (readAloud_) readAloud_->cycleVoice();
#endif
}

// The classic bar's labels ARE narration's state - there is no separate indicator, so a label that lies is the
// whole feedback gone. Re-run on every narration change.
void EbookView::syncReadAloudButtons()
{
#ifdef EB_HAVE_TTS
    if (!readAloud_ || !raBtn_) return;
    const bool on = readAloud_->active();
    raBtn_->setText(on ? tr("Stop") : tr("Read aloud"));
    raPauseBtn_->setText(readAloud_->paused() ? tr("Resume") : tr("Pause"));
    raPauseBtn_->setEnabled(on);
    raSpeedBtn_->setText(QString::number(readAloud_->speed(), 'g', 3) + QString(QChar(0x00D7)));
    const QString v = readAloud_->voiceNames().value(readAloud_->voiceIndex());
    raVoiceBtn_->setText(v.isEmpty() ? tr("Voice") : v);
#endif
}

// ---- ReadAloudTarget: what the controller is allowed to ask of the reader -------------------------------------

QString EbookView::raChapterText() const { return page_ ? page_->plainText() : QString(); }

int EbookView::raChapterCount() const { return book_ ? int(book_->chapterFiles().size()) : 0; }

bool EbookView::raGotoChapter(int index)
{
    if (!book_ || !book_->isOpen()) return false;
    if (index < 0 || index >= book_->chapterFiles().size()) return false;
    loadChapter(index);
    return true;
}

int EbookView::raCurrentOffset() const { return page_ ? page_->topTextPosition() : 0; }

// THE POSITION UNIFICATION (issue #145's third decision), in one function. The reader ARRIVES at the spoken
// paragraph: the page turns if the paragraph has left it, the paragraph is highlighted, a page that actually
// turned accrues through the SAME reckoning a hand-turned page does (so #136's furthest-read carries narration
// without knowing narration exists), and the position is persisted as the PARAGRAPH - not the top of the page -
// so stopping leaves you exactly where the narrator was, and a restart resumes there.
void EbookView::raShowSpoken(int start, int end)
{
    if (!page_) return;
    const int before = page_->topTextPosition();
    page_->ensurePosVisible(start);
    page_->setSpokenRange(start, end);
    if (page_->topTextPosition() > before) accrueReadingProgress();   // a page really turned, and forwards
    updatePageLabel();      // re-emits pageInfoChanged() so the themed chrome mirrors the move
    persist(start);         // the spoken paragraph IS the reading position
}

// Narration ended. The highlight goes; the POSITION stays exactly where it reached - there is no second
// bookmark to restore and nothing to put back.
void EbookView::raClearSpoken()
{
    if (page_) page_->setSpokenRange(-1, -1);
}

QString EbookView::raBookKey() const { return itemKey(); }

// The book's own dc:language is not reachable through EbookSource, and adding it would mean editing the format
// parsers - which issue #144 owns this cycle. The system locale stands in: it is right for the overwhelming
// majority of a library, and every installed voice is still one press of the Voice control away, so a book in
// another language is never stuck with the wrong narrator.
QString EbookView::raPreferredLanguage() const { return QLocale::system().name(); }

void EbookView::raNarrationChanged()
{
    syncReadAloudButtons();
    emit pageInfoChanged();   // the themed chrome mirrors narration through the refresh it mirrors paging with
}

void EbookView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Right: case Qt::Key_PageDown: case Qt::Key_Space: nextPage(); return;
    case Qt::Key_Left:  case Qt::Key_PageUp:                       prevPage(); return;
    case Qt::Key_Backspace: case Qt::Key_Escape:                   emit backRequested(); return;
    default: QWidget::keyPressEvent(e);
    }
}
