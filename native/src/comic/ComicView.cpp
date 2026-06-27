#include "ComicView.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QSettings>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QCollator>
#include <QPixmap>
#include <algorithm>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

static QString comicKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("comic/") + QString::fromLatin1(h) + QStringLiteral("/");
}

static bool isImageName(const QString& name)
{
    const QString lower = name.toLower();
    // Skip macOS resource-fork junk and dotfiles that some archives carry.
    const QString base = QFileInfo(name).fileName();
    if (name.contains(QStringLiteral("__MACOSX")) || base.startsWith(QLatin1Char('.'))) return false;
    for (const char* ext : { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".avif" })
        if (lower.endsWith(QLatin1String(ext))) return true;
    return false;
}

ComicView::ComicView(QWidget* parent) : QWidget(parent)
{
    scroll_ = new QScrollArea(this);
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setStyleSheet(QStringLiteral("QScrollArea{background:#15171c;border:none;}"));
    imageLabel_ = new QLabel(scroll_);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setStyleSheet(QStringLiteral("background:#15171c;"));
    scroll_->setWidget(imageLabel_);

    auto* bar = new QHBoxLayout();
    auto* backBtn = new QPushButton(tr("‹ Back"), this);
    auto* homeBtn = new QPushButton(tr("Home"), this);
    auto* prev = new QPushButton(tr("‹ Prev"), this);
    auto* next = new QPushButton(tr("Next ›"), this);
    auto* zoomOutBtn = new QPushButton(tr("−"), this);
    auto* zoomInBtn = new QPushButton(tr("+"), this);
    auto* fit = new QPushButton(tr("Fit Width"), this);
    pageLabel_ = new QLabel(this);
    pageLabel_->setAlignment(Qt::AlignCenter);

    connect(backBtn, &QPushButton::clicked, this, &ComicView::backRequested);
    connect(homeBtn, &QPushButton::clicked, this, &ComicView::homeRequested);
    connect(prev, &QPushButton::clicked, this, &ComicView::prevPage);
    connect(next, &QPushButton::clicked, this, &ComicView::nextPage);
    connect(zoomOutBtn, &QPushButton::clicked, this, &ComicView::zoomOut);
    connect(zoomInBtn, &QPushButton::clicked, this, &ComicView::zoomIn);
    connect(fit, &QPushButton::clicked, this, &ComicView::fitWidth);

    bar->addWidget(backBtn);
    bar->addWidget(homeBtn);
    bar->addWidget(zoomOutBtn);
    bar->addWidget(zoomInBtn);
    bar->addWidget(fit);
    bar->addStretch(1);
    bar->addWidget(prev);
    bar->addWidget(pageLabel_, 1);
    bar->addWidget(next);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(scroll_, 1);
    v->addLayout(bar);

    setFocusPolicy(Qt::StrongFocus);
}

bool ComicView::isComicFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QStringLiteral("cbz") || ext == QStringLiteral("zip");
}

bool ComicView::openComic(const QString& path, QString* error)
{
    persist(); // save the comic we're leaving

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path.toUtf8().constData(), 0))
    { if (error) *error = tr("This isn't a readable comic archive (CBZ/ZIP)."); return false; }

    // Collect image entries, sorted in natural page order (page1, page2, …, page10 - not page1, page10, page2).
    QVector<QPair<QString, mz_uint>> imgs;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i)
    {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const QString name = QString::fromUtf8(st.m_filename);
        if (isImageName(name)) imgs.append({ name, i });
    }
    if (imgs.isEmpty()) { mz_zip_reader_end(&zip); if (error) *error = tr("No page images found in this comic."); return false; }

    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(imgs.begin(), imgs.end(),
              [&coll](const QPair<QString, mz_uint>& a, const QPair<QString, mz_uint>& b) {
                  return coll.compare(a.first, b.first) < 0;
              });

    QVector<QByteArray> pages;
    pages.reserve(imgs.size());
    for (const auto& e : imgs)
    {
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, e.second, &sz, 0);
        if (!p) continue;
        pages.append(QByteArray(static_cast<const char*>(p), int(sz)));
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    if (pages.isEmpty()) { if (error) *error = tr("Could not read the comic's pages."); return false; }

    pages_ = pages;
    path_ = path;

    int page = store().value(comicKey(path) + QStringLiteral("page"), 0).toInt();
    page = qBound(0, page, pages_.size() - 1);
    fit_ = true;
    zoom_ = 1.0;
    showPage(page);
    setFocus();
    return true;
}

void ComicView::persist()
{
    if (path_.isEmpty() || pages_.isEmpty()) return;
    const QString k = comicKey(path_);
    store().setValue(k + QStringLiteral("page"), current_);
    store().setValue(k + QStringLiteral("title"), QFileInfo(path_).fileName());
    store().sync();
}

void ComicView::showPage(int index)
{
    if (index < 0 || index >= pages_.size()) return;
    current_ = index;
    image_.loadFromData(pages_[index]);
    rescale();
    updateLabel();
    scroll_->verticalScrollBar()->setValue(0); // start each page at the top
}

void ComicView::rescale()
{
    if (image_.isNull()) { imageLabel_->clear(); return; }
    QPixmap pm = QPixmap::fromImage(image_);
    if (fit_)
    {
        const int w = qMax(64, scroll_->viewport()->width() - 4); // fill the viewport width (scale up or down)
        pm = pm.scaledToWidth(w, Qt::SmoothTransformation);
    }
    else
    {
        pm = pm.scaled(image_.size() * zoom_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    imageLabel_->setPixmap(pm);
    // Let the label fill the viewport width so the page stays centred; height tracks the (tall) page so it scrolls.
    imageLabel_->resize(qMax(pm.width(), scroll_->viewport()->width()), pm.height());
}

void ComicView::updateLabel()
{
    pageLabel_->setText(tr("%1  —  Page %2 / %3")
                            .arg(QFileInfo(path_).fileName())
                            .arg(current_ + 1)
                            .arg(pages_.size()));
}

void ComicView::nextPage() { if (current_ < pages_.size() - 1) showPage(current_ + 1); }
void ComicView::prevPage() { if (current_ > 0) showPage(current_ - 1); }

void ComicView::zoomIn()  { fit_ = false; zoom_ = qMin(5.0, zoom_ * 1.2); rescale(); }
void ComicView::zoomOut() { fit_ = false; zoom_ = qMax(0.2, zoom_ / 1.2); rescale(); }
void ComicView::fitWidth() { fit_ = true; rescale(); }

void ComicView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Right: case Qt::Key_PageDown: case Qt::Key_Space: nextPage(); return;
    case Qt::Key_Left:  case Qt::Key_PageUp:                       prevPage(); return;
    case Qt::Key_Plus:  case Qt::Key_Equal:                        zoomIn();   return;
    case Qt::Key_Minus:                                           zoomOut();  return;
    case Qt::Key_Backspace: case Qt::Key_Escape:                  emit backRequested(); return;
    default: QWidget::keyPressEvent(e);
    }
}

void ComicView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (fit_ && !image_.isNull()) rescale(); // refit to the new width
}
