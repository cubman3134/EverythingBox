#include "PdfView.h"

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
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/goliath.ini"),
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

    auto* bar = new QHBoxLayout();
    auto* prev = new QPushButton(tr("‹ Prev"), this);
    auto* next = new QPushButton(tr("Next ›"), this);
    auto* zoomOutBtn = new QPushButton(tr("−"), this);
    auto* zoomInBtn = new QPushButton(tr("+"), this);
    auto* fit = new QPushButton(tr("Fit Width"), this);
    pageLabel_ = new QLabel(this);
    pageLabel_->setAlignment(Qt::AlignCenter);

    connect(prev, &QPushButton::clicked, this, &PdfView::prevPage);
    connect(next, &QPushButton::clicked, this, &PdfView::nextPage);
    connect(zoomOutBtn, &QPushButton::clicked, this, &PdfView::zoomOut);
    connect(zoomInBtn, &QPushButton::clicked, this, &PdfView::zoomIn);
    connect(fit, &QPushButton::clicked, this, &PdfView::fitWidth);
    connect(view_->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this, &PdfView::updateLabel);
    connect(doc_, &QPdfDocument::statusChanged, this, &PdfView::updateLabel);

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
    v->addLayout(bar);

    setFocusPolicy(Qt::StrongFocus);
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
}

void PdfView::zoomOut()
{
    zoom_ = qMax(0.2, zoom_ / 1.2);
    view_->setZoomMode(QPdfView::ZoomMode::Custom);
    view_->setZoomFactor(zoom_);
}

void PdfView::fitWidth()
{
    view_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
}

void PdfView::updateLabel()
{
    if (doc_->status() != QPdfDocument::Status::Ready) { pageLabel_->clear(); return; }
    pageLabel_->setText(tr("%1  —  Page %2 / %3")
                            .arg(QFileInfo(path_).fileName())
                            .arg(view_->pageNavigator()->currentPage() + 1)
                            .arg(doc_->pageCount()));
}

void PdfView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Right: case Qt::Key_PageDown: case Qt::Key_Space: nextPage(); return;
    case Qt::Key_Left:  case Qt::Key_PageUp:                       prevPage(); return;
    case Qt::Key_Plus:  case Qt::Key_Equal:                        zoomIn();   return;
    case Qt::Key_Minus:                                           zoomOut();  return;
    default: QWidget::keyPressEvent(e);
    }
}
