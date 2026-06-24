#include "MainWindow.h"
#include "../video/MpvWidget.h"
#include "../emu/RetroView.h"
#include "../ebook/EbookView.h"
#include "../pdf/PdfView.h"
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
#include <QPushButton>
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
    auto* rewind = new QPushButton(tr("⏪"), mediaControls_);
    auto* playPause = new QPushButton(tr("⏯"), mediaControls_);
    auto* fastFwd = new QPushButton(tr("⏩"), mediaControls_);
    auto* stop = new QPushButton(tr("⏹"), mediaControls_);
    auto* subsBtn = new QPushButton(tr("CC"), mediaControls_);
    auto* subLoad = new QPushButton(tr("＋Sub"), mediaControls_);
    auto* fullScreen = new QPushButton(tr("⛶"), mediaControls_);
    rewind->setToolTip(tr("Rewind 10s"));
    playPause->setToolTip(tr("Play / Pause"));
    fastFwd->setToolTip(tr("Forward 10s"));
    stop->setToolTip(tr("Stop"));
    subsBtn->setToolTip(tr("Subtitles: cycle tracks / off"));
    subLoad->setToolTip(tr("Load a subtitle file…"));
    fullScreen->setToolTip(tr("Toggle full screen (F11)"));
    seek_ = new QSlider(Qt::Horizontal, mediaControls_);
    seek_->setRange(0, 1000);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), mediaControls_);
    mc->addWidget(rewind);
    mc->addWidget(playPause);
    mc->addWidget(fastFwd);
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
    connect(library_, &LibraryView::homeRequested, this, &MainWindow::openHome);
    connect(rewind, &QPushButton::clicked, this, [this] { player_->seekRelative(-10.0); revealMediaControls(); });
    connect(fastFwd, &QPushButton::clicked, this, [this] { player_->seekRelative(10.0); revealMediaControls(); });
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
    const QStringList sel = QFileDialog::getOpenFileNames(
        this, tr("Open Audio"), QString(),
        tr("Audio (*.mp3 *.flac *.ogg *.opus *.wav *.m4a *.aac *.wma *.alac *.aiff *.aif *.ape *.mka);;"
           "All files (*.*)"));
    if (sel.isEmpty()) return;

    if (sel.size() == 1) { openAudioPath(sel.first()); return; } // folder queue starting at this track

    retro_->stop();
    book_->persist();
    pdf_->persist();
    setAudioQueue(sel, 0); // exactly the selected tracks, in the order the dialog returned them
    const QString first = sel.first();
    RecentStore::add({ first, QFileInfo(first).completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::openAudioPath(const QString& path)
{
    // Play the whole containing folder, sorted, starting at this file (the single-select behavior).
    const QFileInfo fi(path);
    QStringList filters;
    for (const QString& ext : kAudioExts) filters << QStringLiteral("*.") + ext;
    const QFileInfoList entries = QDir(fi.absolutePath()).entryInfoList(filters, QDir::Files, QDir::Name);
    QStringList queue;
    for (const QFileInfo& e : entries) queue << e.absoluteFilePath();
    int start = queue.indexOf(fi.absoluteFilePath());
    if (start < 0) { queue = { fi.absoluteFilePath() }; start = 0; }

    retro_->stop();
    book_->persist();
    pdf_->persist();
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
    openGamePath(rom);
}

void MainWindow::openGamePath(const QString& rom)
{
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
    {
        stack_->setCurrentWidget(retro_);
        RecentStore::add({ rom, QFileInfo(rom).completeBaseName(), QStringLiteral("game"), QString() });
    }
    else
        QMessageBox::warning(this, tr("Can't run game"), err);
}

void MainWindow::openDocument()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Document"), QString(),
        tr("Documents (*.epub *.pdf);;EPUB books (*.epub);;PDF documents (*.pdf);;All files (*.*)"));
    if (f.isEmpty()) return;
    openDocumentPath(f);
}

