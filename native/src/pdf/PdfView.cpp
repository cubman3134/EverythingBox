#include "PdfView.h"

#if !defined(Q_OS_ANDROID)
#include "../core/AppPaths.h"

#include <QPdfDocument>
#include <QPdfView>
#include <QPdfPageNavigator>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSettings>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

static QString pdfKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("pdf/") + QString::fromLatin1(h) + QStringLiteral("/");
}

PdfView::PdfView(QWidget* parent) : QWidget(parent)
{
    doc_ = new QPdfDocument(this);
    view_ = new QPdfView(this);
    view_->setDocument(doc_);
    view_->setPageMode(QPdfView::PageMode::SinglePage); // one page at a time, like the old reader
    view_->setZoomMode(QPdfView::ZoomMode::FitInView);

    bar_ = new QWidget(this);
    auto* bar = new QHBoxLayout(bar_);
    bar->setContentsMargins(0, 0, 0, 0);
    auto* backBtn = new QPushButton(tr("‹ Back"), this);
    streamIssueBtn_ = new QPushButton(tr("⚠ Issue with Streaming"), this);
    streamIssueBtn_->setToolTip(tr("Bad or wrong file? Try the next available source."));
    streamIssueBtn_->setVisible(false); // shown only for remote (Allarr) books
    connect(streamIssueBtn_, &QPushButton::clicked, this, &PdfView::streamIssueRequested);
    auto* homeBtn = new QPushButton(tr("Home"), this);
    auto* prev = new QPushButton(tr("‹ Prev"), this);
    auto* next = new QPushButton(tr("Next ›"), this);
    auto* zoomOutBtn = new QPushButton(tr("−"), this);
    auto* zoomInBtn = new QPushButton(tr("+"), this);
    auto* fit = new QPushButton(tr("Fit Width"), this);
    pageLabel_ = new QLabel(this);
    pageLabel_->setAlignment(Qt::AlignCenter);

    connect(backBtn, &QPushButton::clicked, this, &PdfView::backRequested);
    connect(homeBtn, &QPushButton::clicked, this, &PdfView::homeRequested);
    connect(prev, &QPushButton::clicked, this, &PdfView::prevPage);
    connect(next, &QPushButton::clicked, this, &PdfView::nextPage);
    connect(zoomOutBtn, &QPushButton::clicked, this, &PdfView::zoomOut);
    connect(zoomInBtn, &QPushButton::clicked, this, &PdfView::zoomIn);
    connect(fit, &QPushButton::clicked, this, &PdfView::fitWidth);
    connect(view_->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this, &PdfView::updateLabel);
    connect(doc_, &QPdfDocument::statusChanged, this, &PdfView::updateLabel);

    bar->addWidget(backBtn);
    bar->addWidget(streamIssueBtn_);
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
    v->addWidget(view_, 1);
    v->addWidget(bar_);

    setFocusPolicy(Qt::StrongFocus);
}

// Hosted mode: the themed ReaderChromeHost owns all chrome, so hide our own bottom control bar (the themed
// bottom strip replaces it) and stop surfacing the stream-issue button; classic mode restores it. No render or
// page-navigation logic changes — the wrappers below drive exactly what the bar's buttons already called.
void PdfView::setHostedChrome(bool on)
{
    hosted_ = on;
    if (bar_) bar_->setVisible(!on);
    if (streamIssueBtn_) streamIssueBtn_->setVisible(on ? false : streamVisible_);
}

int PdfView::currentPage() const { return view_->pageNavigator()->currentPage() + 1; } // 1-based
int PdfView::pageCount()  const { return qMax(1, doc_->pageCount()); }

void PdfView::zoomDelta(int steps)
{
    for (int i = 0; i < steps; ++i)  zoomIn();
    for (int i = 0; i > steps; --i)  zoomOut();
}

