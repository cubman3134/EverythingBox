#include "MainWindow.h"
#include "../video/MpvWidget.h"
#include "../emu/RetroView.h"
#include "../ebook/EbookView.h"
#include "../pdf/PdfView.h"
#include "LibraryView.h"
#include "../addons/AddonManager.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "SettingsDialog.h"

#include <QWidget>
#include <QStackedWidget>
#include <QSplitter>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QHash>
#include <QCoreApplication>
#include <QMessageBox>
#include <QStatusBar>
#include <QChar>
#include <cmath>

// Audio extensions, shared by the open dialog filter and folder-queue scanning.
static const QStringList kAudioExts = {
    "mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma", "alac", "aiff", "aif", "ape", "mka"
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    player_ = new MpvWidget(this);
    retro_ = new RetroView(this);
    book_ = new EbookView(this);
    pdf_ = new PdfView(this);

    addons_ = std::make_unique<AddonManager>();
    library_ = new LibraryView(addons_.get(), this);
    connect(library_, &LibraryView::openItem, this, &MainWindow::openLibraryItem);

    // The player page pairs the libmpv surface with a playlist panel (shown only for audio queues).
    playlist_ = new QListWidget(this);
    playlist_->setVisible(false);
    playlist_->setMinimumWidth(180); // stay readable when the splitter shows it
    connect(playlist_, &QListWidget::itemActivated, this,
            [this] { playTrack(playlist_->currentRow()); });
    auto* playerPage = new QSplitter(Qt::Horizontal, this);
    playerPage->addWidget(playlist_);
    playerPage->addWidget(player_);
    playerPage->setStretchFactor(1, 1);
    playerPage->setSizes({ 260, 900 });
    playerPage_ = playerPage;

    stack_ = new QStackedWidget(this);
    stack_->addWidget(playerPage); // index 0 - video / audio
    stack_->addWidget(retro_);     // index 1 - games
    stack_->addWidget(book_);      // index 2 - ebooks
    stack_->addWidget(pdf_);       // index 3 - pdf
    stack_->addWidget(library_);   // index 4 - addon library

    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(stack_, 1);

    auto* bar = new QHBoxLayout();
    auto* openVid = new QPushButton(tr("Open Video…"), this);
    auto* openAud = new QPushButton(tr("Open Audio…"), this);
    auto* openRom = new QPushButton(tr("Open Game…"), this);
    auto* openDoc = new QPushButton(tr("Open Document…"), this);
    auto* libraryBtn = new QPushButton(tr("Library"), this);
    auto* settings = new QPushButton(tr("Settings…"), this);
    auto* saveState = new QPushButton(tr("Save State"), this);
    auto* loadState = new QPushButton(tr("Load State"), this);
    auto* prevTrk = new QPushButton(tr("⏮"), this);
    auto* playPause = new QPushButton(tr("Play / Pause"), this);
    auto* nextTrk = new QPushButton(tr("⏭"), this);
    auto* stop = new QPushButton(tr("Stop"), this);
    prevTrk->setToolTip(tr("Previous track"));
    nextTrk->setToolTip(tr("Next track"));
    seek_ = new QSlider(Qt::Horizontal, this);
    seek_->setRange(0, 1000);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);

    bar->addWidget(openVid);
    bar->addWidget(openAud);
    bar->addWidget(openRom);
    bar->addWidget(openDoc);
    bar->addWidget(libraryBtn);
    bar->addWidget(settings);
    bar->addWidget(saveState);
    bar->addWidget(loadState);
    bar->addWidget(prevTrk);
    bar->addWidget(playPause);
    bar->addWidget(nextTrk);
    bar->addWidget(stop);
    bar->addWidget(seek_, 1);
    bar->addWidget(time_);
    v->addLayout(bar);
    setCentralWidget(central);

    connect(openVid, &QPushButton::clicked, this, &MainWindow::openFile);
    connect(openAud, &QPushButton::clicked, this, &MainWindow::openAudio);
    connect(openRom, &QPushButton::clicked, this, &MainWindow::openGame);
    connect(openDoc, &QPushButton::clicked, this, &MainWindow::openDocument);
    connect(libraryBtn, &QPushButton::clicked, this, &MainWindow::openLibrary);
    connect(settings, &QPushButton::clicked, this, &MainWindow::openSettings);
    connect(prevTrk, &QPushButton::clicked, this, &MainWindow::prevTrack);
    connect(nextTrk, &QPushButton::clicked, this, &MainWindow::nextTrack);
    connect(player_, &MpvWidget::endReached, this, &MainWindow::onTrackEnded);
    connect(saveState, &QPushButton::clicked, this, [this] { QString e; if (!retro_->saveState(&e)) statusBar()->showMessage(e, 4000); });
    connect(loadState, &QPushButton::clicked, this, [this] { QString e; if (!retro_->loadState(&e)) statusBar()->showMessage(e, 4000); });
    connect(retro_, &RetroView::statusMessage, this, [this](const QString& t) { statusBar()->showMessage(t, 3000); });
    connect(playPause, &QPushButton::clicked, player_, &MpvWidget::togglePause);
    connect(stop, &QPushButton::clicked, this, [this] { player_->stop(); retro_->stop(); });
    connect(player_, &MpvWidget::durationChanged, this, &MainWindow::onDuration);
    connect(player_, &MpvWidget::positionChanged, this, &MainWindow::onPosition);
    connect(seek_, &QSlider::sliderPressed, this, [this] { sliderDown_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, &MainWindow::onSeekReleased);
}