void MainWindow::openDocumentPath(const QString& f)
{
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
    const Profile before = ProfileStore::current();
    ProfileDialog dlg(/*mustChoose*/ false, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedId().isEmpty())
        ProfileStore::setCurrent(dlg.selectedId());

    const Profile after = ProfileStore::current();
    if (after.id != before.id)
        openHome(); // switched user (or a deletion repointed "current") -> full refresh + this user's recents
    else if (after.name != before.name || after.icon != before.icon)
        home_->refresh(); // edited the active profile -> update the avatar / name button
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

void MainWindow::openSettingsHub()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Settings"));
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(tr("Choose a settings area:"), &dlg));

    auto* general = new QPushButton(tr("General…"), &dlg);
    auto* theme = new QPushButton(tr("Theme…"), &dlg);
    auto* emu = new QPushButton(tr("Emulator Settings…"), &dlg);
    auto* inp = new QPushButton(tr("Input Mapping…"), &dlg);
    auto* addon = new QPushButton(tr("Addon Settings…"), &dlg);
    auto* cloud = new QPushButton(tr("Cloud Sync…"), &dlg);
    connect(general, &QPushButton::clicked, this, [this, &dlg] { dlg.accept(); openGeneralSettings(); });
    connect(theme, &QPushButton::clicked, this, [this, &dlg] { dlg.accept(); openThemes(); });
    connect(emu, &QPushButton::clicked, this, &MainWindow::openEmulatorSettings);
    connect(inp, &QPushButton::clicked, this, &MainWindow::openInputMapping);
    connect(addon, &QPushButton::clicked, this, [this, &dlg] { dlg.accept(); openLibrary(); });
    connect(cloud, &QPushButton::clicked, this, [this, &dlg] { dlg.accept(); openCloudSync(); });
    v->addWidget(general);
    v->addWidget(theme);
    v->addWidget(emu);
    v->addWidget(inp);
    v->addWidget(addon);
    v->addWidget(cloud);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);
    dlg.exec();
}

void MainWindow::openGeneralSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("General Settings"));
    dlg.resize(380, 200);
    auto* v = new QVBoxLayout(&dlg);

    v->addWidget(new QLabel(tr("<b>Subtitles</b>"), &dlg));
    auto* on = new QCheckBox(tr("Show subtitles by default"), &dlg);
    on->setChecked(Settings::subtitlesOnByDefault());
    v->addWidget(on);

    auto* langRow = new QHBoxLayout();
    langRow->addWidget(new QLabel(tr("Default language:"), &dlg));
    auto* lang = new QComboBox(&dlg);
    // (display name, ISO 639 code). Empty code = no preference / first available track.
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
    if (!found && !cur.isEmpty()) lang->addItem(tr("%1 (custom)").arg(cur), cur); // keep an unusual stored code
    lang->setCurrentIndex(qMax(0, lang->findData(cur)));
    lang->setEditable(true); // also allow typing any ISO 639 code (e.g. "swe")
    lang->setEnabled(on->isChecked());
    connect(on, &QCheckBox::toggled, lang, &QComboBox::setEnabled);
    langRow->addWidget(lang, 1);
    v->addLayout(langRow);

    v->addWidget(new QLabel(tr("<span style='color:#888;font-size:11px;'>Applies to the next video. Subtitles "
        "still toggle in-player with the CC button.</span>"), &dlg));
    v->addStretch(1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);

    if (dlg.exec() == QDialog::Accepted)
    {
        Settings::setSubtitlesOnByDefault(on->isChecked());
        // If the text matches a named language, store its code; otherwise treat the text as a typed code.
        const int idx = lang->findText(lang->currentText());
        const QString code = (idx >= 0) ? lang->itemData(idx).toString() : lang->currentText().trimmed();
        Settings::setSubtitleLanguage(code);
    }
}

