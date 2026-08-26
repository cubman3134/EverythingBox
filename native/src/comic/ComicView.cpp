#include "ComicView.h"
#include "ComicPageOrder.h"
#include "Tar.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include "../core/PhotoLibrary.h"
#include "../core/SevenZip.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QSettings>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QCollator>
#include <QPixmap>
#include <QPainter>
#include <QColor>
#include <QImageReader>
#include <QBuffer>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>
#include <algorithm>
#include <cstring>

#include "miniz.h"

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static QString comicKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("comic/") + QString::fromLatin1(h) + QStringLiteral("/");
}

// Skips macOS resource-fork junk and the dotfiles some archives carry. The rule itself now lives in
// ComicPageOrder.h beside the page collation, because the LIBRARY scan (#134) has to pick page one out of a
// CBZ for its cover using the identical rule — a second copy is how a shelf comes to show a picture the
// reader never opens on.
static bool isImageName(const QString& name) { return ComicPages::isImageName(name); }

// Order a set of (inner-name -> encoded image bytes) entries into page sequence and drop the names, using the
// same numeric-aware collation the CBZ path uses (page1, page2, …, page10 — not page1, page10, page2). Shared
// by the CB7 and CBT readers. The ZIP reader kept its own inline collator for a while, so that path would stay
// byte-for-byte as it was; it now shares this one too, because an inline collator is the shape that is inert
// under the C locale (issue #205, NaturalOrder.h) and one page order is worth more than that provenance.
static QVector<QByteArray> orderPages(QVector<QPair<QString, QByteArray>> imgs)
{
    const QCollator coll = ComicPages::collator();
    std::sort(imgs.begin(), imgs.end(),
              [&coll](const QPair<QString, QByteArray>& a, const QPair<QString, QByteArray>& b) {
                  return ComicPages::lessThan(coll, a.first, b.first);
              });
    QVector<QByteArray> out;
    out.reserve(imgs.size());
    for (auto& e : imgs) out.append(e.second);
    return out;
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

    bar_ = new QWidget(this);
    auto* bar = new QHBoxLayout(bar_);
    bar->setContentsMargins(0, 0, 0, 0);
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
    v->addWidget(bar_);

    setFocusPolicy(Qt::StrongFocus);
}

// Hosted mode: the themed ReaderChromeHost owns all chrome, so hide our own bottom control bar (the themed
// bottom strip replaces it); classic mode restores it. No render/scroll change — the wrappers drive exactly
// what the bar's buttons already called.
void ComicView::setHostedChrome(bool on)
{
    hosted_ = on;
    if (bar_) bar_->setVisible(!on);
}

void ComicView::zoomDelta(int steps)
{
    for (int i = 0; i < steps; ++i)  zoomIn();
    for (int i = 0; i > steps; --i)  zoomOut();
}

// User two-up toggle: gate the (otherwise automatic) spread on this preference. Default on preserves the prior
// behaviour exactly; turning it off forces single-page even on a wide viewport.
void ComicView::setTwoUp(bool on)
{
    if (twoUpEnabled_ == on) return;
    twoUpEnabled_ = on;
    if (!image_.isNull()) { rescale(); updateLabel(); }
    emit pageInfoChanged();
}

bool ComicView::isComicFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QStringLiteral("cbz") || ext == QStringLiteral("zip")
        || ext == QStringLiteral("cb7") || ext == QStringLiteral("cbt");
}