bool PdfView::openPdf(const QString& path, QString* error)
{
    persist(); // save the file we're leaving

    doc_->load(path);
    if (doc_->status() != QPdfDocument::Status::Ready)
    {
        if (error)
            *error = (doc_->error() == QPdfDocument::Error::IncorrectPassword)
                         ? tr("This PDF is password-protected.")
                         : tr("Could not open the PDF.");
        return false;
    }
    path_ = path;

    // Resume on the last page read.
    int page = store().value(pdfKey(path) + QStringLiteral("page"), 0).toInt();
    page = qBound(0, page, qMax(0, doc_->pageCount() - 1));
    view_->pageNavigator()->jump(page, QPointF{});
    view_->setFocus();
    updateLabel();
    return true;
}

void PdfView::setStreamIssueVisible(bool on)
{
    streamVisible_ = on; // remembered so leaving hosted mode restores it
    if (streamIssueBtn_) streamIssueBtn_->setVisible(on && !hosted_); // hosted chrome suppresses the raster button
}

void PdfView::persist()
{
    if (path_.isEmpty() || doc_->status() != QPdfDocument::Status::Ready) return;
    const QString k = pdfKey(path_);
    store().setValue(k + QStringLiteral("page"), view_->pageNavigator()->currentPage());
    store().setValue(k + QStringLiteral("title"), QFileInfo(path_).fileName());
    store().sync();
}

void PdfView::nextPage()
{
    const int p = view_->pageNavigator()->currentPage();
    if (p < doc_->pageCount() - 1) view_->pageNavigator()->jump(p + 1, QPointF{});
}

void PdfView::prevPage()
{
    const int p = view_->pageNavigator()->currentPage();
    if (p > 0) view_->pageNavigator()->jump(p - 1, QPointF{});
}

void PdfView::zoomIn()
{
    zoom_ = qMin(5.0, zoom_ * 1.2);
    view_->setZoomMode(QPdfView::ZoomMode::Custom);
    view_->setZoomFactor(zoom_);
    emit pageInfoChanged();
}

void PdfView::zoomOut()
{
    zoom_ = qMax(0.2, zoom_ / 1.2);
    view_->setZoomMode(QPdfView::ZoomMode::Custom);
    view_->setZoomFactor(zoom_);
    emit pageInfoChanged();
}

void PdfView::fitWidth()
{
    view_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    emit pageInfoChanged();
}

void PdfView::updateLabel()
{
    if (doc_->status() != QPdfDocument::Status::Ready) { pageLabel_->clear(); return; }
    pageLabel_->setText(tr("%1  —  Page %2 / %3")
                            .arg(QFileInfo(path_).fileName())
                            .arg(view_->pageNavigator()->currentPage() + 1)
                            .arg(doc_->pageCount()));
    emit pageInfoChanged(); // mirror page moves (buttons + raw keys) into the themed chrome
}

void PdfView::keyPressEvent(QKeyEvent* e)
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

#else // Q_OS_ANDROID -----------------------------------------------------------------------------------
// Qt's PDF module (QtPdf/PDFium) isn't shipped for Android in the open-source packages, so the PDF reader
// is a stub here. The class still exists (so MainWindow/MediaPane link unchanged); EPUB, comics, video,
// audio and emulation are unaffected.
#include <QLabel>
#include <QVBoxLayout>
#include <QKeyEvent>

PdfView::PdfView(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    auto* msg = new QLabel(tr("PDF viewing isn't available on this platform."), this);
    msg->setAlignment(Qt::AlignCenter);
    msg->setWordWrap(true);
    v->addWidget(msg);
    setFocusPolicy(Qt::StrongFocus);
}

bool PdfView::openPdf(const QString&, QString* error)
{
    if (error) *error = tr("PDF viewing isn't available on Android.");
    return false;
}

void PdfView::persist() {}
void PdfView::setStreamIssueVisible(bool) {}
void PdfView::setHostedChrome(bool on) { hosted_ = on; } // no bar to hide in the stub
int  PdfView::currentPage() const { return 1; }
int  PdfView::pageCount()  const { return 1; }
void PdfView::zoomDelta(int) {}
void PdfView::nextPage() {}
void PdfView::prevPage() {}
void PdfView::zoomIn() {}
void PdfView::zoomOut() {}
void PdfView::fitWidth() {}
void PdfView::updateLabel() {}

void PdfView::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Escape) { emit backRequested(); return; }
    QWidget::keyPressEvent(e);
}
#endif
