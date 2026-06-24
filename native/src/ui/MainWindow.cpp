#include "MainWindow.h"
#include "../video/MpvWidget.h"
#include "../emu/RetroView.h"
#include "../ebook/EbookView.h"
#include "../pdf/PdfView.h"
#include "../comic/ComicView.h"
#include "LibraryView.h"
#include "HomeView.h"
#include "ControllerRemapDialog.h"
#include "../addons/AddonManager.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "../core/RecentStore.h"
#include "../core/ProfileStore.h"
#include "../core/Theme.h"
#include "../core/CloudSync.h"
#include "ProfileDialog.h"
#include "RegistryBrowser.h"
#include <QInputDialog>
#include <QSettings>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include "SettingsDialog.h"

#include <QWidget>
#include <QStackedWidget>
#include <QSplitter>
#include <QListWidget>
#include <QFrame>
#include <QTimer>
#include <QEventLoop>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QApplication>
#include <QPushButton>
#include <QAbstractButton>
#include <QSlider>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QResizeEvent>
#include <QShortcut>
#include <QKeyEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QHash>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QStatusBar>
#include <QChar>
#include <cmath>

// Audio extensions, shared by the open dialog filter and folder-queue scanning. (m4b = MP4/AAC audiobooks.)
static const QStringList kAudioExts = {
    "mp3", "flac", "ogg", "opus", "wav", "m4a", "m4b", "aac", "wma", "alac", "aiff", "aif", "ape", "mka"
};

