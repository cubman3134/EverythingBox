#include "ComicView.h"
#include "ComicInfo.h"       // #152: the archive's own ComicInfo.xml -> the reading direction
#include "ComicName.h"       // seriesKey(): the key a per-series direction override is stored under
#include "ComicPageOrder.h"
#include "RarComic.h"
#include "Tar.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ConsumptionStats.h"
#include "../core/PhotoLibrary.h"
#include "../core/SevenZip.h"
#include "../core/Settings.h"

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
    // #154: the scroll area and the (hidden) webtoon thumbnail rail go in as ONE row, so turning the rail on
    // narrows the page instead of covering it. Both surfaces and the five per-series buttons are built in
    // ComicViewModes.cpp.
    installModeSurfaces(v);
    v->addWidget(bar_);
    installModeControls();

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
        || ext == QStringLiteral("cb7") || ext == QStringLiteral("cbt")
        || ext == QStringLiteral("cbr");
}

// CBR (.cbr): a RAR of page images. RarComic.h owns the whole of it - the signature sniff that names RAR5
// rather than calling it corrupt, the ONE sequential pass that keeps a solid archive's decompression window
// intact, and the sentence for each way it can fail. All this branch does is order what came back, with the
// same orderPages() the CB7 and CBT branches use.
bool ComicView::loadCbrPages(const QString& path, QVector<QByteArray>& pages, QString* error)
{
    RarComic::Status st = RarComic::Status::Ok;
    const QVector<QPair<QString, QByteArray>> imgs = RarComic::imagePages(path, &st);
    if (st != RarComic::Status::Ok || imgs.isEmpty())
    {
        if (error) *error = RarComic::message(st == RarComic::Status::Ok ? RarComic::Status::NoPages : st);
        return false;
    }

    pages = orderPages(imgs);
    if (pages.isEmpty()) { if (error) *error = tr("Could not read the comic's pages."); return false; }
    return true;
}

