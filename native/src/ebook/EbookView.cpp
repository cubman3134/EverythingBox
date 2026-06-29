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

void BookPageWidget::relayout()
{
    const qreal w = qMax(1.0, qreal(width())  - 2 * margin_);
    const qreal h = qMax(1.0, qreal(height()) - 2 * margin_);
    doc_->setPageSize(QSizeF(w, h));        // paginate by line into h-tall pages (no line is ever split)
    pageCount_ = qMax(1, doc_->pageCount());
    page_ = qBound(0, page_, pageCount_ - 1);
    emit layoutChanged();
}

void BookPageWidget::resizeEvent(QResizeEvent*)
{
    relayout();
    update();
}

void BookPageWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    if (!doc_) return;

    const qreal pageW = doc_->pageSize().width();
    const qreal pageH = doc_->pageSize().height();
    // Clip to the page's content rectangle so a line belonging to the next page can never paint into our
    // bottom margin - belt-and-braces on top of QTextDocument's line-clean pagination.
    p.setClipRect(QRectF(margin_, margin_, pageW, pageH));
    p.translate(margin_, margin_);
    p.translate(0, -page_ * pageH);        // bring this page's slice up to the top margin

    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette = palette();               // text drawn in QPalette::Text
    ctx.clip = QRectF(0, page_ * pageH, pageW, pageH);
    doc_->documentLayout()->draw(&p, ctx);
}

void BookPageWidget::mousePressEvent(QMouseEvent* e)
{
    const QPoint pos = e->pos();

    // An in-book hyperlink (footnote / cross-reference) takes priority over the page-turn zones.
    const qreal pageH = doc_->pageSize().height();
    const QPointF docPos(pos.x() - margin_, pos.y() - margin_ + page_ * pageH);
    const QString href = doc_->documentLayout()->anchorAt(docPos);
    if (!href.isEmpty()) { emit anchorClicked(href); return; }

    const int topZone = qMax(64, height() / 8);
    if (pos.y() < topZone)             { emit menuRequested(); return; }
    if (pos.x() < width() / 2)           emit prevRequested();
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

    restoreState(); // sets fontPt_, the chapter to resume at, and restoreProgress_
    page_->setFontPointSize(fontPt_);
    loadChapter(chapter_ >= 0 ? chapter_ : 0);
    if (restoreProgress_ >= 0.0)
    {
        page_->setProgress(restoreProgress_); // resume the page within the chapter
        restoreProgress_ = -1.0;
        updatePageLabel();
        persist();
    }
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
    chapter_ = store().value(bookKey(book_->sourcePath()) + QStringLiteral("chapter"), 0).toInt();
    if (chapter_ < 0 || chapter_ >= book_->chapterFiles().size()) chapter_ = 0;
    restoreProgress_ = store().value(bookKey(book_->sourcePath()) + QStringLiteral("scroll"), 0.0).toDouble();
}

void EbookView::persist()
{
    if (!book_->isOpen() || chapter_ < 0) return;
    store().setValue(QStringLiteral("ebook/fontSize"), fontPt_);
    const QString k = bookKey(book_->sourcePath());
    store().setValue(k + QStringLiteral("chapter"), chapter_);
    store().setValue(k + QStringLiteral("scroll"), page_->progress());
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
    const double p = page_->progress();
    fontPt_ = qMin(40, fontPt_ + 2);
    page_->setFontPointSize(fontPt_);
    page_->setProgress(p); // stay roughly where you were after the reflow
    updatePageLabel();
    persist();
}

void EbookView::smallerFont()
{
    const double p = page_->progress();
    fontPt_ = qMax(8, fontPt_ - 2);
    page_->setFontPointSize(fontPt_);
    page_->setProgress(p);
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
    const int menuH = menu_->sizeHint().height();
    menu_->setGeometry(0, 0, width(), menuH > 0 ? menuH : 52);
    tocList_->setGeometry(0, menu_->height(), qMin(340, width() / 3), height() - menu_->height());
}

void EbookView::resizeEvent(QResizeEvent*)
{
    layoutOverlays();
}

void EbookView::updatePageLabel()
{
    if (!book_->isOpen()) { pageLabel_->clear(); return; }
    pageLabel_->setText(tr("%1  —  Ch %2/%3 · Page %4/%5")
                            .arg(book_->title())
                            .arg(chapter_ + 1).arg(book_->chapterFiles().size())
                            .arg(page_->currentPage() + 1).arg(page_->pageCount()));
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
