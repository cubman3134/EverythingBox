#include "EbookView.h"
#include "EpubBook.h"
#include "MobiBook.h"
#include "PdfTextBook.h"
#include "../core/AppPaths.h"

#include <QFile>
#include <QTextBrowser>
#include <QListWidget>
#include <QLabel>
#include <QSplitter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QKeyEvent>
#include <QSettings>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QUrl>
#include <QFileInfo>

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
// (works even when an Allarr book was cached under a ".epub" name); anything else is treated as EPUB.
static std::unique_ptr<EbookSource> makeSource(const QString& path)
{
    QFile f(path);
    QByteArray head;
    if (f.open(QIODevice::ReadOnly)) { head = f.read(68); f.close(); }
    const QByteArray sig = head.mid(60, 8);
    if (sig == QByteArray("BOOKMOBI") || sig == QByteArray("TEXtREAd"))
        return std::make_unique<MobiBook>();
    if (head.startsWith("%PDF-"))            // a text PDF read as a reflowable book (font sizing)
        return std::make_unique<PdfTextBook>();
    return std::make_unique<EpubBook>();
}

EbookView::EbookView(QWidget* parent) : QWidget(parent)
{
    book_ = std::make_unique<EpubBook>(); // a valid (closed) source until a book is opened
    browser_ = new QTextBrowser(this);
    browser_->setOpenLinks(false);   // we drive chapter flow ourselves; handle clicks via anchorClicked
    connect(browser_, &QTextBrowser::anchorClicked, this, &EbookView::onAnchorClicked);
    browser_->setFrameShape(QFrame::NoFrame);
    browser_->document()->setDefaultStyleSheet(QStringLiteral("body { margin: 28px; } img { max-width: 100%; }"));

    tocList_ = new QListWidget(this);
    tocList_->setVisible(false); // hidden until "Contents" is pressed
    connect(tocList_, &QListWidget::itemActivated, this, &EbookView::onTocActivated);
    connect(tocList_, &QListWidget::itemClicked, this, &EbookView::onTocActivated);

    split_ = new QSplitter(Qt::Horizontal, this);
    split_->addWidget(tocList_);
    split_->addWidget(browser_);
    split_->setStretchFactor(1, 1);
    split_->setSizes({ 240, 800 });

    auto* bar = new QHBoxLayout();
    auto* backBtn = new QPushButton(tr("‹ Back"), this);
    streamIssueBtn_ = new QPushButton(tr("⚠ Issue with Streaming"), this);
    streamIssueBtn_->setToolTip(tr("Bad or wrong file? Try the next available source."));
    streamIssueBtn_->setVisible(false); // shown only for remote (Allarr) books
    connect(streamIssueBtn_, &QPushButton::clicked, this, &EbookView::streamIssueRequested);
    auto* homeBtn = new QPushButton(tr("Home"), this);
    auto* contents = new QPushButton(tr("Contents"), this);
    auto* prev = new QPushButton(tr("‹ Prev"), this);
    auto* next = new QPushButton(tr("Next ›"), this);
    auto* smaller = new QPushButton(tr("A−"), this);
    auto* bigger = new QPushButton(tr("A+"), this);
    pageLabel_ = new QLabel(this);
    pageLabel_->setAlignment(Qt::AlignCenter);

    connect(backBtn, &QPushButton::clicked, this, &EbookView::backRequested);
    connect(homeBtn, &QPushButton::clicked, this, &EbookView::homeRequested);
    connect(contents, &QPushButton::clicked, this, &EbookView::toggleContents);
    connect(prev, &QPushButton::clicked, this, &EbookView::prevPage);
    connect(next, &QPushButton::clicked, this, &EbookView::nextPage);
    connect(smaller, &QPushButton::clicked, this, &EbookView::smallerFont);
    connect(bigger, &QPushButton::clicked, this, &EbookView::biggerFont);
    connect(browser_->verticalScrollBar(), &QScrollBar::valueChanged, this, &EbookView::updatePageLabel);

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

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(split_, 1);
    v->addLayout(bar);

    setFocusPolicy(Qt::StrongFocus);
}

bool EbookView::openBook(const QString& path, QString* error)
{
    persist(); // save the book we're leaving, if any

    book_ = makeSource(path); // EPUB or MOBI, by file content
    if (!book_->open(path, error)) return false;

    tocList_->clear();
    for (const EpubTocEntry& e : book_->toc())
    {
        auto* item = new QListWidgetItem(e.title, tocList_);
        item->setData(Qt::UserRole, e.href);
    }
    tocList_->setVisible(false);

    restoreState(); // sets fontPt_ and the chapter to resume at
    loadChapter(chapter_ >= 0 ? chapter_ : 0);
    browser_->setFocus();
    return true;
}

void EbookView::setStreamIssueVisible(bool on) { if (streamIssueBtn_) streamIssueBtn_->setVisible(on); }

