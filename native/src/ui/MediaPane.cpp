#include "MediaPane.h"
#include "../video/MpvWidget.h"
#include "../emu/RetroView.h"
#include "../ebook/EbookView.h"
#include "../pdf/PdfView.h"
#include "../comic/ComicView.h"

#include <QStackedWidget>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QFileInfo>

MediaPane::MediaPane(QWidget* parent) : QWidget(parent)
{
    // Top bar: title, play/pause, volume, close.
    titleLabel_ = new QLabel(tr("Empty"), this);
    titleLabel_->setStyleSheet(QStringLiteral("color:#e8e8e8;font-weight:bold;"));
    pauseBtn_ = new QPushButton(QStringLiteral("⏸"), this);
    pauseBtn_->setFixedWidth(40);
    pauseBtn_->setToolTip(tr("Pause / resume this pane"));
    volume_ = new QSlider(Qt::Horizontal, this);
    volume_->setRange(0, 200);
    volume_->setValue(volPct_);
    volume_->setFixedWidth(90);
    volume_->setToolTip(tr("Volume for this pane"));
    closeBtn_ = new QPushButton(QStringLiteral("✕"), this);
    closeBtn_->setFixedWidth(40);
    closeBtn_->setToolTip(tr("Close this pane"));

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(8, 4, 8, 4);
    bar->addWidget(titleLabel_, 1);
    bar->addWidget(pauseBtn_);
    bar->addWidget(volume_);
    bar->addWidget(closeBtn_);

    // The engines, each its own instance so two panes never share state.
    stack_ = new QStackedWidget(this);
    emptyPage_ = new QWidget(stack_);
    {
        auto* openHere = new QPushButton(tr("＋  Open here"), emptyPage_);
        openHere->setStyleSheet(QStringLiteral(
            "QPushButton{background:rgba(90,140,255,0.18);color:#cfe0ff;border:2px dashed #5A8CFF;"
            "border-radius:10px;padding:14px 22px;font-size:16px;}"
            "QPushButton:hover{background:rgba(90,140,255,0.30);}"));
        auto* v = new QVBoxLayout(emptyPage_);
        v->addStretch(1); v->addWidget(openHere, 0, Qt::AlignHCenter); v->addStretch(1);
        connect(openHere, &QPushButton::clicked, this, &MediaPane::openHereRequested);
    }
    player_ = new MpvWidget(stack_);
    retro_  = new RetroView(stack_);
    retro_->setThreaded(true); // a split pane: emulate on a worker thread so the other pane's video can't throttle it
    book_   = new EbookView(stack_);
    pdf_    = new PdfView(stack_);
    comic_  = new ComicView(stack_);
    stack_->addWidget(emptyPage_);
    stack_->addWidget(player_);
    stack_->addWidget(retro_);
    stack_->addWidget(book_);
    stack_->addWidget(pdf_);
    stack_->addWidget(comic_);
    stack_->setCurrentWidget(emptyPage_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addLayout(bar);
    root->addWidget(stack_, 1);

    connect(closeBtn_, &QPushButton::clicked, this, &MediaPane::closeRequested);
    connect(pauseBtn_, &QPushButton::clicked, this, &MediaPane::togglePause);
    connect(volume_, &QSlider::valueChanged, this, [this](int v) { volPct_ = v; applyVolume(); emit focusRequested(); });
    // A reader's own Back/Home closes this pane (the pane, not the whole app).
    connect(book_,  &EbookView::backRequested, this, &MediaPane::closeRequested);
    connect(book_,  &EbookView::homeRequested, this, &MediaPane::closeRequested);
    connect(pdf_,   &PdfView::backRequested,   this, &MediaPane::closeRequested);
    connect(pdf_,   &PdfView::homeRequested,   this, &MediaPane::closeRequested);
    connect(comic_, &ComicView::backRequested, this, &MediaPane::closeRequested);
    connect(comic_, &ComicView::homeRequested, this, &MediaPane::closeRequested);

    // Any click inside the pane focuses it (so input routes here).
    installEventFilter(this);
    stack_->installEventFilter(this);
    player_->installEventFilter(this);
    retro_->installEventFilter(this);

    setStyleSheet(QStringLiteral("MediaPane{background:#0e1014;}"));
    setFocused(false);
}

void MediaPane::showView(QWidget* w, const QString& title, bool hasAudio)
{
    // Leaving the previous content: stop/persist it so two engines don't run needlessly.
    if (kind_ == Video && w != player_) player_->stop();
    if (kind_ == Game  && w != retro_)  retro_->stop();
    if (kind_ == Document)
    {
        if (w != book_)  book_->persist();
        if (w != pdf_)   pdf_->persist();
        if (w != comic_) comic_->persist();
    }
    titleLabel_->setText(title.isEmpty() ? tr("Untitled") : title);
    pauseBtn_->setVisible(w == player_ || w == retro_);
    volume_->setVisible(hasAudio);
    paused_ = false;
    pauseBtn_->setText(QStringLiteral("⏸"));
    stack_->setCurrentWidget(w);
}

void MediaPane::openVideo(const QString& url, const QString& title)
{
    kind_ = Video;
    showView(player_, title.isEmpty() ? QFileInfo(url).fileName() : title, /*hasAudio*/ true);
    player_->setVolume(volPct_);
    player_->play(url);
    emit focusRequested();
}

void MediaPane::openGame(const QString& corePath, const QString& romPath, const QString& coreName)
{
    kind_ = Game;
    showView(retro_, QFileInfo(romPath).completeBaseName(), /*hasAudio*/ true);
    retro_->setVolume(volPct_ / 100.0);
    QString err;
    retro_->openGame(corePath, romPath, coreName, &err);
    retro_->setInputActive(focused_);
    emit focusRequested();
}

void MediaPane::openBook(const QString& path)
{
    kind_ = Document;
    QString err;
    if (!book_->openBook(path, &err)) return;
    showView(book_, QFileInfo(path).completeBaseName(), /*hasAudio*/ false);
    emit focusRequested();
}

void MediaPane::openPdf(const QString& path)
{
    kind_ = Document;
    QString err;
    if (!pdf_->openPdf(path, &err)) return;
    showView(pdf_, QFileInfo(path).completeBaseName(), /*hasAudio*/ false);
    emit focusRequested();
}

void MediaPane::openComic(const QString& path)
{
    kind_ = Document;
    QString err;
    if (!comic_->openComic(path, &err)) return;
    showView(comic_, QFileInfo(path).completeBaseName(), /*hasAudio*/ false);
    emit focusRequested();
}

void MediaPane::clear()
{
    player_->stop();
    retro_->stop();
    book_->persist(); pdf_->persist(); comic_->persist();
    kind_ = None;
    titleLabel_->setText(tr("Empty"));
    pauseBtn_->setVisible(false);
    volume_->setVisible(false);
    stack_->setCurrentWidget(emptyPage_);
}

void MediaPane::setFocused(bool on)
{
    focused_ = on;
    if (kind_ == Game) retro_->setInputActive(on);
    setStyleSheet(on ? QStringLiteral("MediaPane{background:#0e1014;border:2px solid #5A8CFF;}")
                     : QStringLiteral("MediaPane{background:#0e1014;border:2px solid #23262e;}"));
    if (on)
    {
        // Give the active content Qt keyboard focus so keys reach it.
        if (kind_ == Game) retro_->setFocus();
        else if (kind_ == Document) stack_->currentWidget()->setFocus();
    }
}

void MediaPane::applyVolume()
{
    if (kind_ == Video) player_->setVolume(volPct_);
    else if (kind_ == Game) retro_->setVolume(volPct_ / 100.0);
}

void MediaPane::togglePause()
{
    paused_ = !paused_;
    if (kind_ == Video) player_->setPaused(paused_);
    else if (kind_ == Game) retro_->setPaused(paused_);
    pauseBtn_->setText(paused_ ? QStringLiteral("▶") : QStringLiteral("⏸"));
    emit focusRequested();
}

bool MediaPane::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress) emit focusRequested();
    return QWidget::eventFilter(obj, event);
}