// CB7 (.cb7): a 7-Zip of page images. The LZMA SDK behind SevenZip.h decodes into files, so extract the whole
// archive into an isolated per-open temp dir, read the image pages into memory, natural-sort them, and let the
// temp dir remove itself on the way out (QTemporaryDir auto-removes in its destructor) — nothing is left on disk
// once the pages are in RAM, so there is no scratch to clean up when the view later closes. A corrupt/empty/
// image-less archive returns a readable error, never a crash.
bool ComicView::loadCb7Pages(const QString& path, QVector<QByteArray>& pages, QString* error,
                             QByteArray* comicInfoXml)
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

    // The document, out of the extraction we already made (#152). Before the temp dir goes away with the
    // end of this function.
    if (comicInfoXml) *comicInfoXml = ComicInfo::xmlFromDirectory(tmp.path());

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
    QByteArray comicInfoXml;   // #152: the archive's own ComicInfo.xml, when it has one

    if (ext == QStringLiteral("cb7"))
    {
        // The .cb7 branch hands the document back out of the extraction it makes for the pages; the other
        // three are cheap enough to ask the seam for directly, below.
        if (!loadCb7Pages(path, pages, error, &comicInfoXml)) return false;
    }
    else if (ext == QStringLiteral("cbt"))
    {
        if (!loadCbtPages(path, pages, error)) return false;
    }
    else if (ext == QStringLiteral("cbr"))
    {
        if (!loadCbrPages(path, pages, error)) return false;
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

    // ---- WHICH WAY IT READS (issue #152) -----------------------------------------------------------------
    // The archive's ComicInfo.xml sets the DEFAULT; the user's per-series override beats it; with neither,
    // left to right, which is what every comic did before this existed. Read AFTER the pages, so a comic
    // that failed to open changes nothing about the one still on screen.
    if (ext != QStringLiteral("cb7")) comicInfoXml = ComicInfo::xmlFromArchive(path);
    ComicInfo::Info meta;
    if (!comicInfoXml.isEmpty())
    {
        bool wellFormed = true;
        const ComicInfo::Info parsed = ComicInfo::parse(comicInfoXml, &wellFormed);
        if (wellFormed) meta = parsed;   // a malformed document is ignored whole (ComicInfo.h)
    }
    // The settings are keyed by SERIES. #152 could only look one up for a comic whose document named a
    // series; #154's key (ComicRead::seriesKeyFor) falls back to the FILENAME, so chapter 2 of a webtoon
    // opens in the mode chapter 1 was set to. Both overrides are read under that one key — nothing had ever
    // written a direction override under a filename-derived key, because until now nothing wrote one at all.
    seriesKey_ = ComicRead::seriesKeyFor(meta.series, path);
    readDisplayOptions();
    mode_ = ComicRead::resolveMode(meta.direction,
                                   Settings::comicDirectionOverride(seriesKey_),
                                   Settings::comicDisplayOption(seriesKey_,
                                                                QString::fromLatin1(ComicRead::Opt::kMode)));
    rtl_ = ComicRead::isRtl(mode_);

    // Every page's own size, out of its HEADER (no decode): the webtoon strip is laid out from these and the
    // split decision is taken on them, so both are answerable for any page of a 300-page comic at any moment.
    pageSizes_.clear();
    pageSizes_.reserve(pages_.size());
    for (const QByteArray& bytes : pages_)
    {
        QBuffer buf;
        buf.setData(bytes);
        if (!buf.open(QIODevice::ReadOnly)) { pageSizes_.append(QSize()); continue; }
        pageSizes_.append(QImageReader(&buf).size());
    }
    stripCache_.clear();
    railCache_.clear();
    // Leaving photo mode. openFolder() clears pages_ on the way in, and this is the same door in the other
    // direction: MainWindow reuses ONE ComicView for both, so a comic opened after a photo folder was viewed
    // kept photoMode_ set and was read as that folder — pageTotal()/decodeAt() answer from photoFiles_, and
    // (issue #136) itemKey() reports no bookmark key for a file that is very much open.
    photoMode_ = false;
    photoFiles_.clear();

    int page = store().value(comicKey(path) + QStringLiteral("page"), 0).toInt();
    page = qBound(0, page, pages_.size() - 1);
    // #154: a webtoon's place is a page AND how far into it, because a page there is a screenful of a strip
    // rather than a thing you turn. A comic saved before this existed has no fraction and resumes at 0.0,
    // which is where it always resumed.
    resumeFraction_ = qBound(0.0, store().value(comicKey(path) + QStringLiteral("frac"), 0.0).toDouble(), 1.0);
    // #285: and a page that SPLITS is two screens, so which of the two was on screen is part of the place as
    // well. Read here, applied by showPage() below through ComicRead::resumeHalf, which honours it only while
    // this page still splits at this viewport in this mode. A comic saved before #285 has no half: the key is
    // absent, the value is kNoStoredHalf, and the page opens on the half it opens on today — the same promise
    // the fraction one comment up makes about a comic saved before IT existed.
    bool halfOk = false;
    const int savedHalf = store().value(comicKey(path) + QStringLiteral("half"),
                                        ComicRead::kNoStoredHalf).toInt(&halfOk);
    resumeHalf_ = halfOk ? savedHalf : ComicRead::kNoStoredHalf;
    fit_ = true;
    zoom_ = 1.0;
    current_ = page;
    const double storedFraction = resumeFraction_;
    applyMode();
    showPage(page);   // the page's own bookkeeping (stats, presence, the end-of-chapter hint) is in here...
    if (mode_ == ComicRead::Mode::Webtoon) scrollToPosition(page, storedFraction);   // ... and this is the spot
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
    rtl_ = false;            // a folder of photographs has no reading direction (#152)
    photoFiles_ = files;
    pages_.clear();          // drop any comic archive state; photo pages come from photoFiles_
    path_ = folder;
    fit_ = true;
    zoom_ = 1.0;
    // ... and no reading MODE either (#154). A photo folder is not a series, carries no per-file resume and
    // accrues no reading stats, so it goes back to the plain paged view with every per-series option off —
    // the same door openComic() closes in the other direction.
    mode_ = ComicRead::Mode::PagedLtr;
    split_ = ComicRead::Split::Auto;
    filter_ = ComicRead::Filter::None;
    crop_ = false;
    railOn_ = false;
    seriesKey_.clear();
    half_ = -1;
    resumeHalf_ = ComicRead::kNoStoredHalf;   // #285: a photo folder carries no resume, so it names no half
    pageSizes_.clear();
    stripCache_.clear();
    railCache_.clear();
    applyMode();

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
    // #154: in webtoon mode the place is (page, fraction into that page) — currentPosition() answers exactly
    // that, and answers (current_, 0.0) in every paged mode, so this is one write for both.
    int page = current_;
    double fraction = 0.0;
    currentPosition(&page, &fraction);
    store().setValue(k + QStringLiteral("page"), page);
    store().setValue(k + QStringLiteral("frac"), fraction);
    // #285: ... and WHICH HALF of that page, when what is in front of the reader is a split spread. The key is
    // written only while a half is actually on screen and REMOVED otherwise, because it describes the position
    // being saved right now: a stale half left behind by an earlier close would later be read back against a
    // page the reader was never on a half of. Absent is the pre-#285 state and reads as ComicRead's "none".
    if (half_ >= 0) store().setValue(k + QStringLiteral("half"), half_);
    else            store().remove(k + QStringLiteral("half"));
    store().setValue(k + QStringLiteral("title"), QFileInfo(path_).fileName());
    store().sync();
}