// Per-profile settings store (resume positions, etc.), mirroring the accessor the other views use.
static QSettings& store()
{
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

// Stable, path-derived key prefix for one audiobook's resume state.
static QString audiobookKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("audiobook/") + QString::fromLatin1(h) + QStringLiteral("/");
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    player_ = new MpvWidget(this);
    retro_ = new RetroView(this);
    book_ = new EbookView(this);
    pdf_ = new PdfView(this);
    comic_ = new ComicView(this);

    addons_ = std::make_unique<AddonManager>();
    cloud_ = std::make_unique<CloudSync>(this); // eager: needed for push-on-exit even if the panel never opens
    library_ = new LibraryView(addons_.get(), this);
    connect(library_, &LibraryView::openItem, this, &MainWindow::openLibraryItem);
    home_ = new HomeView(addons_.get(), this);
    connect(home_, &HomeView::openItem, this, &MainWindow::openLibraryItem);

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
    stack_->addWidget(home_);      // index 5 - home / catalog landing
    stack_->addWidget(comic_);     // index 6 - comic (CBZ) reader

    // Inline settings panel page (Settings / Theme / Cloud Sync / General live here instead of popups).
    panelPage_ = new QWidget(this);
    panelPage_->setObjectName(QStringLiteral("settingsPanel"));
    panelPage_->setStyleSheet(QStringLiteral("#settingsPanel{background:#f4f6f8;}"));
    auto* pv = new QVBoxLayout(panelPage_);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(0);
    auto* panelHeader = new QWidget(panelPage_);
    panelHeader->setObjectName(QStringLiteral("panelHeader"));
    panelHeader->setStyleSheet(QStringLiteral("#panelHeader{background:#2b2f3a;} #panelHeader QLabel{color:#fff;}"));
    auto* phl = new QHBoxLayout(panelHeader);
    phl->setContentsMargins(16, 10, 16, 10);
    auto* panelBack = new QPushButton(tr("‹ Back"), panelHeader);
    panelBack->setStyleSheet(QStringLiteral(
        "QPushButton{background:rgba(255,255,255,0.12);color:#fff;border:none;border-radius:8px;"
        "padding:10px 18px;font-size:16px;font-weight:bold;} QPushButton:hover{background:rgba(255,255,255,0.22);}"));
    panelBack->setCursor(Qt::PointingHandCursor);
    connect(panelBack, &QPushButton::clicked, this, [this] { if (panelOnBack_) panelOnBack_(); });
    panelTitle_ = new QLabel(panelHeader);
    panelTitle_->setStyleSheet(QStringLiteral("font-size:20px;font-weight:bold;"));
    phl->addWidget(panelBack);
    phl->addSpacing(12);
    phl->addWidget(panelTitle_, 1);
    pv->addWidget(panelHeader);
    panelScroll_ = new QScrollArea(panelPage_);
    panelScroll_->setWidgetResizable(true);
    panelScroll_->setFrameShape(QFrame::NoFrame);
    pv->addWidget(panelScroll_, 1);
    stack_->addWidget(panelPage_); // index 7 - inline settings panels

    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(stack_, 1);
    setCentralWidget(central);
    // No persistent bottom bar: navigation lives in each view (Home's top bar, the Settings hub, the media
    // transport overlay, the emulator Esc menu, and per-view Home buttons).

    // Media transport overlay: a child of the player surface (composites over the GL video), shown only
    // while media is open and the mouse moves.
    mediaControls_ = new QFrame(player_);
    mediaControls_->setObjectName(QStringLiteral("mediaControls"));
    mediaControls_->setStyleSheet(QStringLiteral(
        "#mediaControls { background: rgba(20,20,24,0.85); border-radius: 10px; }"
        "#mediaControls QLabel { color: #e8e8e8; }"));
    auto* mc = new QHBoxLayout(mediaControls_);
    mc->setContentsMargins(12, 8, 12, 8);
    auto* prevChap = new QPushButton(tr("⏮"), mediaControls_);
    auto* rewind = new QPushButton(tr("⏪"), mediaControls_);
    auto* playPause = new QPushButton(tr("⏯"), mediaControls_);
    auto* fastFwd = new QPushButton(tr("⏩"), mediaControls_);
    auto* nextChap = new QPushButton(tr("⏭"), mediaControls_);
    auto* stop = new QPushButton(tr("⏹"), mediaControls_);
    auto* subsBtn = new QPushButton(tr("CC"), mediaControls_);
    auto* subLoad = new QPushButton(tr("＋Sub"), mediaControls_);
    auto* fullScreen = new QPushButton(tr("⛶"), mediaControls_);
    prevChap->setToolTip(tr("Previous chapter"));
    rewind->setToolTip(tr("Rewind 10s"));
    playPause->setToolTip(tr("Play / Pause"));
    fastFwd->setToolTip(tr("Forward 10s"));
    nextChap->setToolTip(tr("Next chapter"));
    stop->setToolTip(tr("Stop"));
    subsBtn->setToolTip(tr("Subtitles: cycle tracks / off"));
    subLoad->setToolTip(tr("Load a subtitle file…"));
    fullScreen->setToolTip(tr("Toggle full screen (F11)"));
    // Chapter nav is only meaningful for chaptered media (M4B audiobooks, some videos); hidden otherwise.
    prevChap->hide();
    nextChap->hide();
    seek_ = new QSlider(Qt::Horizontal, mediaControls_);
    seek_->setRange(0, 1000);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), mediaControls_);
    mc->addWidget(prevChap);
    mc->addWidget(rewind);
    mc->addWidget(playPause);
    mc->addWidget(fastFwd);
    mc->addWidget(nextChap);
    mc->addWidget(stop);
    mc->addWidget(seek_, 1);
    mc->addWidget(time_);
    mc->addWidget(subsBtn);
    mc->addWidget(subLoad);
    mc->addWidget(fullScreen);
    mediaControls_->hide();

    // Top-left "Back" overlay to exit the movie. Shown/hidden with the transport (on mouse move).
    videoBack_ = new QPushButton(tr("‹ Back"), player_);
    videoBack_->setObjectName(QStringLiteral("videoBack"));
    videoBack_->setStyleSheet(QStringLiteral(
        "#videoBack { background: rgba(20,20,24,0.85); color:#e8e8e8; border:none; border-radius:8px;"
        " padding:8px 16px; font-weight:bold; }"
        "#videoBack:hover { background: rgba(45,45,52,0.95); }"));
    videoBack_->setCursor(Qt::PointingHandCursor);
    videoBack_->setToolTip(tr("Exit the movie"));
    videoBack_->hide();
    videoBack_->installEventFilter(this); // keep the overlay alive while hovering it
    connect(videoBack_, &QPushButton::clicked, this, [this] {
        player_->stop(); mediaControls_->hide(); videoBack_->hide(); clearAudioQueue(); openHome();
    });

    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this] {
        // Hide after the inactivity timeout (mouse movement re-reveals via the event filter). In full
        // screen also blank the cursor so nothing lingers over the movie.
        mediaControls_->hide();
        videoBack_->hide();
        if (isFullScreen() && stack_->currentWidget() == playerPage_)
            player_->setCursor(Qt::BlankCursor);
    });

    // Reveal the controls on mouse movement over the player / controls.
    player_->setMouseTracking(true);
    player_->installEventFilter(this);
    mediaControls_->installEventFilter(this);

    connect(home_, &HomeView::requestOpenFile, this, &MainWindow::onRequestOpenFile);
    connect(home_, &HomeView::openRecent, this, &MainWindow::openRecent);
    connect(home_, &HomeView::switchProfileRequested, this, &MainWindow::onSwitchProfile);
    connect(home_, &HomeView::themeChanged, this, &MainWindow::onThemeChanged);
    connect(home_, &HomeView::settingsRequested, this, &MainWindow::openSettingsHub);
    connect(book_, &EbookView::homeRequested, this, &MainWindow::openHome);
    connect(pdf_, &PdfView::homeRequested, this, &MainWindow::openHome);
    connect(comic_, &ComicView::homeRequested, this, &MainWindow::openHome);
    connect(library_, &LibraryView::homeRequested, this, &MainWindow::openHome);
    connect(rewind, &QPushButton::clicked, this, [this] { player_->seekRelative(-10.0); revealMediaControls(); });
    connect(fastFwd, &QPushButton::clicked, this, [this] { player_->seekRelative(10.0); revealMediaControls(); });
    connect(prevChap, &QPushButton::clicked, this, [this] { player_->prevChapter(); revealMediaControls(); });
    connect(nextChap, &QPushButton::clicked, this, [this] { player_->nextChapter(); revealMediaControls(); });
    // Reveal the chapter-skip buttons only when the current file actually has chapters.
    connect(player_, &MpvWidget::chapterCountChanged, this, [prevChap, nextChap](int count) {
        const bool has = count > 1; // a single "chapter" spanning the whole file is not worth navigating
        prevChap->setVisible(has);
        nextChap->setVisible(has);
    });
    connect(fullScreen, &QPushButton::clicked, this, [this] { toggleFullScreen(); revealMediaControls(); });
    connect(subsBtn, &QPushButton::clicked, this, [this] { player_->cycleSubtitle(); revealMediaControls(); });
    connect(subLoad, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Load subtitle"), QString(),
            tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx);;All files (*)"));
        if (!f.isEmpty()) player_->addSubtitle(f);
        revealMediaControls();
    });
    connect(player_, &MpvWidget::endReached, this, &MainWindow::onTrackEnded);
    connect(retro_, &RetroView::statusMessage, this, [this](const QString& t) { statusBar()->showMessage(t, 3000); });
    connect(retro_, &RetroView::exitRequested, this, [this] { retro_->stop(); openHome(); });
    connect(playPause, &QPushButton::clicked, player_, &MpvWidget::togglePause);
    connect(stop, &QPushButton::clicked, this, [this] {
        player_->stop(); mediaControls_->hide(); clearAudioQueue(); openHome(); });
    connect(player_, &MpvWidget::durationChanged, this, &MainWindow::onDuration);
    connect(player_, &MpvWidget::positionChanged, this, &MainWindow::onPosition);
    connect(seek_, &QSlider::sliderPressed, this, [this] { sliderDown_ = true; });
    connect(seek_, &QSlider::sliderReleased, this, &MainWindow::onSeekReleased);
    // Hide the transport when leaving the player page.
    connect(stack_, &QStackedWidget::currentChanged, this, [this] {
        if (stack_->currentWidget() != playerPage_) { mediaControls_->hide(); videoBack_->hide(); } });

    // F11 toggles full screen anywhere in the window (Esc leaves it - see keyPressEvent).
    auto* fsShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fsShortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);

    stack_->setCurrentWidget(home_); // the catalog landing screen is shown first
}

