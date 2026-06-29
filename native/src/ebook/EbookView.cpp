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
    page_ = 0;
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

void BookPageWidget::setCurrentPage(int p)
{
    page_ = qBound(0, p, pageCount_ - 1);
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

void BookPageWidget::relayout()
{
    const qreal w = qMax(1.0, qreal(width())  - 2 * sideMargin_);
    const qreal h = qMax(1.0, qreal(height()) - topMargin_ - botMargin_);
    doc_->setPageSize(QSizeF(w, h));        // paginate by line into h-tall pages (no line is ever split)
    pageCount_ = qMax(1, doc_->pageCount());
    page_ = qBound(0, page_, pageCount_ - 1);
    emit layoutChanged();
}

int BookPageWidget::topTextPosition() const
{
    if (!doc_) return 0;
    const qreal pageH = doc_->pageSize().height();
    // The character at the top-left of the current page, in document offset terms.
    return qMax(0, doc_->documentLayout()->hitTest(QPointF(1.0, page_ * pageH + 1.0), Qt::FuzzyHit));
}

int BookPageWidget::pageForTextPosition(int pos) const
{
    const qreal pageH = doc_->pageSize().height();
    if (!doc_ || pageH <= 0) return 0;

    const QTextBlock block = doc_->findBlock(pos);
    if (!block.isValid()) return 0;

    // Document-space y of that character: the block's top, plus the offset of its line within the block.
    qreal y = doc_->documentLayout()->blockBoundingRect(block).top();
    if (QTextLayout* layout = block.layout(); layout && layout->lineCount() > 0)
    {
        const QTextLine line = layout->lineForTextPosition(qMax(0, pos - block.position()));
        if (line.isValid()) y += line.y();
    }
    return qBound(0, int(y / pageH), pageCount_ - 1);
}

void BookPageWidget::scrollToTextPosition(int pos)
{
    setCurrentPage(pageForTextPosition(pos));
}

int BookPageWidget::countPages(const QString& html, const QString& baseDir) const
{
    // Lay the chapter out in a throwaway document with the live view's exact font and page geometry, so its
    // page count matches what this widget would show - without touching the on-screen document.
    BookDocument d;
    d.setDocumentMargin(0);
    d.setDefaultStyleSheet(doc_->defaultStyleSheet());
    d.setBaseUrl(QUrl::fromLocalFile(baseDir + QStringLiteral("/")));
    d.setHtml(html);
    d.setDefaultFont(doc_->defaultFont());
    d.setPageSize(doc_->pageSize());
    return qMax(1, d.pageCount());
}

void BookPageWidget::resizeEvent(QResizeEvent*)
{
    const int pos = topTextPosition(); // anchor to the text before the geometry (and page count) changes
    relayout();
    scrollToTextPosition(pos);          // ...then land on whatever page now holds it
    update();
}

void BookPageWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (!doc_) return;

    const qreal pageW = doc_->pageSize().width();
    const qreal pageH = doc_->pageSize().height();

    p.save();
    // Clip to the page's content rectangle so a line belonging to the next page can never paint into our
    // margins - belt-and-braces on top of QTextDocument's line-clean pagination.
    p.setClipRect(QRectF(sideMargin_, topMargin_, pageW, pageH));
    p.translate(sideMargin_, topMargin_);
    p.translate(0, -page_ * pageH);        // bring this page's slice up to the top margin

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = palette();               // text drawn in QPalette::Text
    ctx.clip = QRectF(0, page_ * pageH, pageW, pageH);
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
    const qreal pageH = doc_->pageSize().height();
    const QPointF docPos(pos.x() - sideMargin_, pos.y() - topMargin_ + page_ * pageH);
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
    if (!page_->atLast())                                      page_->setCurrentPage(page_->currentPage() + 1);
    else if (chapter_ < book_->chapterFiles().size() - 1)      loadChapter(chapter_ + 1);
    else                                                       return;
    updatePageLabel();
    persist();
}

void EbookView::prevPage()
{
    if (!book_->isOpen()) return;
    if (!page_->atFirst())     page_->setCurrentPage(page_->currentPage() - 1);
    else if (chapter_ > 0)     loadChapter(chapter_ - 1, /*toLast*/ true);
    else                       return;
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