MainWindow::~MainWindow() = default; // AddonManager is complete in this translation unit

void MainWindow::openFile()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Video"), QString(),
        tr("Video (*.mkv *.mp4 *.avi *.mov *.webm *.m4v *.wmv *.flv *.ts *.m2ts);;All files (*.*)"));
    if (f.isEmpty()) return;
    retro_->stop();
    book_->persist();
    pdf_->persist();
    clearAudioQueue();
    stack_->setCurrentWidget(playerPage_);
    player_->play(f);
}

void MainWindow::openAudio()
{
    // Audio plays through the same libmpv player; an overlay shows the track since there's no picture.
    // Select one file to queue its whole folder, or multi-select to queue exactly those tracks.
    const QStringList sel = QFileDialog::getOpenFileNames(
        this, tr("Open Audio"), QString(),
        tr("Audio (*.mp3 *.flac *.ogg *.opus *.wav *.m4a *.aac *.wma *.alac *.aiff *.aif *.ape *.mka);;"
           "All files (*.*)"));
    if (sel.isEmpty()) return;

    QStringList queue;
    int start = 0;
    if (sel.size() == 1)
    {
        // Folder queue: play the whole directory, sorted, starting at the chosen track.
        const QFileInfo fi(sel.first());
        QStringList filters;
        for (const QString& ext : kAudioExts) filters << QStringLiteral("*.") + ext;
        const QFileInfoList entries = QDir(fi.absolutePath()).entryInfoList(filters, QDir::Files, QDir::Name);
        for (const QFileInfo& e : entries) queue << e.absoluteFilePath();
        start = queue.indexOf(fi.absoluteFilePath());
        if (start < 0) { queue = { fi.absoluteFilePath() }; start = 0; }
    }
    else
    {
        queue = sel; // exactly the selected tracks, in the order the dialog returned them
    }

    retro_->stop();
    book_->persist();
    pdf_->persist();
    setAudioQueue(queue, start);
}

void MainWindow::setAudioQueue(const QStringList& files, int startIndex)
{
    tracks_ = files;
    playlist_->clear();
    for (const QString& f : tracks_) playlist_->addItem(QFileInfo(f).completeBaseName());
    playlist_->setVisible(true);
    stack_->setCurrentWidget(playerPage_);
    playTrack(startIndex);
}

void MainWindow::playTrack(int index)
{
    if (index < 0 || index >= tracks_.size()) return;
    trackIndex_ = index;
    playlist_->setCurrentRow(index);
    statusBar()->showMessage(tr("Track %1 of %2").arg(index + 1).arg(tracks_.size()), 3000);
    player_->play(tracks_[index]);
}

void MainWindow::nextTrack()
{
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) playTrack(trackIndex_ + 1);
}

void MainWindow::prevTrack()
{
    if (trackIndex_ > 0) playTrack(trackIndex_ - 1);
}

void MainWindow::onTrackEnded()
{
    // Auto-advance the audio queue when a track finishes (ignored for video / single files).
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) playTrack(trackIndex_ + 1);
}

void MainWindow::clearAudioQueue()
{
    tracks_.clear();
    trackIndex_ = -1;
    if (playlist_) { playlist_->clear(); playlist_->setVisible(false); }
}