MainWindow::~MainWindow() = default; // AddonManager is complete in this translation unit

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseMove && (obj == player_ || obj == mediaControls_ || obj == videoBack_))
        revealMediaControls();
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (mediaControls_ && mediaControls_->isVisible())
        positionMediaControls();
}

void MainWindow::leaveFullScreen()
{
    showNormal();
    statusBar()->show();       // restore the bottom bar
    player_->unsetCursor();    // restore the cursor
}

void MainWindow::toggleFullScreen()
{
    if (isFullScreen())
    {
        leaveFullScreen();
    }
    else
    {
        showFullScreen();
        statusBar()->hide();   // the status bar would otherwise show as a strip at the bottom of the movie
    }
    if (stack_->currentWidget() == playerPage_) revealMediaControls();
}

void MainWindow::keyPressEvent(QKeyEvent* e)
{
    // Esc leaves full screen (the emulator view consumes its own Esc for the pause menu, so this only
    // fires for the player / readers).
    if (e->key() == Qt::Key_Escape && isFullScreen()) { leaveFullScreen(); return; }

    // Arrow-key / remote navigation for the inline settings pages: Up/Down (and Left/Right) move focus
    // between rows, Enter/Select activates the focused row, Backspace goes back. Reaches MainWindow because
    // the panel rows (buttons/checkboxes) don't consume the arrow keys themselves.
    if (stack_->currentWidget() == panelPage_ && !panelDialog_) // an embedded dialog drives its own keys
    {
        switch (e->key())
        {
        case Qt::Key_Down: case Qt::Key_Right:
            focusNextChild(); return;
        case Qt::Key_Up: case Qt::Key_Left:
            focusPreviousChild(); return;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Select:
            if (auto* b = qobject_cast<QAbstractButton*>(focusWidget())) { b->click(); return; }
            break;
        case Qt::Key_Backspace:
            if (panelOnBack_) { panelOnBack_(); return; }
            break;
        default: break;
        }
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (focusedOnShow_) return;
    focusedOnShow_ = true;
    // Become the active window and drop keyboard focus into the home view so arrow keys work without a
    // click first. Deferred a tick so it runs after the window is actually on screen / activated.
    raise();
    activateWindow();
    QTimer::singleShot(0, this, [this] {
        activateWindow();
        if (stack_->currentWidget() == home_ && home_) home_->focusContent();
    });
}

void MainWindow::revealMediaControls()
{
    if (stack_->currentWidget() != playerPage_) return; // only over an open media item
    player_->unsetCursor();                              // cursor visible again whenever controls show
    positionMediaControls();
    mediaControls_->show();
    mediaControls_->raise();
    videoBack_->show();
    videoBack_->raise();
    controlsHideTimer_->start(2500);
}

void MainWindow::positionMediaControls()
{
    // mediaControls_ is a child of player_, so position it in the player's local coordinates (bottom band).
    const int margin = 16;
    const int h = mediaControls_->sizeHint().height();
    mediaControls_->setGeometry(margin, player_->height() - h - margin,
                                player_->width() - 2 * margin, h);
    videoBack_->adjustSize();
    videoBack_->move(margin, margin); // top-left
}

void MainWindow::openFile()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Video"), QString(),
        tr("Video (*.mkv *.mp4 *.avi *.mov *.webm *.m4v *.wmv *.flv *.ts *.m2ts);;All files (*.*)"));
    if (f.isEmpty()) return;
    openVideoPath(f);
}

void MainWindow::openVideoPath(const QString& path)
{
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();
    stack_->setCurrentWidget(playerPage_);
    player_->play(path);
    revealMediaControls();
    RecentStore::add({ path, QFileInfo(path).completeBaseName(), QStringLiteral("video"), QString() });
}