// CB7 (.cb7): a 7-Zip of page images. The LZMA SDK behind SevenZip.h decodes into files, so extract the whole
// archive into an isolated per-open temp dir, read the image pages into memory, natural-sort them, and let the
// temp dir remove itself on the way out (QTemporaryDir auto-removes in its destructor) — nothing is left on disk
// once the pages are in RAM, so there is no scratch to clean up when the view later closes. A corrupt/empty/
// image-less archive returns a readable error, never a crash.
bool ComicView::loadCb7Pages(const QString& path, QVector<QByteArray>& pages, QString* error)
{
    QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/eb-cb7-XXXXXX"));
    if (!tmp.isValid())
    { if (error) *error = tr("Couldn't create a temporary folder to open this comic."); return false; }

    QString err7;
    if (!SevenZip::extractAllToDir(path, tmp.path(), &err7))
    { if (error) *error = tr("This isn't a readable comic archive (CB7)."); return false; }

    // Name each page by its path relative to the temp root so the natural sort sees the archive's own layout
    // (page1/page2/…), not the absolute temp path.
    QVector<QPair<QString, QByteArray>> imgs;
    const QDir base(tmp.path());
    QDirIterator it(tmp.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString abs = it.next();
        const QString rel = base.relativeFilePath(abs);
        if (!isImageName(rel)) continue;
        QFile pf(abs);
        if (!pf.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = pf.readAll();
        if (!bytes.isEmpty()) imgs.append({ rel, bytes });
    }
    if (imgs.isEmpty()) { if (error) *error = tr("No page images found in this comic."); return false; }

    pages = orderPages(imgs);
    if (pages.isEmpty()) { if (error) *error = tr("Could not read the comic's pages."); return false; }
    return true;
}

// CBT (.cbt): a tar of page images. Parsed in memory (like the CBZ path) by the pure Tar reader — collect the
// image members, natural-sort, feed the render path. A malformed tar degrades to whatever parsed, never throws.
bool ComicView::loadCbtPages(const QString& path, QVector<QByteArray>& pages, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    { if (error) *error = tr("This isn't a readable comic archive (CBT)."); return false; }
    const QByteArray tar = f.readAll();
    f.close();

    QVector<QPair<QString, QByteArray>> imgs;
    const QVector<Tar::TarEntry> entries = Tar::listEntries(tar);
    for (const Tar::TarEntry& e : entries)
    {
        if (!isImageName(e.name)) continue;
        const QByteArray bytes = Tar::extractEntry(tar, e);
        if (!bytes.isEmpty()) imgs.append({ e.name, bytes });
    }
    if (imgs.isEmpty()) { if (error) *error = tr("No page images found in this comic."); return false; }

    pages = orderPages(imgs);
    if (pages.isEmpty()) { if (error) *error = tr("Could not read the comic's pages."); return false; }
    return true;
}

bool ComicView::openComic(const QString& path, QString* error)
{
    persist(); // save the comic we're leaving

    const QString ext = QFileInfo(path).suffix().toLower();
    QVector<QByteArray> pages;

    if (ext == QStringLiteral("cb7"))
    {
        if (!loadCb7Pages(path, pages, error)) return false;
    }
    else if (ext == QStringLiteral("cbt"))
    {
        if (!loadCbtPages(path, pages, error)) return false;
    }
    else
    {
        // CBZ / ZIP — the original miniz path, unchanged.
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

        // The same collator the CB7/CBT path builds — via NaturalOrder, because building it inline
        // (`QCollator coll; coll.setNumericMode(true);`) is INERT under the C locale and silently orders
        // page10 before page2 there. See NaturalOrder.h (issue #205).
        const QCollator coll = ComicPages::collator();
        std::sort(imgs.begin(), imgs.end(),
                  [&coll](const QPair<QString, mz_uint>& a, const QPair<QString, mz_uint>& b) {
                      return coll.compare(a.first, b.first) < 0;
                  });

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
    }

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

// Photo mode: page through a folder of image FILES using the same render/page/zoom widget the comic reader
// already is. No archive layer — the "pages" are the folder's images (natural order), decoded lazily one at a
// time from disk (a photo folder can be far larger than a comic, so we do NOT slurp every file into memory the
// way the CBZ path does). Two-up book pairing is suppressed and per-file resume is skipped: both are comic
// notions the issue explicitly drops for photos.
bool ComicView::openFolder(const QString& folder, const QString& startFile, QString* error)
{
    persist(); // save any comic we're leaving

    const QStringList files = PhotoLibrary::imagesInFolder(folder);
    if (files.isEmpty())
    { if (error) *error = tr("No photos found in this folder."); return false; }

    photoMode_ = true;
    photoFiles_ = files;
    pages_.clear();          // drop any comic archive state; photo pages come from photoFiles_
    path_ = folder;
    fit_ = true;
    zoom_ = 1.0;

    int start = 0;
    if (!startFile.isEmpty())
    {
        const QString target = QFileInfo(startFile).absoluteFilePath();
        for (int i = 0; i < photoFiles_.size(); ++i)
            if (QFileInfo(photoFiles_[i]).absoluteFilePath() == target) { start = i; break; }
    }
    showPage(start);
    setFocus();
    return true;
}

// The number of pages regardless of source: a ZIP's decoded entries (comic) or the folder's image files (photo).
int ComicView::pageTotal() const { return photoMode_ ? int(photoFiles_.size()) : int(pages_.size()); }

// Decode one page/photo. Comic pages are already-in-memory encoded bytes; photos are read from disk through a
// QImageReader with auto-transform, which applies the file's EXIF orientation tag (phone photos are sideways
// without it). Returns a null QImage on any failure — callers already handle image_.isNull().
QImage ComicView::decodeAt(int index) const
{
    if (photoMode_)
    {
        if (index < 0 || index >= photoFiles_.size()) return QImage();
        QImageReader reader(photoFiles_[index]);
        reader.setAutoTransform(true); // honour EXIF orientation
        return reader.read();
    }
    if (index < 0 || index >= pages_.size()) return QImage();
    QImage img;
    img.loadFromData(pages_[index]);
    return img;
}

void ComicView::persist()
{
    if (photoMode_ || path_.isEmpty() || pages_.isEmpty()) return; // photos carry no per-file resume (issue #102)
    const QString k = comicKey(path_);
    store().setValue(k + QStringLiteral("page"), current_);
    store().setValue(k + QStringLiteral("title"), QFileInfo(path_).fileName());
    store().sync();
}

void ComicView::showPage(int index)
{
    if (index < 0 || index >= pageTotal()) return;
    current_ = index;
    image_ = decodeAt(index);
    rescale();
    updateLabel();
    scroll_->verticalScrollBar()->setValue(0); // start each page at the top
    // Consumption stats: high-water page read (revisits/backward turns don't accrue). Path-derived key + title,
    // 1-based page to match the reader's own labels; the store owns the accrual math. Comics only — a photo
    // folder isn't a "book being read", so it does not accrue reading stats (issue #102).
    if (!photoMode_)
        ConsumptionStats::addPagesRead(path_, current_ + 1, QFileInfo(path_).fileName());
    emit pageInfoChanged();                     // mirror the page move into the themed chrome
}

// Show two pages at once (like an open book) when it makes sense: only in fit-width mode, for portrait
// pages, and on a wide landscape viewport - never on a narrow / phone-width window (or for a page that's
// itself a landscape spread).
bool ComicView::spreadActive() const
{
    return fit_ && twoUp_ && current_ + 1 < pageTotal();
}

void ComicView::rescale()
{
    if (image_.isNull()) { imageLabel_->clear(); return; }
    const int vw = qMax(64, scroll_->viewport()->width() - 4); // fill the viewport width (scale up or down)
    const int vh = qMax(64, scroll_->viewport()->height());

    // Photo mode never pairs pages book-style — two-up is a comic notion (issue #102).
    twoUp_ = !photoMode_ && twoUpEnabled_ && fit_ && image_.height() > image_.width() && vw > vh && vw >= 800;

    if (twoUp_ && current_ + 1 < pages_.size())
    {
        QImage right;
        right.loadFromData(pages_[current_ + 1]);
        if (!right.isNull())
        {
            // Normalise both pages to a common height, lay them side by side, then fit the whole spread to the
            // viewport — to BOTH its width and its height. Fitting to width alone made two portrait pages a
            // spread taller than the viewport, so the bottom was cut off on the default two-up view; the helper
            // clamps to whichever dimension binds so the open book is fully visible.
            const int h = qMax(image_.height(), right.height());
            const QImage l = image_.height() == h ? image_ : image_.scaledToHeight(h, Qt::SmoothTransformation);
            const QImage r = right.height()  == h ? right  : right.scaledToHeight(h, Qt::SmoothTransformation);
            const int gap = 10;
            const double scale = comicSpreadScale(vw, vh, l.width() + gap + r.width(), h);
            const int outH = qMax(1, int(h * scale));
            const int lw = int(l.width() * scale), rw = int(r.width() * scale), g = int(gap * scale);
            const int x0 = qMax(0, (vw - (lw + g + rw)) / 2); // centre the spread horizontally

            // The spread now fits the viewport height too, so outH can be SHORTER than the viewport. Build the
            // canvas at least a viewport tall and blit the pages at a vertical offset, so the open book sits
            // centred rather than jammed to the top; sizing the label to the canvas keeps outH <= vh, so no
            // vertical scrollbar appears (and none steals width to re-introduce a cutoff) when the spread fits.
            const int canvasH = qMax(outH, vh);
            const int y0 = qMax(0, (vh - outH) / 2);          // centre the spread vertically

            QImage canvas(vw, canvasH, QImage::Format_RGB32);
            canvas.fill(QColor(0x15, 0x17, 0x1c));
            QPainter p(&canvas);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRect(x0, y0, lw, outH), l);            // first page on the left
            p.drawImage(QRect(x0 + lw + g, y0, rw, outH), r);   // next page on the right
            p.end();

            const QPixmap pm = QPixmap::fromImage(canvas);
            imageLabel_->setPixmap(pm);
            imageLabel_->resize(qMax(pm.width(), scroll_->viewport()->width()), pm.height());
            return;
        }
    }

    QPixmap pm = QPixmap::fromImage(image_);
    if (fit_) pm = pm.scaledToWidth(vw, Qt::SmoothTransformation);
    else      pm = pm.scaled(image_.size() * zoom_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imageLabel_->setPixmap(pm);
    // Let the label fill the viewport width so the page stays centred; height tracks the (tall) page so it scrolls.
    imageLabel_->resize(qMax(pm.width(), scroll_->viewport()->width()), pm.height());
}

void ComicView::updateLabel()
{
    if (photoMode_)
    {
        // Photos show the current file's own name + a 1-based position through the folder.
        const QString name = (current_ >= 0 && current_ < photoFiles_.size())
                                 ? QFileInfo(photoFiles_[current_]).fileName() : QString();
        pageLabel_->setText(name + QStringLiteral("  —  ")
                            + tr("Photo %1 / %2").arg(current_ + 1).arg(pageTotal()));
        return;
    }
    const QString where = spreadActive()
        ? tr("Pages %1–%2 / %3").arg(current_ + 1).arg(current_ + 2).arg(pages_.size())
        : tr("Page %1 / %2").arg(current_ + 1).arg(pages_.size());
    pageLabel_->setText(QFileInfo(path_).fileName() + QStringLiteral("  —  ") + where);
}

void ComicView::nextPage()
{
    if (current_ >= pageTotal() - 1) return;
    showPage(qMin(current_ + (spreadActive() ? 2 : 1), pageTotal() - 1)); // advance a whole spread in book mode
}
void ComicView::prevPage()
{
    if (current_ <= 0) return;
    showPage(qMax(current_ - ((fit_ && twoUp_) ? 2 : 1), 0));
}

void ComicView::zoomIn()  { fit_ = false; zoom_ = qMin(5.0, zoom_ * 1.2); rescale(); emit pageInfoChanged(); }
void ComicView::zoomOut() { fit_ = false; zoom_ = qMax(0.2, zoom_ / 1.2); rescale(); emit pageInfoChanged(); }
void ComicView::fitWidth() { fit_ = true; rescale(); updateLabel(); emit pageInfoChanged(); }

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
    if (fit_ && !image_.isNull()) { rescale(); updateLabel(); } // refit to the new width (may toggle the spread)
}

void ComicView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // When the reader is wrapped in the themed ReaderChromeHost, its scroll viewport only reaches full size
    // after it is shown (the host lays the widget out on show), so the rescale done at openComic() time saw a
    // stale/small width. Refit on the next event-loop turn once the geometry has settled, so the opening page
    // fits width and the two-up spread is evaluated against the real viewport. Idempotent (classic mode too).
    if (!image_.isNull())
        QTimer::singleShot(0, this, [this] { rescale(); updateLabel(); });
}