void MainWindow::openGame()
{
    const QString rom = QFileDialog::getOpenFileName(
        this, tr("Open Game (ROM)"), QString(),
        tr("ROMs (*.gba *.gb *.gbc *.sgb *.nes *.fds *.sfc *.smc *.md *.gen *.smd *.sms *.gg *.n64 *.z64 "
           "*.pce *.ws *.wsc *.a26 *.cue *.chd *.pbp);;All files (*.*)"));
    if (rom.isEmpty()) return;

    const QString ext = QFileInfo(rom).suffix().toLower();
    const GameSystem* sys = SystemCatalog::forExtension(ext);
    if (!sys)
    {
        QMessageBox::warning(this, tr("Unsupported file"),
                             tr("No system is configured for .%1 files.").arg(ext));
        return;
    }

    QString core = Settings::coreFor(sys->id);
    if (core.isEmpty())
        core = sys->cores.value(0); // catalog default

    // No prompt: use the configured core, downloading it from the buildbot if it isn't installed.
    const QString corePath = CoreManager::ensureCore(core, this);
    if (corePath.isEmpty())
        return; // cancelled / failed (a message was already shown)

    player_->stop();
    book_->persist();
    pdf_->persist();
    clearAudioQueue();
    QString err;
    if (retro_->openGame(corePath, rom, core, &err))
        stack_->setCurrentWidget(retro_);
    else
        QMessageBox::warning(this, tr("Can't run game"), err);
}

void MainWindow::openDocument()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Document"), QString(),
        tr("Documents (*.epub *.pdf);;EPUB books (*.epub);;PDF documents (*.pdf);;All files (*.*)"));
    if (f.isEmpty()) return;

    const QString ext = QFileInfo(f).suffix().toLower();
    QString err;

    if (ext == QStringLiteral("pdf"))
    {
        if (!pdf_->openPdf(f, &err)) { QMessageBox::warning(this, tr("Can't open PDF"), err); return; }
        player_->stop();
        retro_->stop();
        book_->persist();
        clearAudioQueue();
        stack_->setCurrentWidget(pdf_);
    }
    else // treat everything else as an EPUB (the reader validates and reports if it isn't one)
    {
        if (!book_->openBook(f, &err)) { QMessageBox::warning(this, tr("Can't open book"), err); return; }
        player_->stop();
        retro_->stop();
        pdf_->persist();
        clearAudioQueue();
        stack_->setCurrentWidget(book_);
    }
}

void MainWindow::openLibrary()
{
    library_->refreshSources();
    stack_->setCurrentWidget(library_);
}

void MainWindow::openLibraryItem(const MediaItem& item)
{
    if (item.url.isEmpty())
    {
        QMessageBox::warning(this, tr("Can't open"), tr("This item has no location."));
        return;
    }
    const QString url = item.url;
    const QString type = item.type.toLower();
    const QString lower = url.toLower();
    QString err;

    if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
    {
        if (!book_->openBook(url, &err)) { QMessageBox::warning(this, tr("Can't open book"), err); return; }
        player_->stop(); retro_->stop(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(book_);
    }
    else if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
    {
        if (!pdf_->openPdf(url, &err)) { QMessageBox::warning(this, tr("Can't open PDF"), err); return; }
        player_->stop(); retro_->stop(); book_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(pdf_);
    }
    else if (type == QStringLiteral("audio"))
    {
        retro_->stop(); book_->persist(); pdf_->persist();
        setAudioQueue({ url }, 0); // a single-track queue; libmpv also streams http(s) audio
    }
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(playerPage_);
        player_->play(url);
    }
}

void MainWindow::openSettings()
{
    // The settings dialog loads cores headlessly to read their options; only one libretro core can be
    // live at a time (the C ABI routes through a global), so stop any running game first.
    retro_->stop();
    SettingsDialog dlg(retro_->gamepad(), retro_->keymap(), this);
    dlg.exec();
}

void MainWindow::onDuration(double seconds) { duration_ = seconds; }

void MainWindow::onPosition(double seconds)
{
    if (!sliderDown_ && duration_ > 0.0)
        seek_->setValue(static_cast<int>(seconds / duration_ * 1000.0));
    time_->setText(fmt(seconds) + QStringLiteral(" / ") + fmt(duration_));
}

void MainWindow::onSeekReleased()
{
    sliderDown_ = false;
    if (duration_ > 0.0)
        player_->setPosition(seek_->value() / 1000.0 * duration_);
}

QString MainWindow::fmt(double seconds)
{
    if (seconds < 0.0 || std::isnan(seconds))
        seconds = 0.0;
    const int t = static_cast<int>(seconds);
    return QString(QStringLiteral("%1:%2")).arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}