void ComicView::showPage(int index, int dir)
{
    if (index < 0 || index >= pageTotal()) return;
    current_ = index;
    if (mode_ == ComicRead::Mode::Webtoon && !photoMode_)
    {
        // In a strip a page is a PLACE, not a picture to swap in: the whole chapter is already laid out, so
        // "show page N" means scroll to where page N starts.
        half_ = -1;
        scrollToPosition(index, 0.0);
    }
    else
    {
        // #154: a page that splits arrives as one of its halves — the second one when the reader is walking
        // backwards, so a spread read in reverse shows the half it showed last.
        // #285: ... unless this is the comic being REOPENED, in which case the half the resume recorded is the
        // half to come back to. resumeHalf_ holds the stored value until the reader navigates (nextPage,
        // prevPage and gotoPage each spend it), and resumeHalf honours it only on a page that still splits —
        // so a wider window, a Never override or webtoon all fall straight back to entryHalf's answer.
        half_ = ComicRead::resumeHalf(resumeHalf_, pageSplits(index), dir);
        image_ = preparedPage(index, half_);
        rescale();
        scroll_->verticalScrollBar()->setValue(0); // start each page at the top
    }
    updateLabel();
    // Consumption stats: high-water page read (revisits/backward turns don't accrue). Path-derived key + title,
    // 1-based page to match the reader's own labels; the store owns the accrual math. Comics only — a photo
    // folder isn't a "book being read", so it does not accrue reading stats (issue #102).
    if (!photoMode_)
        ConsumptionStats::addPagesRead(path_, current_ + 1, QFileInfo(path_).fileName());
    // #Discord: the same edge, and the same photoMode_ exclusion above - a photo folder is not a book.
    if (!photoMode_)
        emit readingProgress(QFileInfo(path_).completeBaseName(),
                             tr("Reading · p. %1 of %2").arg(current_ + 1).arg(pageTotal()));
    emit pageInfoChanged();                     // mirror the page move into the themed chrome
    // The end of the chapter is the one moment worth telling the user another one is waiting. MainWindow owns
    // the once-per-open throttling — this fires every time the last page comes up, including on the way back.
    if (!photoMode_ && comicPastEnd(current_, pageTotal())) emit reachedLastPage();
}

// Show two pages at once (like an open book) when it makes sense: only in fit-width mode, for portrait
// pages, and on a wide landscape viewport - never on a narrow / phone-width window (or for a page that's
// itself a landscape spread).
bool ComicView::spreadActive() const
{
    // #154: neither a split page nor a strip is ever ALSO an open book — the first is already two pages of
    // one image and the second has no page boundaries at all.
    if (half_ >= 0 || mode_ == ComicRead::Mode::Webtoon) return false;
    return fit_ && twoUp_ && current_ + 1 < pageTotal();
}