void EbookView::restoreState()
{
    fontPt_ = store().value(QStringLiteral("ebook/fontSize"), 14).toInt();
    fontPt_ = qBound(8, fontPt_, 40);
    chapter_ = store().value(bookKey(book_->sourcePath()) + QStringLiteral("chapter"), 0).toInt();
    if (chapter_ < 0 || chapter_ >= book_->chapterFiles().size()) chapter_ = 0;
}

void EbookView::persist()
{
    if (!book_->isOpen() || chapter_ < 0) return;
    store().setValue(QStringLiteral("ebook/fontSize"), fontPt_);
    const QString k = bookKey(book_->sourcePath());
    store().setValue(k + QStringLiteral("chapter"), chapter_);
    QScrollBar* sb = browser_->verticalScrollBar();
    const double frac = sb->maximum() > 0 ? double(sb->value()) / sb->maximum() : 0.0;
    store().setValue(k + QStringLiteral("scroll"), frac);
    store().setValue(k + QStringLiteral("title"), book_->title());
    store().sync();
}

void EbookView::loadChapter(int index, bool toBottom)
{
    const QStringList& files = book_->chapterFiles();
    if (files.isEmpty()) return;
    chapter_ = qBound(0, index, files.size() - 1);

    browser_->setSource(QUrl::fromLocalFile(files[chapter_]));
    applyFont();

    QScrollBar* sb = browser_->verticalScrollBar();
    sb->setValue(toBottom ? sb->maximum() : 0);
    updatePageLabel();

    // Reflect the current chapter in the contents list (best-effort by file name).
    const QString href = QFileInfo(files[chapter_]).fileName();
    for (int i = 0; i < tocList_->count(); ++i)
        if (tocList_->item(i)->data(Qt::UserRole).toString().compare(href, Qt::CaseInsensitive) == 0)
        { tocList_->setCurrentRow(i); break; }

    persist();
}

void EbookView::applyFont()
{
    QFont f = browser_->document()->defaultFont();
    f.setPointSize(fontPt_);
    browser_->document()->setDefaultFont(f);
}

void EbookView::nextPage()
{
    if (!book_->isOpen()) return;
    QScrollBar* sb = browser_->verticalScrollBar();
    if (sb->value() < sb->maximum())
        sb->setValue(qMin(sb->maximum(), sb->value() + sb->pageStep()));
    else if (chapter_ < book_->chapterFiles().size() - 1)
        loadChapter(chapter_ + 1);
}

void EbookView::prevPage()
{
    if (!book_->isOpen()) return;
    QScrollBar* sb = browser_->verticalScrollBar();
    if (sb->value() > 0)
        sb->setValue(qMax(0, sb->value() - sb->pageStep()));
    else if (chapter_ > 0)
        loadChapter(chapter_ - 1, /*toBottom*/ true);
}

void EbookView::biggerFont()  { fontPt_ = qMin(40, fontPt_ + 2); applyFont(); updatePageLabel(); persist(); }
void EbookView::smallerFont() { fontPt_ = qMax(8,  fontPt_ - 2); applyFont(); updatePageLabel(); persist(); }

void EbookView::toggleContents()
{
    tocList_->setVisible(!tocList_->isVisible());
}

void EbookView::onTocActivated()
{
    QListWidgetItem* item = tocList_->currentItem();
    if (!item) return;
    const int idx = book_->chapterIndexForHref(item->data(Qt::UserRole).toString());
    if (idx >= 0) loadChapter(idx);
}

void EbookView::onAnchorClicked(const QUrl& url)
{
    // A fragment with no file part points within the current chapter - just scroll to it.
    if (url.path().isEmpty() && url.hasFragment())
    {
        browser_->scrollToAnchor(url.fragment());
        return;
    }

    // Otherwise resolve the target file to a spine chapter and jump there (then to its anchor, if any).
    const QString file = QFileInfo(url.path()).fileName();
    const int idx = book_->chapterIndexForHref(file);
    if (idx < 0) return; // external or unmatched link - ignore rather than navigate away

    if (idx != chapter_) loadChapter(idx); // already on it? just move to the anchor below
    if (url.hasFragment())
        browser_->scrollToAnchor(url.fragment());
}

void EbookView::updatePageLabel()
{
    if (!book_->isOpen()) { pageLabel_->clear(); return; }
    QScrollBar* sb = browser_->verticalScrollBar();
    const int step = qMax(1, sb->pageStep());
    const int pages = sb->maximum() / step + 1;
    const int page = sb->value() / step + 1;
    pageLabel_->setText(tr("%1  —  Chapter %2/%3 · Page %4/%5")
                            .arg(book_->title())
                            .arg(chapter_ + 1).arg(book_->chapterFiles().size())
                            .arg(qMin(page, pages)).arg(pages));
}

void EbookView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Right: case Qt::Key_PageDown: case Qt::Key_Space: nextPage(); return;
    case Qt::Key_Left:  case Qt::Key_PageUp:                       prevPage(); return;
    case Qt::Key_Backspace: case Qt::Key_Escape:                  emit backRequested(); return;
    default: QWidget::keyPressEvent(e);
    }
}