void MainWindow::openCloudSync()
{
    if (!cloud_) cloud_ = std::make_unique<CloudSync>(this);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Cloud Sync"));
    dlg.resize(440, 250);
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(tr("<b>Google Drive sync</b><br>Back up your profiles, history, favourites, settings "
        "and local add-ons to a “MyMediaVault” folder on your Google Drive, to sync between devices."), &dlg));
    auto* status = new QLabel(&dlg); status->setWordWrap(true); status->setTextFormat(Qt::RichText);
    v->addWidget(status);
    auto* signIn = new QPushButton(tr("Sign in with Google"), &dlg);
    auto* syncNow = new QPushButton(tr("Sync now"), &dlg);
    auto* signOut = new QPushButton(tr("Sign out"), &dlg);
    auto* setup = new QPushButton(tr("Set up sign-in…"), &dlg);
    v->addWidget(signIn); v->addWidget(syncNow); v->addWidget(signOut); v->addWidget(setup);

    auto refresh = [this, status, signIn, syncNow, signOut, setup] {
        const bool cfg = CloudSync::isConfigured();
        const bool in = cloud_->isSignedIn();
        setup->setText(cfg ? tr("Change sign-in client…") : tr("Set up sign-in…")); // always available
        signIn->setVisible(cfg && !in);
        syncNow->setVisible(in);
        signOut->setVisible(in);
        if (!cfg) status->setText(tr("Google sign-in isn’t set up yet. Click “Set up sign-in…” to paste your "
                                     "OAuth client id + secret (use a “Desktop app” client)."));
        else if (in) status->setText(tr("Signed in as <b>%1</b>.").arg(cloud_->accountEmail().toHtmlEscaped()));
        else status->setText(tr("Not signed in. Client configured — click “Sign in with Google”."));
    };
    refresh();

    connect(signIn, &QPushButton::clicked, &dlg, [this, status] { status->setText(tr("Opening your browser…")); cloud_->signIn(); });
    connect(signOut, &QPushButton::clicked, &dlg, [this] { cloud_->signOut(); });
    connect(setup, &QPushButton::clicked, &dlg, [this, &dlg, refresh] {
        QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        bool ok = false;
        const QString id = QInputDialog::getText(&dlg, tr("OAuth client id"),
            tr("Google OAuth client id (Desktop-app type):"), QLineEdit::Normal,
            s.value(QStringLiteral("cloud/clientId")).toString(), &ok);
        if (!ok || id.trimmed().isEmpty()) return;
        const QString sec = QInputDialog::getText(&dlg, tr("OAuth client secret"), tr("Google OAuth client secret:"),
            QLineEdit::Normal, s.value(QStringLiteral("cloud/clientSecret")).toString(), &ok);
        if (!ok) return;
        s.setValue(QStringLiteral("cloud/clientId"), id.trimmed());
        s.setValue(QStringLiteral("cloud/clientSecret"), sec.trimmed());
        s.sync();
        cloud_->signOut(); // the stored token (if any) was for the old client; start clean
        refresh();
    });
    connect(syncNow, &QPushButton::clicked, &dlg, [this, status] { status->setText(tr("Syncing…")); cloudSyncNow(); });
    connect(cloud_.get(), &CloudSync::signedIn, &dlg, [this, refresh](const QString&) {
        refresh();
        raise(); activateWindow();   // bring the app back in front of the browser tab
        cloudSyncNow();              // first sync right after signing in
    });
    connect(cloud_.get(), &CloudSync::signInFailed, &dlg, [status](const QString& e) { status->setText(tr("Sign-in failed: %1").arg(e)); });
    connect(cloud_.get(), &CloudSync::signedOut, &dlg, [refresh] { refresh(); });

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);
    dlg.exec();
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
            QMessageBox box(QMessageBox::Warning, tr("Sync conflict"),
                tr("The cloud has newer changes from another device, and this device has unsynced changes."), QMessageBox::NoButton, this);
            QPushButton* useCloud = box.addButton(tr("Use cloud data"), QMessageBox::AcceptRole);
            box.addButton(tr("Keep this device"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == useCloud) cloud_->applyRemote(st.fileId, st.modifiedIso, pulled);
            else                                 cloud_->pushLocal(pushed);
        }
        else if (st.remoteChanged)               cloud_->applyRemote(st.fileId, st.modifiedIso, pulled);
        else if (st.localChanged || !st.hasRemote) cloud_->pushLocal(pushed);
        else                                     statusBar()->showMessage(tr("Already up to date."), 4000);
    });
}

void MainWindow::closeEvent(QCloseEvent* e)
{
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
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Theme"));
    dlg.resize(320, 380);
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(tr("Choose a theme:"), &dlg));

    auto* list = new QListWidget(&dlg);
    auto repopulate = [list] {
        list->clear();
        const QString cur = ThemeStore::currentName();
        for (const Theme& t : ThemeStore::all())
        {
            auto* it = new QListWidgetItem(t.name, list);
            if (t.name.compare(cur, Qt::CaseInsensitive) == 0) list->setCurrentItem(it);
        }
    };
    repopulate();
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    v->addWidget(list, 1);

    auto* browse = new QPushButton(tr("Browse Themes…"), &dlg);
    connect(browse, &QPushButton::clicked, &dlg, [this, &dlg, repopulate] {
        RegistryBrowser rb(RegistryBrowser::Themes, nullptr, &dlg);
        rb.exec();
        repopulate(); // a newly downloaded theme appears in the list
    });
    v->addWidget(browse);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);

    if (dlg.exec() == QDialog::Accepted && list->currentItem())
    {
        ThemeStore::setCurrent(list->currentItem()->text());
        home_->applyTheme();
    }
}

void MainWindow::openEmulatorSettings()
{
    // The dialog loads cores headlessly to read their options; only one libretro core can be live at a
    // time (the C ABI routes through a global), so stop any running game first.
    retro_->stop();
    SettingsDialog dlg(this);
    dlg.exec();
}

void MainWindow::openInputMapping()
{
    // Stop the game so the remap dialog has sole use of the controller (for "press a button" capture).
    retro_->stop();
    ControllerRemapDialog dlg(retro_->gamepad(), retro_->keymap(), this);
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