void ComicView::rescale()
{
    if (mode_ == ComicRead::Mode::Webtoon && !photoMode_) return;   // #154: the strip paints itself
    if (image_.isNull()) { imageLabel_->clear(); return; }
    const int vw = qMax(64, scroll_->viewport()->width() - 4); // fill the viewport width (scale up or down)
    const int vh = qMax(64, scroll_->viewport()->height());

    // Photo mode never pairs pages book-style — two-up is a comic notion (issue #102). A SPLIT page is never
    // paired either (#154): it is a double spread already, and pairing half of one with half of the next is
    // the one arrangement that is wrong in every reading direction.
    twoUp_ = !photoMode_ && half_ < 0 && twoUpEnabled_ && fit_
             && image_.height() > image_.width() && vw > vh && vw >= 800;

    if (twoUp_ && current_ + 1 < pages_.size())
    {
        // The facing page goes through the SAME display pipeline as the page beside it — a spread with one
        // page cropped and tinted and the other not would be a worse artefact than either setting.
        const QImage right = preparedPage(current_ + 1, -1);
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
            // WHICH SIDE EACH PAGE IS DRAWN ON is the whole of what right-to-left changes about the render
            // (issue #152): in a manga spread the EARLIER page is on the right, because that is the one the
            // eye reaches first. Nothing about the page order, the resume position or the page numbering
            // moves — only the two rectangles.
            if (rtl_)
            {
                p.drawImage(QRect(x0, y0, lw, outH), r);            // next page on the left
                p.drawImage(QRect(x0 + lw + g, y0, rw, outH), l);   // first page on the right
            }
            else
            {
                p.drawImage(QRect(x0, y0, lw, outH), l);            // first page on the left
                p.drawImage(QRect(x0 + lw + g, y0, rw, outH), r);   // next page on the right
            }
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
    // #154: a strip has no page boundaries, so the page number in it is where you ARE rather than what is on
    // screen — the label says "approx" outright instead of implying a precision the mode does not have. A
    // split page says which half you are on, because "page 7" twice in a row is the one thing that looks
    // like a stuck reader.
    const QString where =
        mode_ == ComicRead::Mode::Webtoon
            ? tr("Page %1 / %2 (approx)").arg(current_ + 1).arg(pages_.size())
        : half_ >= 0
            ? tr("Page %1 / %2 · half %3 of 2").arg(current_ + 1).arg(pages_.size()).arg(half_ + 1)
        : spreadActive()
            ? tr("Pages %1–%2 / %3").arg(current_ + 1).arg(current_ + 2).arg(pages_.size())
            : tr("Page %1 / %2").arg(current_ + 1).arg(pages_.size());
    pageLabel_->setText(QFileInfo(path_).fileName() + QStringLiteral("  —  ") + where);
}

void ComicView::nextPage()
{
    // The press that used to be a silent no-op: at the last page it reports the boundary instead. Nothing is
    // opened here, and nothing is judged here either — MainWindow owns the crossing, knows whether a next
    // chapter exists, and is the only place that can say "That's the last chapter." when one does not. So the
    // report is unconditional; a comic with no run there is answered with the same silence as before.
    // #154: a page that SPLIT is two screens, and the first press over it moves between them. It changes no
    // page, reports no boundary and writes no resume — the reader has not left this page yet.
    // #285: the reader is moving, so the resume's half is spent — from here on the direction decides again.
    resumeHalf_ = ComicRead::kNoStoredHalf;
    if (ComicRead::stepStaysInPage(half_, +1))
    {
        half_ = 1;
        image_ = preparedPage(current_, half_);
        rescale();
        updateLabel();
        emit pageInfoChanged();
        return;
    }
    if (comicPastEnd(current_, pageTotal()))
    {
        emit chapterAdvanceRequested(+1);
        return;
    }
    showPage(qMin(current_ + (spreadActive() ? 2 : 1), pageTotal() - 1)); // advance a whole spread in book mode
}
void ComicView::prevPage()
{
    resumeHalf_ = ComicRead::kNoStoredHalf;   // #285: as in nextPage() — a move spends the resume's half
    if (ComicRead::stepStaysInPage(half_, -1))
    {
        half_ = 0;
        image_ = preparedPage(current_, half_);
        rescale();
        updateLabel();
        emit pageInfoChanged();
        return;
    }
    if (comicBeforeStart(current_))
    {
        emit chapterAdvanceRequested(-1);   // likewise unconditional — see nextPage()
        return;
    }
    showPage(qMax(current_ - ((fit_ && twoUp_) ? 2 : 1), 0), -1);
}

void ComicView::zoomIn()  { fit_ = false; zoom_ = qMin(5.0, zoom_ * 1.2); rescale(); emit pageInfoChanged(); }
void ComicView::zoomOut() { fit_ = false; zoom_ = qMax(0.2, zoom_ / 1.2); rescale(); emit pageInfoChanged(); }
void ComicView::fitWidth() { fit_ = true; rescale(); updateLabel(); emit pageInfoChanged(); }

void ComicView::keyPressEvent(QKeyEvent* e)
{
    // #154, THE RAIL FIRST. While the thumbnail rail holds the cursor every arrow belongs to it — that is
    // what makes it a nav zone on a D-pad without being a themed NavGraph zone (which would have put it on
    // one layout only; see ComicView.h). LEFT/RIGHT hand the arrows back to the strip.
    //
    // ESCAPE AND BACK ARE DELIBERATELY NOT LISTED. Under the themed chrome those two never reach this widget
    // at all — the host takes them as "leave the reader" before the reader sees a thing — so consuming them
    // here would make one press mean "leave the rail" on the classic layout and "leave the comic" on the
    // themed one. A rail you get out of with a different key depending on the chrome is worse than a rail
    // you always get out of the same way.
    if (railFocus_ && railWidget_)
    {
        switch (e->key())
        {
        case Qt::Key_Up:   railStep(-1); return;
        case Qt::Key_Down: railStep(+1); return;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Select:
            scrollToPosition(railIndex_, 0.0);
            updateLabel();
            emit pageInfoChanged();
            return;
        case Qt::Key_Left: case Qt::Key_Right:
            setRailFocus(false);
            return;
        default: break;
        }
    }
    // #154, THE STRIP. Up/Down scroll it by a viewport fraction; Left/Right (below) still jump a whole page,
    // because in a chapter you are part-way through "the next page" is the only landmark there is.
    if (mode_ == ComicRead::Mode::Webtoon && !photoMode_)
    {
        switch (e->key())
        {
        case Qt::Key_Down: scrollByViewport(+1); return;
        case Qt::Key_Up:   scrollByViewport(-1); return;
        default: break;
        }
    }

    switch (e->key())
    {
    // THE ARROWS FOLLOW THE READING DIRECTION (issue #152); PageDown/PageUp and Space do NOT. Left and
    // right are SPATIAL — in a right-to-left comic the next page is to the left, and a reader that ignored
    // that would page backwards through every manga. PageDown is not spatial: it means "onward" in every
    // document anybody has ever scrolled, and flipping it would be a second, worse surprise.
    case Qt::Key_Right:  rtl_ ? prevPage() : nextPage(); return;
    case Qt::Key_Left:   rtl_ ? nextPage() : prevPage(); return;
    case Qt::Key_PageDown: case Qt::Key_Space:                     nextPage(); return;
    case Qt::Key_PageUp:                                           prevPage(); return;
    case Qt::Key_Plus:  case Qt::Key_Equal:                        zoomIn();   return;
    case Qt::Key_Minus:                                           zoomOut();  return;
    case Qt::Key_Backspace: case Qt::Key_Escape:                  emit backRequested(); return;
    default: QWidget::keyPressEvent(e);
    }
}

void ComicView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // #154: a strip is laid out for ONE viewport width, so a resize relays it out — and lands back on the
    // same (page, fraction), which is the whole reason the reading position is kept in those terms.
    if (mode_ == ComicRead::Mode::Webtoon && !photoMode_) { relayoutStrip(); updateLabel(); return; }
    // ... and a rotation can change whether the page in front of you is a double spread at all.
    // #285: through resumeHalf, because a comic REOPENED inside the themed host is laid out after it is shown
    // — the viewport the split is decided on only settles here — so the resume's half has to survive into this
    // recomputation or the restored spread would snap back to its first half. Once the reader has moved,
    // resumeHalf_ is "none" and this is entryHalf's answer exactly, as it was.
    const int wantHalf = ComicRead::resumeHalf(resumeHalf_, pageSplits(current_), +1);
    if ((wantHalf < 0) != (half_ < 0))
    {
        half_ = wantHalf;
        image_ = preparedPage(current_, half_);
    }
    if (fit_ && !image_.isNull()) { rescale(); updateLabel(); } // refit to the new width (may toggle the spread)
}

void ComicView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // When the reader is wrapped in the themed ReaderChromeHost, its scroll viewport only reaches full size
    // after it is shown (the host lays the widget out on show), so the rescale done at openComic() time saw a
    // stale/small width. Refit on the next event-loop turn once the geometry has settled, so the opening page
    // fits width and the two-up spread is evaluated against the real viewport. Idempotent (classic mode too).
    // #154: the strip has the same problem and the same answer — it is laid out for a width that is only
    // right once the host has finished, so it is laid out again on the next turn.
    if (mode_ == ComicRead::Mode::Webtoon && !photoMode_)
    {
        QTimer::singleShot(0, this, [this] { relayoutStrip(); updateLabel(); });
        return;
    }
    if (!image_.isNull())
        QTimer::singleShot(0, this, [this] { rescale(); updateLabel(); });
}
