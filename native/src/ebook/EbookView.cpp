#include "EbookView.h"
#include "EpubBook.h"
#include "MobiBook.h"
#include "PdfTextBook.h"
#include "../core/AppPaths.h"

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
#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QKeyEvent>
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
    doc_->setHtml(html);
    QFont f = doc_->defaultFont();
    f.setPointSize(fontPt_);
    doc_->setDefaultFont(f);
    topPos_ = 0;
    relayout();
    update();
}

void BookPageWidget::setFontPointSize(int pt)
{
    fontPt_ = pt;
    QFont f = doc_->defaultFont();
    f.setPointSize(pt);
    doc_->setDefaultFont(f);
    relayout();
    update();
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
void BookPageWidget::relayout()
{
    doc_->setTextWidth(qMax(1.0, qreal(width()) - 2 * sideMargin_));
    rebuildLines();
    buildPageTops();
    snapTopToLine();
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

void BookPageWidget::snapTopToLine()
{
    topPos_ = lines_[lineIndexForPos(topPos_)].pos;
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
    topPos_ = lines_[lineIndexForPos(pos)].pos; // the page starts exactly at this line
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

    const qreal contentW = doc_->textWidth();
    const int startLine = lineIndexForPos(topPos_);
    const int endLine = lastFittingLine(startLine);
    const qreal y0 = lines_[startLine].y;
    const qreal slice = lines_[endLine].y + lines_[endLine].h - y0; // height of the whole-line page

    p.save();
    // Clip to exactly the lines on this page so neither a partial next line nor the margins get painted.
    p.setClipRect(QRectF(sideMargin_, topMargin_, contentW, slice));
    p.translate(sideMargin_, topMargin_ - y0); // map this page's first line to the top margin

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = palette();                  // text drawn in QPalette::Text
    ctx.clip = QRectF(0, y0, contentW, slice);
    doc_->documentLayout()->draw(&p, ctx);
    p.restore();

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

void BookPageWidget::mousePressEvent(QMouseEvent* e)
{
    const QPoint pos = e->pos();

    // An in-book hyperlink (footnote / cross-reference) takes priority over the page-turn zones.
    const qreal y0 = lines_.isEmpty() ? 0.0 : lines_[lineIndexForPos(topPos_)].y;
    const QPointF docPos(pos.x() - sideMargin_, pos.y() - topMargin_ + y0);
    const QString href = doc_->documentLayout()->anchorAt(docPos);
    if (!href.isEmpty()) { emit anchorClicked(href); return; }

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
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
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
    auto* bar = new QHBoxLayout(menu_);
    bar->setContentsMargins(8, 6, 8, 6);
    auto* backBtn = new QPushButton(tr("‹ Back"), menu_);
    streamIssueBtn_ = new QPushButton(tr("⚠ Issue with Streaming"), menu_);
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
    page_->setFontPointSize(fontPt_);
    loadChapter(chapter_ >= 0 ? chapter_ : 0);
    if (restorePos_ >= 0)        page_->scrollToTextPosition(restorePos_); // exact spot, size-independent
    else if (restoreFrac_ >= 0)  page_->setProgress(restoreFrac_);         // legacy save
    restorePos_ = -1; restoreFrac_ = -1.0;
    recomputeBookPages(); // tally the book's pages, then show "page x / y"
    updatePageLabel();
    revealMenu();      // flash the controls so they're discoverable, then auto-hide
    setFocus();
    return true;
}

void EbookView::setStreamIssueVisible(bool on)
{
    streamVisible_ = on;
    if (streamIssueBtn_) streamIssueBtn_->setVisible(on);
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

void EbookView::persist()
{
    if (!book_->isOpen() || chapter_ < 0) return;
    store().setValue(QStringLiteral("ebook/fontSize"), fontPt_);
    const QString k = bookKey(book_->sourcePath());
    store().setValue(k + QStringLiteral("chapter"), chapter_);
    store().setValue(k + QStringLiteral("pos"), page_->topTextPosition()); // where in the text, not which page
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
    if (page_->pageForward())                             { /* moved within the chapter */ }
    else if (chapter_ < book_->chapterFiles().size() - 1)   loadChapter(chapter_ + 1);
    else                                                    return;
    updatePageLabel();
    persist();
}

void EbookView::prevPage()
{
    if (!book_->isOpen()) return;
    if (page_->pageBackward())   { /* moved within the chapter */ }
    else if (chapter_ > 0)         loadChapter(chapter_ - 1, /*toLast*/ true);
    else                           return;
    updatePageLabel();
    persist();
}

void EbookView::biggerFont()
{
    const int pos = page_->topTextPosition();
    fontPt_ = qMin(40, fontPt_ + 2);
    page_->setFontPointSize(fontPt_);
    page_->scrollToTextPosition(pos); // stay on the same text after the reflow
    recomputeBookPages();             // the whole book just repaginated
    updatePageLabel();
    persist();
}

void EbookView::smallerFont()
{
    const int pos = page_->topTextPosition();
    fontPt_ = qMax(8, fontPt_ - 2);
    page_->setFontPointSize(fontPt_);
    page_->scrollToTextPosition(pos);
    recomputeBookPages();
    updatePageLabel();
    persist();
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