void MainWindow::openAudio()
{
    // Audio plays through the same libmpv player; an overlay shows the track since there's no picture.
    // Select one file to queue its whole folder, or multi-select to queue exactly those tracks.
    const QString pattern = QStringLiteral("*.") + kAudioExts.join(QStringLiteral(" *."));
    const QStringList sel = QFileDialog::getOpenFileNames(
        this, tr("Open Audio"), QString(),
        tr("Audio (%1);;All files (*.*)").arg(pattern));
    if (sel.isEmpty()) return;

    if (sel.size() == 1) { openAudioPath(sel.first()); return; } // folder queue starting at this track

    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    setAudioQueue(sel, 0); // exactly the selected tracks, in the order the dialog returned them
    const QString first = sel.first();
    RecentStore::add({ first, QFileInfo(first).completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::openAudioPath(const QString& path)
{
    const QFileInfo fi(path);
    QStringList queue;
    int start = 0;
    if (fi.suffix().toLower() == QStringLiteral("m4b"))
    {
        // An audiobook is one self-contained file (often with chapters) - don't queue the rest of the folder,
        // and resume where the listener left off.
        queue = { fi.absoluteFilePath() };
        audiobookPath_ = fi.absoluteFilePath();
        audiobookResume_ = store().value(audiobookKey(audiobookPath_) + QStringLiteral("pos"), 0.0).toDouble();
        lastSavedPos_ = -100.0;
    }
    else
    {
        audiobookPath_.clear(); // a normal track, not a resumable audiobook
        // Play the whole containing folder, sorted, starting at this file (the single-select behavior).
        QStringList filters;
        for (const QString& ext : kAudioExts) filters << QStringLiteral("*.") + ext;
        const QFileInfoList entries = QDir(fi.absolutePath()).entryInfoList(filters, QDir::Files, QDir::Name);
        for (const QFileInfo& e : entries) queue << e.absoluteFilePath();
        start = queue.indexOf(fi.absoluteFilePath());
        if (start < 0) { queue = { fi.absoluteFilePath() }; start = 0; }
    }

    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    setAudioQueue(queue, start);
    RecentStore::add({ fi.absoluteFilePath(), fi.completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::setAudioQueue(const QStringList& files, int startIndex)
{
    tracks_ = files;
    playlist_->clear();
    for (const QString& f : tracks_) playlist_->addItem(QFileInfo(f).completeBaseName());
    playlist_->setVisible(true);
    stack_->setCurrentWidget(playerPage_);
    playTrack(startIndex);
    revealMediaControls();
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
    // A finished audiobook is "done" - clear its resume mark so the next open starts at the beginning.
    if (!audiobookPath_.isEmpty())
    {
        store().remove(audiobookKey(audiobookPath_));
        store().sync();
        audiobookPath_.clear();
        lastSavedPos_ = -100.0;
        return;
    }
    // Auto-advance the audio queue when a track finishes (ignored for video / single files).
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) playTrack(trackIndex_ + 1);
}

void MainWindow::persistAudiobook()
{
    if (audiobookPath_.isEmpty() || audioPos_ <= 0.0) return;
    const QString k = audiobookKey(audiobookPath_);
    store().setValue(k + QStringLiteral("pos"), audioPos_);
    store().setValue(k + QStringLiteral("title"), QFileInfo(audiobookPath_).completeBaseName());
    store().sync();
    lastSavedPos_ = audioPos_;
}

void MainWindow::clearAudioQueue()
{
    persistAudiobook();      // save where we left off before leaving the audiobook
    audiobookPath_.clear();
    audiobookResume_ = 0.0;
    lastSavedPos_ = -100.0;
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
    openGamePath(rom);
}

void MainWindow::openGamePath(const QString& rom)
{
    const QString ext = QFileInfo(rom).suffix().toLower();
    const GameSystem* sys = SystemCatalog::forExtension(ext);
    if (!sys)
    {
        statusBar()->showMessage(tr("No system is configured for .%1 files.").arg(ext), 6000);
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
    comic_->persist();
    clearAudioQueue();
    QString err;
    if (retro_->openGame(corePath, rom, core, &err))
    {
        stack_->setCurrentWidget(retro_);
        RecentStore::add({ rom, QFileInfo(rom).completeBaseName(), QStringLiteral("game"), QString() });
    }
    else
        statusBar()->showMessage(tr("Can't run game: %1").arg(err), 6000);
}

void MainWindow::openDocument()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Document"), QString(),
        tr("Documents (*.epub *.pdf *.cbz);;EPUB books (*.epub);;PDF documents (*.pdf);;"
           "Comics (*.cbz);;All files (*.*)"));
    if (f.isEmpty()) return;
    openDocumentPath(f);
}

void MainWindow::openDocumentPath(const QString& f)
{
    const QString ext = QFileInfo(f).suffix().toLower();
    QString err;

    if (ext == QStringLiteral("pdf"))
    {
        if (!pdf_->openPdf(f, &err)) { statusBar()->showMessage(tr("Can't open PDF: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(pdf_);
    }
    else if (ext == QStringLiteral("cbz"))
    {
        if (!comic_->openComic(f, &err)) { statusBar()->showMessage(tr("Can't open comic: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(comic_);
    }
    else // treat everything else as an EPUB (the reader validates and reports if it isn't one)
    {
        if (!book_->openBook(f, &err)) { statusBar()->showMessage(tr("Can't open book: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(book_);
    }
    RecentStore::add({ f, QFileInfo(f).completeBaseName(), QStringLiteral("document"), QString() });
}

void MainWindow::openLibrary()
{
    library_->refreshSources();
    stack_->setCurrentWidget(library_);
}

void MainWindow::openHome()
{
    // Leaving whatever was open: stop playback/emulation, save reader positions.
    player_->stop();
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();
    home_->refresh();
    stack_->setCurrentWidget(home_);
}

void MainWindow::onRequestOpenFile(const QString& kind)
{
    // The Home view's "open a file" item routes here (the toolbar open buttons were removed).
    if (kind == QStringLiteral("video"))         openFile();
    else if (kind == QStringLiteral("audio"))    openAudio();
    else if (kind == QStringLiteral("game"))     openGame();
    else if (kind == QStringLiteral("document")) openDocument();
}

void MainWindow::openRecent(const QString& path, const QString& kind)
{
    if (!QFileInfo::exists(path))
    {
        statusBar()->showMessage(tr("That file can no longer be found: %1").arg(path), 5000);
        return;
    }
    if (kind == QStringLiteral("video"))         openVideoPath(path);
    else if (kind == QStringLiteral("audio"))    openAudioPath(path);
    else if (kind == QStringLiteral("game"))     openGamePath(path);
    else if (kind == QStringLiteral("document")) openDocumentPath(path);
}

void MainWindow::onSwitchProfile()
{
    auto* dlg = new ProfileDialog(/*mustChoose*/ false, this);
    showDialogPanel(tr("Profiles"), dlg, [this, dlg](int result) {
        if (result == QDialog::Accepted && !dlg->selectedId().isEmpty())
            ProfileStore::setCurrent(dlg->selectedId());
        // This entry point lives on Home; openHome() switches back and refreshes for the active profile
        // (covers a switched user, a deletion repointing "current", or an edited name/icon).
        openHome();
    }, [this] { openHome(); });
}

void MainWindow::openLibraryItem(const MediaItem& item)
{
    if (item.url.isEmpty())
    {
        // Catalog metadata with no file associated yet (movies/games/episodes/tracks).
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(item.title), 4000);
        return;
    }
    const QString url = item.url;
    const QString type = item.type.toLower();
    const QString lower = url.toLower();
    QString err;

    if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
    {
        if (!book_->openBook(url, &err)) { statusBar()->showMessage(tr("Can't open book: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(book_);
    }
    else if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
    {
        if (!pdf_->openPdf(url, &err)) { statusBar()->showMessage(tr("Can't open PDF: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(pdf_);
    }
    else if (lower.endsWith(QStringLiteral(".cbz"))) // a downloaded/associated comic archive
    {
        if (!comic_->openComic(url, &err)) { statusBar()->showMessage(tr("Can't open comic: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(comic_);
    }
    else if (type == QStringLiteral("audio"))
    {
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
        setAudioQueue({ url }, 0); // a single-track queue; libmpv also streams http(s) audio
    }
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(playerPage_);
        player_->play(url);
        revealMediaControls();
    }
}

void MainWindow::onThemeChanged(const QColor& background, const QColor& accent)
{
    // Match the home view's theme app-wide: light window + status bar, accent-coloured status text.
    setStyleSheet(QString("QMainWindow{background:%1;}").arg(background.name()));
    statusBar()->setStyleSheet(QString("QStatusBar{background:%1;color:#2a2c30;}").arg(background.name()));
    Q_UNUSED(accent);
}

// Swap the inline panel's content and show it. `build` fills a fresh content layout; `onBack` runs when
// the header Back is pressed (returns to the parent panel or the underlying page).
void MainWindow::showPanel(const QString& title, const std::function<void(QVBoxLayout*)>& build,
                           const std::function<void()>& onBack)
{
    panelTitle_->setText(title);
    panelOnBack_ = onBack;
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(28, 24, 28, 24);
    v->setSpacing(14);
    build(v);
    v->addStretch(1);
    panelScroll_->setWidget(content); // deletes the previous content widget
    stack_->setCurrentWidget(panelPage_);
    panelDialog_ = nullptr; // a plain panel: no inline dialog owns the keyboard
    // Drop focus onto the first interactive row so arrow keys / a remote work without a click first.
    // Deferred a tick so the new content widget is laid out and its children are focusable.
    QTimer::singleShot(0, this, [this] {
        if (stack_->currentWidget() != panelPage_) return;
        QWidget* w = panelScroll_->widget();
        if (!w) return;
        for (QWidget* child : w->findChildren<QWidget*>())
        {
            if (child->isVisibleTo(w) && child->focusPolicy() & Qt::TabFocus)
            { child->setFocus(); return; }
        }
    });
}

void MainWindow::showDialogPanel(const QString& title, QDialog* dlg,
                                 const std::function<void(int)>& onFinished,
                                 const std::function<void()>& onBack)
{
    dlg->setWindowFlags(Qt::Widget); // render inline instead of as a separate top-level window
    // Queued: the handler navigates away (deleting this dialog), so it must run AFTER QDialog::done()
    // returns rather than mid-emission, or we'd free the dialog out from under its own call stack.
    connect(dlg, &QDialog::finished, this, onFinished, Qt::QueuedConnection);
    showPanel(title, [dlg](QVBoxLayout* v) { v->addWidget(dlg); }, onBack);
    panelDialog_ = dlg; // its own widgets handle the keyboard; suppress the panel's row arrow-nav
    dlg->setFocus();
}

// A large, left-aligned menu row for the inline settings pages (TV/remote-friendly target size).
static QPushButton* panelRow(const QString& label)
{
    auto* b = new QPushButton(label);
    b->setMinimumHeight(54);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{font-size:17px;text-align:left;padding:14px 20px;border:1px solid rgba(0,0,0,0.12);"
        "border-radius:10px;background:rgba(0,0,0,0.04);} QPushButton:hover{background:rgba(0,0,0,0.10);}"
        "QPushButton:focus{border:2px solid #2C72C9;}"));
    return b;
}

void MainWindow::openSettingsHub()
{
    // Entering the settings area from a real page: remember it so the top-level Back returns there.
    if (stack_->currentWidget() != panelPage_) panelReturnTo_ = stack_->currentWidget();
    showPanel(tr("Settings"), [this](QVBoxLayout* v) {
        auto add = [this, v](const QString& label, std::function<void()> fn) {
            auto* b = panelRow(label);
            connect(b, &QPushButton::clicked, this, fn);
            v->addWidget(b);
        };
        add(tr("General"),            [this] { openGeneralSettings(); });
        add(tr("Theme"),              [this] { openThemes(); });
        add(tr("Add-ons"),            [this] { openLibrary(); });
        add(tr("Cloud Sync"),         [this] { openCloudSync(); });
        add(tr("Emulator Settings…"), [this] { openEmulatorSettings(); }); // still a popup (phase 2)
        add(tr("Input Mapping…"),     [this] { openInputMapping(); });     // still a popup (phase 2)
    }, [this] { stack_->setCurrentWidget(panelReturnTo_); });
}

void MainWindow::openGeneralSettings()
{
    showPanel(tr("General"), [this](QVBoxLayout* v) {
        auto* heading = new QLabel(tr("Subtitles"));
        heading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(heading);

        auto* on = new QCheckBox(tr("Show subtitles by default"));
        on->setStyleSheet(QStringLiteral("font-size:15px;"));
        on->setChecked(Settings::subtitlesOnByDefault());
        v->addWidget(on);
        connect(on, &QCheckBox::toggled, this, [](bool c) { Settings::setSubtitlesOnByDefault(c); }); // save on change

        auto* langRow = new QHBoxLayout();
        langRow->addWidget(new QLabel(tr("Default language:")));
        auto* lang = new QComboBox();
        lang->setMinimumHeight(34);
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / first available"), QString() }, { QStringLiteral("English"), QStringLiteral("eng") },
            { QStringLiteral("Spanish"), QStringLiteral("spa") }, { QStringLiteral("French"), QStringLiteral("fra") },
            { QStringLiteral("German"), QStringLiteral("deu") }, { QStringLiteral("Italian"), QStringLiteral("ita") },
            { QStringLiteral("Portuguese"), QStringLiteral("por") }, { QStringLiteral("Dutch"), QStringLiteral("nld") },
            { QStringLiteral("Russian"), QStringLiteral("rus") }, { QStringLiteral("Japanese"), QStringLiteral("jpn") },
            { QStringLiteral("Korean"), QStringLiteral("kor") }, { QStringLiteral("Chinese"), QStringLiteral("zho") },
            { QStringLiteral("Arabic"), QStringLiteral("ara") },
        };
        const QString cur = Settings::subtitleLanguage();
        bool found = false;
        for (const auto& l : langs) { lang->addItem(l.first, l.second); if (l.second == cur) found = true; }
        if (!found && !cur.isEmpty()) lang->addItem(tr("%1 (custom)").arg(cur), cur); // keep a previously-set code
        lang->setCurrentIndex(qMax(0, lang->findData(cur)));
        // Non-editable: clicking anywhere opens the list (an editable combo only opens via the tiny arrow),
        // and it's fully arrow/remote navigable.
        lang->setEnabled(on->isChecked());
        connect(on, &QCheckBox::toggled, lang, &QComboBox::setEnabled);
        connect(lang, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [lang](int idx) { Settings::setSubtitleLanguage(lang->itemData(idx).toString()); }); // save on change
        langRow->addWidget(lang, 1);
        v->addLayout(langRow);

        auto* note = new QLabel(tr("Applies to the next video. Subtitles still toggle in-player with the CC button."));
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(note);
    }, [this] { openSettingsHub(); });
}

void MainWindow::openCloudSync()
{
    if (!cloud_) cloud_ = std::make_unique<CloudSync>(this);
    showPanel(tr("Cloud Sync"), [this](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("<b>Google Drive sync</b><br>Back up your profiles, history, favourites, "
            "settings and local add-ons to a “MyMediaVault” folder on your Google Drive, to sync between devices."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);
        auto* status = new QLabel(); status->setWordWrap(true); status->setTextFormat(Qt::RichText);
        status->setStyleSheet(QStringLiteral("font-size:15px;padding:6px 0;"));
        v->addWidget(status);

        auto* signIn = panelRow(tr("Sign in with Google"));
        auto* syncNow = panelRow(tr("Sync now"));
        auto* signOut = panelRow(tr("Sign out"));
        auto* setup = panelRow(tr("Set up sign-in…"));
        v->addWidget(signIn); v->addWidget(syncNow); v->addWidget(signOut); v->addWidget(setup);

        auto refresh = [this, status, signIn, syncNow, signOut, setup] {
            const bool cfg = CloudSync::isConfigured();
            const bool in = cloud_->isSignedIn();
            setup->setText(cfg ? tr("Change sign-in client…") : tr("Set up sign-in…"));
            signIn->setVisible(cfg && !in);
            syncNow->setVisible(in);
            signOut->setVisible(in);
            if (!cfg) status->setText(tr("Google sign-in isn’t set up yet — “Set up sign-in…” to paste a Desktop-app client."));
            else if (in) status->setText(tr("Signed in as <b>%1</b>.").arg(cloud_->accountEmail().toHtmlEscaped()));
            else status->setText(tr("Not signed in — click “Sign in with Google”."));
        };
        refresh();

        connect(signIn, &QPushButton::clicked, this, [this, status] { status->setText(tr("Opening your browser…")); cloud_->signIn(); });
        connect(signOut, &QPushButton::clicked, this, [this] { cloud_->signOut(); });
        connect(syncNow, &QPushButton::clicked, this, [this, status] { status->setText(tr("Syncing…")); cloudSyncNow(); });
        connect(setup, &QPushButton::clicked, this, [this] { openCloudClientSetup(); });
        // Context = status (recreated each time the panel is built) -> these auto-disconnect on rebuild.
        connect(cloud_.get(), &CloudSync::signedIn, status, [this, refresh](const QString&) {
            refresh(); raise(); activateWindow(); cloudSyncNow();
        });
        connect(cloud_.get(), &CloudSync::signInFailed, status, [status](const QString& e) { status->setText(tr("Sign-in failed: %1").arg(e)); });
        connect(cloud_.get(), &CloudSync::signedOut, status, [refresh] { refresh(); });
    }, [this] { openSettingsHub(); });
}

// Inline form (no popup) to paste the Google OAuth client id/secret used for Drive sign-in.
void MainWindow::openCloudClientSetup()
{
    showPanel(tr("Sign-in client"), [this](QVBoxLayout* v) {
        QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        auto* intro = new QLabel(tr("Paste a Google <b>Desktop-app</b> OAuth client (from the Google Cloud "
            "console). The secret is non-confidential for desktop apps."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);

        auto* idLabel = new QLabel(tr("Client id"));
        idLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
        v->addWidget(idLabel);
        auto* idEdit = new QLineEdit(s.value(QStringLiteral("cloud/clientId")).toString());
        idEdit->setMinimumHeight(34);
        idEdit->setPlaceholderText(tr("…apps.googleusercontent.com"));
        v->addWidget(idEdit);

        auto* secLabel = new QLabel(tr("Client secret"));
        secLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
        v->addWidget(secLabel);
        auto* secEdit = new QLineEdit(s.value(QStringLiteral("cloud/clientSecret")).toString());
        secEdit->setMinimumHeight(34);
        secEdit->setPlaceholderText(tr("GOCSPX-…"));
        v->addWidget(secEdit);

        auto* err = new QLabel(); err->setStyleSheet(QStringLiteral("color:#c0392b;font-size:13px;"));
        v->addWidget(err);

        auto* save = panelRow(tr("Save"));
        connect(save, &QPushButton::clicked, this, [this, idEdit, secEdit, err] {
            const QString id = idEdit->text().trimmed();
            if (id.isEmpty()) { err->setText(tr("Enter a client id.")); return; }
            QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
            s.setValue(QStringLiteral("cloud/clientId"), id);
            s.setValue(QStringLiteral("cloud/clientSecret"), secEdit->text().trimmed());
            s.sync();
            cloud_->signOut();    // a new client invalidates the old sign-in
            openCloudSync();      // back to the sync panel (now configured)
        });
        v->addWidget(save);
    }, [this] { openCloudSync(); });
}

// Conflict-aware sync: pull when the cloud is newer, push when this device has changes, prompt when both.
void MainWindow::cloudSyncNow()
{
    if (!cloud_ || !cloud_->isSignedIn()) return;
    statusBar()->showMessage(tr("Syncing with Google Drive…"));
    auto pulled = [this](bool ok) { statusBar()->showMessage(ok ? tr("Downloaded cloud data — restart to apply it.")
                                                                 : tr("Sync failed."), 8000); };
    auto pushed = [this](bool ok, const QString& m) { statusBar()->showMessage(ok ? tr("Synced with Google Drive.") : m, 5000); };
    cloud_->checkStatus([this, pulled, pushed](const CloudSync::Status& st) {
        if (!st.reached) { statusBar()->showMessage(tr("Couldn't reach Google Drive."), 5000); return; }
        if (st.remoteChanged && st.localChanged)
        {
            // Inline conflict chooser (no popup): user-initiated sync, so present it as a panel page.
            showPanel(tr("Sync conflict"), [this, st, pulled, pushed](QVBoxLayout* v) {
                auto* msg = new QLabel(tr("The cloud has newer changes from another device, and this device "
                                          "also has unsynced changes. Choose which to keep."));
                msg->setWordWrap(true); msg->setStyleSheet(QStringLiteral("font-size:15px;"));
                v->addWidget(msg);
                auto* useCloud = panelRow(tr("Use cloud data — replace this device"));
                auto* keepThis = panelRow(tr("Keep this device — overwrite the cloud"));
                connect(useCloud, &QPushButton::clicked, this, [this, st, pulled] {
                    cloud_->applyRemote(st.fileId, st.modifiedIso, st.remoteHash, pulled); openCloudSync();
                });
                connect(keepThis, &QPushButton::clicked, this, [this, pushed] {
                    cloud_->pushLocal(pushed); openCloudSync();
                });
                v->addWidget(useCloud); v->addWidget(keepThis);
            }, [this] { openCloudSync(); });
        }
        else if (st.remoteChanged)               cloud_->applyRemote(st.fileId, st.modifiedIso, st.remoteHash, pulled);
        else if (st.localChanged || !st.hasRemote) cloud_->pushLocal(pushed);
        else                                     statusBar()->showMessage(tr("Already up to date."), 4000);
    });
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    persistAudiobook(); // flush the current audiobook position before anything else on exit
    // On the way out: push this device's changes, but don't clobber a newer cloud unless the user says so.
    if (cloud_ && cloud_->isSignedIn())
    {
        QEventLoop loop;
        QTimer::singleShot(8000, &loop, &QEventLoop::quit); // never let quitting hang on the network
        cloud_->checkStatus([this, &loop](const CloudSync::Status& st) {
            if (!st.reached) { loop.quit(); return; }
            if (st.remoteChanged && st.localChanged)
            {
                QMessageBox box(QMessageBox::Warning, tr("Sync conflict"),
                    tr("The cloud has newer changes from another device, and this device has unsynced changes.\n\n"
                       "Keep this device's data (overwrite the cloud), or discard this device's changes?"), QMessageBox::NoButton, this);
                QPushButton* keep = box.addButton(tr("Keep this device"), QMessageBox::AcceptRole);
                box.addButton(tr("Discard mine"), QMessageBox::RejectRole);
                box.exec();
                if (box.clickedButton() == keep) cloud_->pushLocal([&loop](bool, const QString&) { loop.quit(); });
                else loop.quit(); // discard local; next startup pulls the cloud
            }
            else if (st.localChanged && !st.remoteChanged)
                cloud_->pushLocal([&loop](bool, const QString&) { loop.quit(); });
            else
                loop.quit(); // nothing to push (or cloud is newer with no local edits -> startup will pull)
        });
        loop.exec();
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::openThemes()
{
    showPanel(tr("Theme"), [this](QVBoxLayout* v) {
        const QString cur = ThemeStore::currentName();
        for (const Theme& t : ThemeStore::all())
        {
            const bool active = (t.name.compare(cur, Qt::CaseInsensitive) == 0);
            auto* b = panelRow(active ? (t.name + QStringLiteral("   ✓")) : t.name);
            connect(b, &QPushButton::clicked, this, [this, name = t.name] {
                ThemeStore::setCurrent(name);
                home_->applyTheme();
                openThemes(); // rebuild to move the ✓
            });
            v->addWidget(b);
        }
        auto* browse = panelRow(tr("Browse Themes…"));
        connect(browse, &QPushButton::clicked, this, [this] {
            auto* rb = new RegistryBrowser(RegistryBrowser::Themes, nullptr, this);
            showDialogPanel(tr("Browse Themes"), rb,
                            [this](int) { home_->applyTheme(); openThemes(); }, // refresh after installs
                            [this] { openThemes(); });
        });
        v->addWidget(browse);
    }, [this] { openSettingsHub(); });
}

void MainWindow::openEmulatorSettings()
{
    // The dialog loads cores headlessly to read their options; only one libretro core can be live at a
    // time (the C ABI routes through a global), so stop any running game first.
    retro_->stop();
    auto* dlg = new SettingsDialog(this);
    showDialogPanel(tr("Emulator Settings"), dlg, [this](int) { openSettingsHub(); },
                    [this] { openSettingsHub(); });
}

void MainWindow::openInputMapping()
{
    // Stop the game so the remap dialog has sole use of the controller (for "press a button" capture).
    retro_->stop();
    auto* dlg = new ControllerRemapDialog(retro_->gamepad(), retro_->keymap(), this);
    showDialogPanel(tr("Input Mapping"), dlg, [this](int) { openSettingsHub(); },
                    [this] { openSettingsHub(); });
}

void MainWindow::onDuration(double seconds)
{
    duration_ = seconds;
    // Resume an audiobook where the listener left off, now that the file is loaded and its length is known.
    if (!audiobookPath_.isEmpty() && audiobookResume_ > 1.0 && audiobookResume_ < seconds - 1.0)
        player_->setPosition(audiobookResume_);
    audiobookResume_ = 0.0; // one-shot
}

void MainWindow::onPosition(double seconds)
{
    if (!sliderDown_ && duration_ > 0.0)
        seek_->setValue(static_cast<int>(seconds / duration_ * 1000.0));
    time_->setText(fmt(seconds) + QStringLiteral(" / ") + fmt(duration_));

    audioPos_ = seconds;
    // Throttle resume writes so we're not hammering the ini every position tick.
    if (!audiobookPath_.isEmpty() && std::abs(seconds - lastSavedPos_) >= 5.0)
        persistAudiobook();
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
