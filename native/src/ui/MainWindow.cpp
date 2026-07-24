#include "MainWindow.h"
#include "Notifier.h"
#include "BlackFrameWatchdog.h"
#include "FeedbackPolicy.h"   // kFeedbackShort/Long — feedback duration policy (J08/J10/J11)
#include "../media/StreamResolver.h"
#include "../media/PlaybackSession.h"
#include "../launch/GameLauncher.h"
#include "../core/AppPaths.h"
#include "../video/MpvWidget.h"
#include "../emu/RetroView.h"
#include "../ebook/EbookView.h"
#include "../pdf/PdfView.h"
#include "../comic/ComicView.h"
#include "LibraryView.h"
#include "HomeView.h"
#include "SplitView.h"
#include "MediaPane.h"
#include "../core/Achievements.h"
#include "ControllerRemapDialog.h"
#include "../addons/AddonManager.h"
#include "../addons/AddonContext.h"
#include "../addons/CatalogPrefetcher.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/LocalLibrary.h"
#include "../core/LocalResolveCache.h"
#include "../core/CatalogResolver.h"
#include "../core/SyncOffsets.h"
#include "../core/BackgroundMusic.h"
#include "../core/CoreManager.h"
#include "../core/ArchiveRom.h"
#include <QDirIterator>
#include "../core/EmulatorRegistry.h"
#include "../core/EmulatorManager.h"
#include "../core/AppUpdater.h"
#include "../core/SubtitleFetcher.h"
#include "../core/CastManager.h"
#include "../core/TraktClient.h"
#include "../core/RecentStore.h"
#include "../core/SteamLibrary.h"
#include "../core/EpicLibrary.h"
#include "../core/GogLibrary.h"
#include "../core/ExternalPlayer.h"
#include "../core/PcGameStore.h"
#include "../core/DownloadsStore.h"
#include "../core/FavoritesStore.h"
#include "../core/DownloadManager.h"
#include "../core/PlayStats.h"
#include "../core/RomLibrary.h"
#include "../core/BiosCatalog.h"
#include "../core/ProfileStore.h"
#include "../core/OnboardingRoute.h"
#include "../core/ItemMarks.h"
#include "../core/ConsumptionStats.h"
#include "../core/Theme.h"
#include "../core/CloudSync.h"
#include "../core/CloudMerge.h"
#include "ProfileDialog.h"
#include "RegistryBrowser.h"
#include "../core/MetaCache.h"
#include "../core/PerfTrace.h"
#include "../core/UiTestServer.h"
#include "nav/Nav.h"
#include "nav/NavGraph.h"
#include "nav/NavOverlay.h"
#include "../core/PlaylistStore.h"
#include "nav/Osk.h"
#include "../theme2/FormFactor.h"
#include <QSettings>
#include <QSet>
#include <QLineEdit>
#include <QUrl>
#include <QDesktopServices>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QAbstractSpinBox>
#include <QScrollBar>
#include "SettingsDialog.h"
#include "../libretro/LibretroCore.h"   // themed core-options editor reads CoreOption headlessly (B2 Task 5)

#include <QWidget>
#include <QStackedWidget>
#include <QSplitter>
#include <QListWidget>
#include <QFrame>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QEventLoop>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QApplication>
#include <QGuiApplication>
#include <QSize>
#include <QWindow>
#include <QPointer>
#include <QImage>
#include <QScreen>
#include <QPointingDevice>                    // uitest `touch`: the synthetic touchscreen device
#include <qpa/qwindowsysteminterface.h>       // uitest `touch`: REAL touch delivery (real hit-testing)
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QAbstractButton>
#include <QSlider>
#include <QLabel>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QShortcut>
#include <QKeyEvent>
#include <QTouchEvent>
#include <QFileDialog>
#include <QMenu>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QHash>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStatusBar>
#include <QChar>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <cmath>
#include <cstring>
#include <memory>
#include <algorithm>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "miniz.h"

// PanelRow is a pure Qt-Core POD (no QML/Quick deps) and the SHARED row descriptor for both the themed panel
// host AND the classic showPanel fallback (e.g. openStats), so its header must be visible in a no-QML build too —
// keep this include OUTSIDE the MMV_HAVE_QML guard or the classic path fails to compile (pre-existing, fixed here).
#include "../theme2/PanelModel.h"

#ifdef MMV_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include "../theme2/ReaderChromeHost.h"
#include "../theme2/ThemedPanelHost.h"
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFrame>
#include <QFileSystemWatcher>
#include <QLineEdit>
#endif

// Win32 (PC-game installers): launch the setup via the shell so it can elevate (UAC), get a process handle,
// and monitor it to completion. Included last so <windows.h>'s macros don't clobber the Qt headers above.
#ifdef Q_OS_WIN
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shellapi.h>
  #include <QWinEventNotifier>
#endif

// Starts the catalog prefetcher only once the main window has actually painted its first frame. Paint-gated
// (not a plain singleShot(0), which can fire before the native paint arrives) AND then deferred one more
// event-loop pass, so the sweep's very first request always timestamps AFTER startup.firstpaint ends — under
// MMV_PERF, main's FirstPaintProbe ends that span in the same Paint event, and posting the kick guarantees it
// runs strictly after. Installed unconditionally on first show; removes itself and self-destructs once fired.
class PrefetchPaintKick : public QObject
{
public:
    PrefetchPaintKick(QWidget* win, CatalogPrefetcher* pf) : QObject(win), win_(win), pf_(pf) {}
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Paint)
            if (auto* w = qobject_cast<QWidget*>(o); w && w->window() == win_ && pf_)
            {
                CatalogPrefetcher* pf = pf_;
                QTimer::singleShot(0, pf, [pf] { pf->start(); }); // pf is the context object -> safe if it dies
                qApp->removeEventFilter(this);
                deleteLater();
            }
        return false;
    }
private:
    QWidget* win_;
    QPointer<CatalogPrefetcher> pf_;
};

// One-line append to <app>/stream_debug.log, shared with the addon stream/manga resolution tracing.
static void mwLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A log-safe rendering of a URL: scheme://host[:port]/…/<filename>. Drops the path's middle segments (which
// can carry an addon access token) and the query string (which can carry debrid keys), so logs never leak secrets.
static QString logSafeUrl(const QString& url)
{
    const QUrl u(url);
    if (u.scheme().isEmpty()) return QFileInfo(url).fileName(); // a local path
    const QString file = QFileInfo(u.path()).fileName();
    return u.scheme() + QStringLiteral("://") + u.host()
         + (u.port() > 0 ? QStringLiteral(":") + QString::number(u.port()) : QString())
         + QStringLiteral("/…/") + file;
}

// Audio extensions, shared by the open dialog filter and folder-queue scanning. (m4b = MP4/AAC audiobooks.)
static const QStringList kAudioExts = {
    "mp3", "flac", "ogg", "opus", "wav", "m4a", "m4b", "aac", "wma", "alac", "aiff", "aif", "ape", "mka"
};

// Per-profile settings store (resume positions, etc.), mirroring the accessor the other views use.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

static QPushButton* panelRow(const QString& label); // large TV-friendly menu row (defined below)

MainWindow::MainWindow(bool chooseProfileAtStart, QWidget* parent)
    : QMainWindow(parent), startupChooseProfile_(chooseProfileAtStart)
{
    // The image-cache cap may evict browsed posters, but never art of downloaded/favorited items (their
    // shelves must keep rendering offline). Queried lazily, only when an eviction actually runs.
    MetaCache::setPinnedKeysProvider([] {
        QSet<QString> keys = FavoritesStore::allKeys();
        for (const DownloadedItem& d : DownloadsStore::list())
        {
            if (!d.key.isEmpty())  keys.insert(d.key);
            if (!d.path.isEmpty()) keys.insert(d.path);
        }
        return keys;
    });

    // No standalone Settings-read block in this ctor; this brackets the initial subsystem construction that
    // reads persisted state (achievements stored token below, subtitle prefs, saved volume follows later).
    PerfTrace::begin(QStringLiteral("startup.settings"));
    player_ = new MpvWidget(this);
    retro_ = new RetroView(this);
    if (retro_->gamepad()) mwLog(QString::fromStdString(retro_->gamepad()->describeControllers()));

    // Android OS lifecycle: when the app is backgrounded, freeze a running core / playing video (battery +
    // audio focus), and resume ONLY what we froze on return (onApplicationStateChanged -> LifecyclePolicy).
    // Android-only: on desktop, applicationStateChanged fires on every alt-tab, which must NOT pause playback.
    // The handler itself is unguarded so a probe/test can drive it directly.
#ifdef Q_OS_ANDROID
    connect(qApp, &QGuiApplication::applicationStateChanged,
            this, &MainWindow::onApplicationStateChanged);
#endif
    book_ = new EbookView(this);
    pdf_ = new PdfView(this);
    comic_ = new ComicView(this);

    // Auto-subtitle download: when an eligible video finishes loading with no subtitle in the preferred
    // language, fetch one from OpenSubtitles and hand it to the player. Dormant unless the user configured
    // OpenSubtitles credentials and enabled "show subtitles by default".
    trakt_ = new TraktClient(this);
    connect(trakt_, &TraktClient::log, this, [this](const QString& l) { mwLog(l); });

    castMgr_ = new CastManager(this);
    connect(castMgr_, &CastManager::castStarted, this, [this](const QString& name) {
        notify(tr("Casting to %1.").arg(name), 4000); });
    connect(castMgr_, &CastManager::castError, this, [this](const QString& msg) { notify(msg, kFeedbackLong); });
    connect(castMgr_, &CastManager::castStopped, this, [this] { notify(tr("Stopped casting."), 3000); });

    subFetcher_ = new SubtitleFetcher(this);
    connect(subFetcher_, &SubtitleFetcher::log, this, [this](const QString& line) { mwLog(line); });
    connect(player_, &MpvWidget::fileLoaded, this, [this](bool hasSub, bool isVideo) {
        PerfTrace::end(QStringLiteral("open.video")); // one of these two is the live span, the other an orphan no-op
        PerfTrace::end(QStringLiteral("open.audio"));
        // Consumption-stats category is NOT taken from mpv's isVideo here: an audio file with embedded cover art
        // registers a video track (and the main MpvWidget displays it, so width>0 too), which would misfile a
        // "listen" as a "watch". The session's kind is instead stamped by the APP at each open site (openVideoPath
        // / playStream / IPTV queue = video; openAudio* = audio) — deterministic, no mpv cover-art quirk. See
        // setSessionMediaKind() calls below.
        // Apply the resolved audio/subtitle sync offsets for this file (per-file override if saved, else the
        // global default). Every play path set syncKey_ beside its beginResume(); this is the single choke point
        // after load that covers them all. Re-applying each file also resets mpv's global audio-delay/sub-delay
        // properties so an offset from the previous file never bleeds into an un-offset one.
        const auto off = SyncOffsets::resolve(syncKey_);
        player_->setAudioDelay(off.audio);
        player_->setSubtitleDelay(off.sub);
        if (speedBtn_) speedBtn_->setText(QString::number(player_->speed(), 'g', 3) + QStringLiteral("×")); // reset to 1× per file
        // Themed audio page: the newly-loaded file plays (not paused); refresh its play button + speed + progress.
#ifdef MMV_HAVE_QML
        if (themedAudioSession_) { themedAudioPaused_ = false; themedAudioPushSec_ = -1; updateThemedAudioProgress(); }
#endif
        if (!subCtx_.active) return;
        subCtx_.active = false; // one-shot per open
        if (hasSub || !isVideo) return;
        if (subCtx_.imdbStreamId.isEmpty() && subCtx_.title.isEmpty()) return;
        subFetcher_->fetch(subCtx_.imdbStreamId, subCtx_.title, Settings::subtitleLanguage(),
                           [this](const QString& srt) {
            if (!srt.isEmpty()) player_->addSubtitle(srt);
        });
    });

    // RetroAchievements: one client, attached to the full-screen emulator. Logs in silently if a token was
    // saved, and announces unlocks. Split-screen panes don't participate (one active game at a time).
    ach_ = new Achievements(this);
    retro_->setAchievements(ach_);
    connect(ach_, &Achievements::achievementUnlocked, this,
            [this](const QString& title, const QString& desc, int pts, const QString& badge) {
        // J08: the unlock's visible channel is retro_->showAchievement (an on-screen popup over the emulator).
        // The old statusBar() echo sat under the full-screen game surface and was never seen — dropped here as
        // redundant. The gameLoaded summaries below have no such popup, so they route through the window-level
        // notify() overlay ("over ANY view"), NOT playerNotice (playerNotice_ is parented to the mpv player,
        // which is hidden during a RetroView game — so it'd be invisible exactly when achievements fire).
        retro_->showAchievement(title, desc, pts, badge); // on-screen popup over the game (full-screen has no status bar)
    });
    connect(ach_, &Achievements::gameLoaded, this, [this](bool ok, const QString& title, int unlocked, int total) {
        if (ok && total > 0)
            notify(tr("🏆  %1 — %2/%3 achievements").arg(title).arg(unlocked).arg(total), kFeedbackLong);
        else if (ok && title.contains(QStringLiteral("Unsupported Game Version"), Qt::CaseInsensitive))
            // RA recognized the ROM but it isn't the dump the achievement set is tied to — tell the user why
            // there are no achievements so they know to grab the RetroAchievements-supported (No-Intro) dump.
            notify(tr("🏆  This ROM isn't a RetroAchievements-supported version — no achievements. "
                      "Try the No-Intro/RA-verified dump."), kFeedbackLong);
        else if (ok)
            notify(tr("🏆  No RetroAchievements set for this game."), kFeedbackLong);
    });
    ach_->tryLoginWithStoredToken();
    PerfTrace::end(QStringLiteral("startup.settings"));

    // AddonManager's ctor performs the initial addon load (reload() off disk); this span also covers the
    // addon-consuming views wired up alongside it (cloud, library, downloads).
    PerfTrace::begin(QStringLiteral("startup.addons"));
    addons_ = std::make_unique<AddonManager>();
    // Background catalog warmer: sweeps every enabled source × catalog (page 1) into AddonManager's cache so a
    // menu opens instantly off the warm read path in HomeView::issueRequest. Constructed here (right after the
    // manager it wraps) but not kicked until post-first-paint — see showEvent's PrefetchPaintKick — so the
    // sweep's first request never competes with the first frame.
    prefetcher_ = new CatalogPrefetcher(addons_.get(), this);
    cloud_ = std::make_unique<CloudSync>(this); // eager: needed for push-on-exit even if the panel never opens
    library_ = new LibraryView(addons_.get(), this);
    connect(library_, &LibraryView::openItem, this, &MainWindow::openLibraryItem);
    dm_ = new DownloadManager(this);
    // A finished download joins Recent + the catalogue's Downloaded folder (offline-openable).
    connect(dm_, &DownloadManager::jobCompleted, this, [this](const DownloadJob& j) {
        RecentStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        DownloadsStore::add({ j.dest, j.title, j.kind, j.thumb, j.key, j.sysId });
        notify(tr("Downloaded “%1”.").arg(j.title), 4000);
    });
    // Live progress: update the open panel's bars/labels in place (a full rebuild would steal focus).
    connect(dm_, &DownloadManager::jobProgress, this, &MainWindow::updateDownloadRow);
    // State changes (queued/active/failed/removed) change the row SET (a job added/removed, empty-state toggling,
    // Clear-finished appearing) — and, in themed mode, the action-chooser contents — so rebuild the panel.
    connect(dm_, &DownloadManager::changed, this, [this] {
        if (dlPanelOpen_ && stack_->currentWidget() == panelPage_) openDownloadManager();
#ifdef MMV_HAVE_QML
        else if (themedPanelIsTop(tr("Downloads"))) openDownloadManager();   // themed: replaceTop (reentry) below
#endif
    });
    PerfTrace::end(QStringLiteral("startup.addons"));

    home_ = new HomeView(addons_.get(), this);
    connect(home_, &HomeView::openItem, this, &MainWindow::openLibraryItem);
    connect(home_, &HomeView::downloadItem, this, &MainWindow::enqueueDownload);
    connect(home_, &HomeView::openImagePages, this, &MainWindow::openImagePages);

    // Local Library ID-resolver: own the on-disk match cache + the background resolver (searches addons_).
    // Constructed after addons_/home_ and before the first rescanLocalLibrary(); resolved() progressively
    // rebuilds the index off-thread from the current scan + the now-richer cache, then refreshes the home.
    resolveCache_ = std::make_unique<LocalResolveCache>(AppPaths::dataDir() + QStringLiteral("/localresolve.json"));
    resolveCache_->load();
    resolver_ = std::make_unique<CatalogResolver>(addons_.get(), resolveCache_.get(), this);
    connect(resolver_.get(), &CatalogResolver::resolved, this, [this] {
        // A batch of movies resolved: rebuild the index from the current scan + the now-richer cache, then refresh.
        const QString libRoot = LocalLibrary::root();
        const auto extra = resolveCache_->matchedIdsByPath();       // snapshot on the MAIN thread (thread-safe by value)
        const quint64 gen = libScanGen_;                            // READ (do not ++) — a refresh, not a superseding scan
        auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
        connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w, gen] {
            if (gen == libScanGen_) {                               // a newer folder-change rescan invalidates this stale rebuild
                LocalLibrary::installIndex(w->result());
                if (home_) home_->onLocalLibraryChanged();
            }
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([libRoot, extra] {
            return LocalLibrary::buildIndex(LocalLibrary::scanFolder(libRoot), extra);
        }));
    });

    // The player page pairs the libmpv surface with a playlist panel (shown only for audio queues).
    playlist_ = new QListWidget(this);
    playlist_->setVisible(false);
    playlist_->setMinimumWidth(180); // stay readable when the splitter shows it
    connect(playlist_, &QListWidget::itemActivated, this,
            [this] { session_->playIndex(playlist_->currentRow()); });
    auto* playerPage = new QSplitter(Qt::Horizontal, this);
    playerPage->addWidget(playlist_);
    playerPage->addWidget(player_);
    playerPage->setStretchFactor(1, 1);
    playerPage->setSizes({ 260, 900 });
    playerPage_ = playerPage;

    stack_ = new QStackedWidget(this);
    stack_->addWidget(playerPage); // index 0 - video / audio
    stack_->addWidget(retro_);     // index 1 - games
#ifdef MMV_HAVE_QML
    // The ebook reader is wrapped in the themed chrome host: it reparents book_ inside itself and adds themed
    // strips over it in themed mode; in classic mode it's a transparent passthrough (book_ shows its own
    // chrome). The stack page is the host, not book_ directly (index 2 - ebooks).
    readerHost_ = new ReaderChromeHost(book_, ReaderKind::Book, this);
    stack_->addWidget(readerHost_);
#else
    stack_->addWidget(book_);      // index 2 - ebooks
#endif
#ifdef MMV_HAVE_QML
    // PDF + comic readers are wrapped in the same themed chrome host (Task 4): each reparents its reader and
    // adds themed strips in themed mode; classic mode is a transparent passthrough. The stack page is the host.
    pdfHost_ = new ReaderChromeHost(pdf_, ReaderKind::Pdf, this);
    stack_->addWidget(pdfHost_);   // index 3 - pdf (via host)
#else
    stack_->addWidget(pdf_);       // index 3 - pdf
#endif
    stack_->addWidget(library_);   // index 4 - addon library
    stack_->addWidget(home_);      // index 5 - home / catalog landing
#ifdef MMV_HAVE_QML
    comicHost_ = new ReaderChromeHost(comic_, ReaderKind::Comic, this);
    stack_->addWidget(comicHost_); // index 6 - comic (CBZ) reader (via host)
#else
    stack_->addWidget(comic_);     // index 6 - comic (CBZ) reader
#endif
#ifdef MMV_HAVE_QML
    // The themed settings-panel surface (B2): a persistent stack page rendering PanelRow lists through the Nav
    // Contract, used in themed mode instead of the classic showPanel widget panel. Classic mode never shows it.
    themedPanelHost_ = new ThemedPanelHost(this);
    themedPanelHost_->setObjectName(QStringLiteral("themedPanelHost"));
    stack_->addWidget(themedPanelHost_);
#endif

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
    panelBack_ = new QPushButton(tr("‹ Back"), panelHeader);
    panelBack_->setStyleSheet(QStringLiteral(
        "QPushButton{background:rgba(255,255,255,0.12);color:#fff;border:2px solid transparent;border-radius:8px;"
        "padding:10px 18px;font-size:16px;font-weight:bold;} QPushButton:hover{background:rgba(255,255,255,0.22);}"
        "QPushButton:focus{background:rgba(90,140,255,0.55);border:2px solid #fff;}")); // visible when arrowed-to
    panelBack_->setCursor(Qt::PointingHandCursor);
    connect(panelBack_, &QPushButton::clicked, this, [this] { if (panelOnBack_) panelOnBack_(); });
    panelTitle_ = new QLabel(panelHeader);
    panelTitle_->setStyleSheet(QStringLiteral("font-size:20px;font-weight:bold;"));
    phl->addWidget(panelBack_);
    phl->addSpacing(12);
    phl->addWidget(panelTitle_, 1);
    pv->addWidget(panelHeader);
    panelScroll_ = new QScrollArea(panelPage_);
    panelScroll_->setWidgetResizable(true);
    panelScroll_->setFrameShape(QFrame::NoFrame);
    pv->addWidget(panelScroll_, 1);
    stack_->addWidget(panelPage_); // index 7 - inline settings panels
    // The split screen (index 8) is created lazily on first use (it spins up a second set of media engines),
    // so users who never split pay nothing for it - see enterSplitScreen().

    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(stack_, 1);
    setCentralWidget(central);

    // Menu background music (RetroBat-style): plays while browsing, pauses on games/video. Follow the view.
    // Dominant contiguous theme/BGM block; themeWatcher_ is created lazily in showThemedHome() (excluded here)
    // and the themed-home QML signal wiring further down is interleaved with nav/updater setup (excluded).
    PerfTrace::begin(QStringLiteral("startup.theme"));
    bgm_ = new BackgroundMusic(this);
    connect(stack_, &QStackedWidget::currentChanged, this, [this] {
        updateBackgroundMusic();
        // Remember the full-screen state as we ENTER content (a game / video / reader), so exiting back to the
        // home restores it instead of always dropping to a window (see openHome). Content doesn't change the
        // full-screen state itself, so isFullScreen() here is the browsing state we want to come back to.
        QWidget* w = stack_->currentWidget();
        // The readers are wrapped in their themed chrome hosts, so the stack page is the host (or the bare
        // reader without QML) — treat both as content so full-screen memory works in either build.
        bool content = (w == retro_ || w == playerPage_ || w == emuPage_
                        || w == book_ || w == pdf_ || w == comic_);
#ifdef MMV_HAVE_QML
        content = content || (readerHost_ && w == readerHost_) || (pdfHost_ && w == pdfHost_)
                          || (comicHost_ && w == comicHost_);
#endif
        if (content && !inContent_) fsBeforeContent_ = isFullScreen();
        inContent_ = content;
    });
    connect(bgm_, &BackgroundMusic::nowPlayingChanged, this, [this] { updateThemedNowPlaying(); }); // Triple theme readout
    PerfTrace::end(QStringLiteral("startup.theme"));
    statusBar()->hide(); // no bottom status strip; showMessage() calls stay harmless (they don't re-show it)

    // Pull any manually-added ROMs sitting in the library folders into the Downloaded list, so they show up
    // in the home like downloaded games. Deferred so it never delays the window appearing.
    QTimer::singleShot(0, this, [] { RomLibrary::syncToDownloads(); });

    // The controller-navigation kit: routes nav keys to overlays/rings before the legacy per-view paths,
    // guarantees a live selection on ring-managed screens, and carries the per-screen Back action. The
    // panel ring covers panelPage_ (its Back button + whatever rows showPanel builds).
    navCtx_ = new NavContext(this);
    panelRing_ = new NavRing(panelPage_, this);
    connect(stack_, &QStackedWidget::currentChanged, this, [this](int) { updateNavForPage(); });
    updateNavForPage();

    // UI-test/automation channel (opt-in): see updateUiTestServer(). Created here when enabled at launch.
    updateUiTestServer();

    // Controller navigation of the menus: poll the gamepad ~60Hz and inject nav keys (see pollMenuPad). This
    // also opens a controller connected while browsing (Gamepad::poll handles hot-plug), so it works even if
    // the user never enters a game.
    padNavTimer_ = new QTimer(this);
    padNavTimer_->setInterval(16);
    connect(padNavTimer_, &QTimer::timeout, this, &MainWindow::pollMenuPad);
    padNavTimer_->start();

    // App self-update: quietly check GitHub Releases a few seconds after launch (opt-out in General settings).
    // A found update just surfaces a toast; the actual install is user-triggered from Settings ▸ General.
    updater_ = new AppUpdater(this);
    connect(updater_, &AppUpdater::updateAvailable, this, [this](const QString& ver, const QString&) {
        notify(tr("My Media Vault %1 is available — Settings ▸ General ▸ Updates to install.").arg(ver), 12000);
    });
    connect(updater_, &AppUpdater::progress, this, [this](const QString& t, int) { notify(t, 0); });
    connect(updater_, &AppUpdater::applyFailed, this, [this](const QString& why) { notify(why, kFeedbackLong); });
    if (Settings::checkUpdatesOnStartup())
        QTimer::singleShot(4000, updater_, [this] { updater_->checkForUpdate(); });

#ifdef MMV_HAVE_QML
    // Appearance (themed-home toggle + theme picker) is reachable anywhere via Ctrl+Shift+A - this is also
    // the reliable escape hatch to turn the themed home back off.
    {
        auto* sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")), this);
        connect(sc, &QShortcut::activated, this, [this] { openAppearance(); });

        // Keep the themed browse view - or the XMB home's column - mirroring HomeView as it loads / drills /
        // pages. On a page append keep the selection where it is (the user scrolled to trigger it); on a fresh
        // set reset to the top.
        connect(home_, &HomeView::browseItemsChanged, this, [this](bool appended) {
            syncThemedLevels(); // a load / drill / drill-up / page moved HomeView's stack — mirror it onto the graph
            QWidget* tgt = nullptr;
            if (themedBrowse_ && stack_->currentWidget() == themedBrowse_) tgt = themedBrowse_;
            // The XMB column mirrors HomeView only while drilled into a catalog (not on the catalog-list level).
            else if (themedHome_ && themedHomeIsXmb_ && themedXmbInCatalog_ && stack_->currentWidget() == themedHome_) tgt = themedHome_;
            if (!tgt) return;
            QQuickItem* r = ThemeEngine::rootItem(tgt);
            if (!r) return;
            const int cur = r->property("currentIndex").toInt();
            r->setProperty("items", home_->browseItems()); // rebuilds the row map used by browseRestoreIndex()
            r->setProperty("catLoading", false); // items arrived -> stop the category spinner
            // Append -> keep the selection; a fresh set -> re-select the row we drilled into (Back), else top.
            r->setProperty("currentIndex", appended ? cur : home_->browseRestoreIndex());
            if (tgt == themedBrowse_) // the XMB home keeps its "home" view + categories; only browse swaps view/title
            {
                r->setProperty("currentView", QStringLiteral("browse"));
                QVariantMap sys; sys.insert(QStringLiteral("name"), home_->browseTitle());
                r->setProperty("system", sys);
            }
            else // the XMB home column: refresh the live metadata panel for the (now current) row
                refreshThemedMeta(r->property("currentIndex").toInt());
        });

        // (The classic info-page-over-themed-home swap was retired: opening info on a themed surface now stays
        // on the Nav Contract via the themed DETAIL view — see openThemedDetail()/showThemedXmb/showThemedBrowse.
        // HomeView's infoPageRequested is left for the CLASSIC (non-themed) home only; the themed path never
        // handles it — a themed grid-browse info leaf routes to the detail view before browseActivate is called.)

        // Triple/XMB: the live metadata for a browse-item arrived (a skeleton, then synopsis + facts). Merge
        // it into the cross's metadata panel - but only if it's still the selected row of the open catalog.
        connect(home_, &HomeView::themedMetaReady, this, [this](int index, const QVariantMap& meta) {
            // While the themed DETAIL view for this row is open, merge the (async) enrichment into detailData too
            // so a synopsis/facts/art that lands after the page opened fills in live (any themed surface).
            {
                QWidget* cw = stack_->currentWidget();
                if ((cw == themedHome_ || cw == themedBrowse_) && themedDetailIndex_ == index)
                    if (QQuickItem* dr = ThemeEngine::rootItem(cw))
                        if (dr->property("currentView").toString() == QStringLiteral("detail"))
                        {
                            QVariantMap d = dr->property("detailData").toMap();
                            for (auto it = meta.constBegin(); it != meta.constEnd(); ++it)
                                if (it.key() != QStringLiteral("index")) d.insert(it.key(), it.value());
                            dr->setProperty("detailData", d);
                        }
            }
            if (!themedHomeIsXmb_ || !themedXmbInCatalog_ || stack_->currentWidget() != themedHome_) return;
            QQuickItem* r = ThemeEngine::rootItem(themedHome_);
            if (!r || r->property("currentIndex").toInt() != index) return; // moved on -> stale, ignore
            QVariantMap cur = r->property("selectedMeta").toMap();
            for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) cur.insert(it.key(), it.value());
            r->setProperty("selectedMeta", cur);
            // Per-item "theme song": duck the shuffle and loop this item's preview audio while it's hovered
            // (resume the shuffle when the selection carries none).
            if (bgm_)
            {
                const QStringList au = cur.value(QStringLiteral("audio")).toStringList();
                bgm_->setPreview(au.isEmpty() ? QString() : au.first());
            }
        });
    }
#endif
    // No persistent bottom bar: navigation lives in each view (Home's top bar, the Settings hub, the media
    // transport overlay, the emulator Esc menu, and per-view Home buttons).

    // Media transport overlay: a child of the player surface (composites over the GL video), shown only
    // while media is open and the mouse moves.
    mediaControls_ = new QFrame(player_);
    mediaControls_->setObjectName(QStringLiteral("mediaControls"));
    mediaControls_->setStyleSheet(QStringLiteral(
        "#mediaControls { background: rgba(20,20,24,0.85); border-radius: 10px; }"
        "#mediaControls QLabel { color: #e8e8e8; }"
        "#mediaControls QPushButton:focus { background: rgba(90,140,255,0.80); border-radius:6px; }")); // arrowed-to
    auto* mc = new QHBoxLayout(mediaControls_);
    mc->setContentsMargins(12, 8, 12, 8);
    auto* prevChap = new QPushButton(tr("⏮"), mediaControls_);
    auto* rewind = new QPushButton(tr("⏪"), mediaControls_);
    auto* playPause = new QPushButton(tr("⏯"), mediaControls_);
    auto* fastFwd = new QPushButton(tr("⏩"), mediaControls_);
    auto* nextChap = new QPushButton(tr("⏭"), mediaControls_);
    auto* stop = new QPushButton(tr("⏹"), mediaControls_);
    speedBtn_ = new QPushButton(tr("1×"), mediaControls_);
    auto* subsBtn = new QPushButton(tr("CC"), mediaControls_);
    auto* shotBtn = new QPushButton(tr("📷"), mediaControls_);
    auto* castBtn = new QPushButton(tr("📡"), mediaControls_);
    auto* fullScreen = new QPushButton(tr("⛶"), mediaControls_);
    prevChap->setToolTip(tr("Previous chapter"));
    rewind->setToolTip(tr("Rewind 10s"));
    playPause->setToolTip(tr("Play / Pause"));
    fastFwd->setToolTip(tr("Forward 10s"));
    nextChap->setToolTip(tr("Next chapter"));
    stop->setToolTip(tr("Stop"));
    speedBtn_->setToolTip(tr("Playback speed (click to cycle; [ and ] to adjust)"));
    subsBtn->setToolTip(tr("Audio & subtitles — pick tracks, sync, size, load or download"));
    shotBtn->setToolTip(tr("Screenshot (F12) — save the current frame"));
    castBtn->setToolTip(tr("Cast to a TV (Chromecast / DLNA)"));
    fullScreen->setToolTip(tr("Toggle full screen (F11)"));
    // Chapter nav is only meaningful for chaptered media (M4B audiobooks, some videos); hidden otherwise.
    prevChap->hide();
    nextChap->hide();
    seek_ = new QSlider(Qt::Horizontal, mediaControls_);
    seek_->setRange(0, 1000);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), mediaControls_);
    // Volume: a speaker/mute toggle + a compact slider. Remembered across sessions in the ini.
    muteBtn_ = new QPushButton(tr("🔊"), mediaControls_);
    muteBtn_->setToolTip(tr("Mute / unmute"));
    volume_ = new QSlider(Qt::Horizontal, mediaControls_);
    volume_->setRange(0, 200); // 0..200%: above 100% is software amplification ("boost"), VLC-style
    volume_->setFixedWidth(120);
    volume_->setToolTip(tr("Volume"));
    mc->addWidget(prevChap);
    mc->addWidget(rewind);
    mc->addWidget(playPause);
    mc->addWidget(fastFwd);
    mc->addWidget(nextChap);
    mc->addWidget(stop);
    mc->addWidget(seek_, 1);
    mc->addWidget(time_);
    mc->addWidget(muteBtn_);
    mc->addWidget(volume_);
    mc->addWidget(speedBtn_);
    mc->addWidget(subsBtn);
    mc->addWidget(shotBtn);
    mc->addWidget(castBtn);
    mc->addWidget(fullScreen);
    mediaControls_->hide();
    // Order for Left/Right arrow navigation across the transport (chapter buttons skipped while hidden).
    playerButtons_ = { prevChap, rewind, playPause, fastFwd, nextChap, stop, muteBtn_, speedBtn_, subsBtn, shotBtn, castBtn, fullScreen };

    // Restore the saved volume and apply it (mpv's volume is a session-global property, so it carries across
    // files). Changing the slider updates mpv + persists; the speaker button toggles mute.
    {
        QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        const int vol = s.value(QStringLiteral("player/volume"), 100).toInt();
        volume_->setValue(qBound(0, vol, 200));
        player_->setVolume(volume_->value());
        volume_->setToolTip(tr("Volume: %1%").arg(volume_->value()));
    }
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        if (muted_ && v > 0) { muted_ = false; muteBtn_->setText(QStringLiteral("🔊")); player_->setMuted(false); }
        player_->setVolume(v);
        // Speaker shows mute at 0, plain at 1..100, and a "boost" badge above 100%.
        muteBtn_->setText(v == 0 ? QStringLiteral("🔇") : v > 100 ? QStringLiteral("🔊+") : QStringLiteral("🔊"));
        volume_->setToolTip(tr("Volume: %1%").arg(v));
        QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        s.setValue(QStringLiteral("player/volume"), v);
    });
    connect(muteBtn_, &QPushButton::clicked, this, [this] {
        muted_ = !muted_;
        player_->setMuted(muted_);
        const int v = volume_->value();
        muteBtn_->setText(muted_ || v == 0 ? QStringLiteral("🔇") : v > 100 ? QStringLiteral("🔊+") : QStringLiteral("🔊"));
    });

    // Top-left "Back" overlay to exit the movie. Shown/hidden with the transport (on mouse move).
    videoBack_ = new QPushButton(tr("‹ Back"), player_);
    videoBack_->setObjectName(QStringLiteral("videoBack"));
    videoBack_->setStyleSheet(QStringLiteral(
        "#videoBack { background: rgba(20,20,24,0.85); color:#e8e8e8; border:2px solid transparent; border-radius:8px;"
        " padding:8px 16px; font-weight:bold; }"
        "#videoBack:hover { background: rgba(45,45,52,0.95); }"
        "#videoBack:focus { background: rgba(90,140,255,0.80); border:2px solid #fff; }")); // arrowed-to
    videoBack_->setCursor(Qt::PointingHandCursor);
    videoBack_->setToolTip(tr("Exit the movie"));
    videoBack_->hide();
    videoBack_->installEventFilter(this); // keep the overlay alive while hovering it
    connect(videoBack_, &QPushButton::clicked, this, [this] {
        player_->stop(); mediaControls_->hide(); videoBack_->hide(); session_->clearQueue(); openHome();
    });

    // "Issue with Streaming" overlay next to Back: asks Allarr for the next-best source for the current item
    // (movies/TV/audiobooks). Only shown when the open media came from a file provider that supports it.
    streamIssueBtn_ = new QPushButton(tr("⚠ Issue with Streaming"), player_);
    streamIssueBtn_->setObjectName(QStringLiteral("streamIssue"));
    streamIssueBtn_->setStyleSheet(QStringLiteral(
        "#streamIssue { background: rgba(20,20,24,0.85); color:#e8e8e8; border:2px solid transparent; border-radius:8px;"
        " padding:8px 16px; font-weight:bold; }"
        "#streamIssue:hover { background: rgba(45,45,52,0.95); }"
        "#streamIssue:focus { background: rgba(90,140,255,0.80); border:2px solid #fff; }"));
    streamIssueBtn_->setCursor(Qt::PointingHandCursor);
    streamIssueBtn_->setToolTip(tr("Bad or wrong file? Try the next available source."));
    streamIssueBtn_->hide();
    streamIssueBtn_->installEventFilter(this);
    connect(streamIssueBtn_, &QPushButton::clicked, this, [this] {
        notifier_->playerNotice(tr("Finding another source…"), 30000);
        home_->requestNextSource();
    });

    // The app's single user-feedback channel: a window-level notice (download/resolve progress + errors,
    // over ANY view) and a transient centred message over the player (visible in full screen).
    notifier_ = new Notifier(centralWidget(), this);
    notifier_->setPlayerHost(player_, [this]{ return 16 + videoBack_->height() + 14; });

    // .m3u/.m3u8 playlist + stream-link classification (HLS vs. IPTV/media list vs. PlayStation disc set).
    streams_ = new StreamResolver(this);
    connect(streams_, &StreamResolver::status, this,
            [this](const QString& m) { statusBar()->showMessage(m); });
    connect(streams_, &StreamResolver::playDirect, this,
            [this](const QString& url, const QString& title) { playStream(url, QString(), title); });
    connect(streams_, &StreamResolver::openDisc, this,
            [this](const QString& src, const QString& title) { openGamePath(src, title); });
    connect(streams_, &StreamResolver::playQueue, this,
            [this](const QStringList& urls, const QStringList& titles,
                   const QString& src, const QString& title) {
        // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
        currentNextSourceCapable_ = false;
        themedAudioSession_ = false; // an IPTV/channel queue is VIDEO — keep the classic player page
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
        session_->setQueue(urls, 0, titles);
        session_->setMediaVideo(true); // consumption-stats: kind AFTER setQueue (outgoing track flushes under its own kind)
        RecentStore::add({ src, title.isEmpty() ? QFileInfo(src).completeBaseName() : title,
                           QStringLiteral("video"), QString(), src });
    });

    // The audio-queue + resume state machine: owns the track list/position, tells us what to play and where
    // to resume via signals; we own the actual player + playlist widget.
    session_ = new PlaybackSession(QString(), this);
    connect(session_, &PlaybackSession::playRequested, this,
            [this](const QString& p) {
        // Per-track choke point for EVERY queue-driven load — initial track and advances alike, keyed exactly
        // as PlaybackSession::beginResume(tracks_[index]) keys the resume. Covers local audio folders/multi-select
        // and IPTV queues (the direct-play paths set their own key and never reach here). openAudioStream re-keys
        // the initial track to its stable id AFTER setQueue, so a stable id still wins track 1 while advances fall
        // through to this per-track key.
        syncKey_ = p;
        player_->play(p);
    });
    connect(session_, &PlaybackSession::queueChanged, this,
            [this](const QStringList& titles, int current) {
        themedAudioQueue_ = titles;
        themedAudioCurrent_ = current;
        // Themed-mode audio: the QML now-playing page is the surface (mpv plays invisibly) — never show the
        // classic player page. VIDEO queues (IPTV) and classic-mode audio keep the playlist_ + player page.
#ifdef MMV_HAVE_QML
        if (themedAudioSession_) { showThemedAudioPage(); pushThemedAudioQueue(); return; }
#endif
        playlist_->clear();
        for (const QString& t : titles) playlist_->addItem(t);
        playlist_->setCurrentRow(current);
        playlist_->setVisible(true);
        stack_->setCurrentWidget(playerPage_);
        revealMediaControls();
    });
    connect(session_, &PlaybackSession::trackChanged, this,
            [this](int i, int n, const QString&) {
        playlist_->setCurrentRow(i);
        themedAudioCurrent_ = i;
        themedAudioPaused_ = false;                    // a new track auto-plays
#ifdef MMV_HAVE_QML
        if (themedAudioSession_) pushThemedAudioQueue(); // move the highlighted queue row on the themed page
#endif
        statusBar()->showMessage(tr("Track %1 of %2").arg(i + 1).arg(n), 3000);
    });
    connect(session_, &PlaybackSession::queueCleared, this,
            [this] { syncKey_.clear();                     // left the media -> the card falls back to the globals
                     if (playlist_) { playlist_->clear(); playlist_->setVisible(false); } });
    connect(session_, &PlaybackSession::queueFinished, this, [this] {
        stopScrobble(); // a finished video scrobbles a stop at ~100% -> marked watched
        // PRECEDENCE: an active channel OWNS the natural end and supersedes episode-autoplay. Both hang off this
        // one EOF-gated seam (queueFinished fires ONLY on MPV_END_FILE_REASON_EOF — stop/seek are swallowed), so
        // a channel running over a TV-show playlist advances by the shuffle bag, NOT by episode order. Deferred a
        // turn so the countdown's nested event loop unwinds out of the mpv end-of-file callback first (the
        // T2 QueuedConnection precedent), and so no stray goBack/openHome can clear the flag before we read it.
        if (channelActive())
        { QMetaObject::invokeMethod(this, [this] { advanceChannel(); }, Qt::QueuedConnection); return; }
        if (Settings::autoplayNextEpisode()) tryPlayNextEpisode();
    });
    connect(session_, &PlaybackSession::resumeSaved, this, &MainWindow::scheduleProgressSync);

    // mdsync T2: mark/favourite/playlist mutations arm the SAME debounced progress push as a resume change.
    // The stores stay QtCore-clean (no Qt signals) — each exposes a lightweight std::function change-hook we
    // register once here. The hook marshals back onto this window's thread (a store write may originate off the
    // GUI thread, e.g. an importer) before touching the QTimer, then (re)arms the 15s debounce.
    auto armProgressSync = [this] {
        QMetaObject::invokeMethod(this, [this] { scheduleProgressSync(); }, Qt::QueuedConnection);
    };
    ItemMarks::setChangeHook(armProgressSync);
    FavoritesStore::setChangeHook(armProgressSync);
    PlaylistStore::setChangeHook(armProgressSync);

    // The game-launch pipeline + external-emulator lifecycle: resolves the ROM's system/core, loads it into the
    // shared RetroView or hands it to a standalone emulator (installing/monitoring it), and drives the touchy
    // window/wait-page bits via signals so they stay here where the window lives.
    launcher_ = new GameLauncher(retro_, this);
    connect(launcher_, &GameLauncher::aboutToLaunch, this, [this] {
        player_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
        session_->clearQueue();
    });
    connect(launcher_, &GameLauncher::showRetroRequested, this,
            [this] { stack_->setCurrentWidget(retro_); });
    connect(launcher_, &GameLauncher::waitPage, this, [this](const QString& text, bool stop) {
        ensureEmuPage();
        emuLabel_->setText(text);
        emuStopBtn_->setVisible(stop);
        stack_->setCurrentWidget(emuPage_);
    });
    // Install/launch progress: refresh the wait-page label only when it's already showing — never switch to it.
    // (An install-only flow from Settings ▸ Emulators must leave the user on the settings panel.)
    connect(launcher_, &GameLauncher::waitPageStatus, this, [this](const QString& t) {
        if (emuPage_ && stack_->currentWidget() == emuPage_) { emuLabel_->setText(t); emuStopBtn_->setVisible(false); }
    });
    connect(launcher_, &GameLauncher::waitPageDone, this,
            [this] { if (stack_->currentWidget() == emuPage_) openHome(); });
    connect(launcher_, &GameLauncher::minimizeRequested, this, [this] {
        emuReturnState_ = windowState();
        showMinimized();
    });
    connect(launcher_, &GameLauncher::restoreRequested, this, [this] {
        if (!isMinimized()) return; // come back to where we were before handing off
        if (emuReturnState_ & Qt::WindowFullScreen)    showFullScreen();
        else if (emuReturnState_ & Qt::WindowMaximized) showMaximized();
        else                                            showNormal();
        raise();
        activateWindow();
    });
    connect(launcher_, &GameLauncher::statusMessage, this,
            [this](const QString& m, int ms) { statusBar()->showMessage(m, ms); });
    connect(launcher_, &GameLauncher::notifyUser, this,
            [this](const QString& m, int ms) { notifier_->notify(m, ms); });

    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    // Hide after the inactivity timeout. Every interaction (mouse move or arrow-key navigation) calls
    // revealMediaControls(), which restarts this timer - so the controls stay up while you're actively
    // navigating and fade out a few seconds after you stop. (The hide body is shared with the touch tap-toggle.)
    connect(controlsHideTimer_, &QTimer::timeout, this, [this] { hideMediaControls(); });

    // Reveal the controls on mouse movement over the player / controls.
    player_->setMouseTracking(true);
    player_->installEventFilter(this);
    mediaControls_->installEventFilter(this);

    // Touch (D1 Task 5): a tap on the bare video TOGGLES the transport chrome; a double-tap (<350 ms) on the
    // left/right third seeks ∓10 s. Gated on QTouchEvent only — the mouse path above is untouched, so a physical
    // click behaves exactly as today. The pending-tap timer defers the single-tap toggle so it never fires on the
    // first tap of a double-tap.
    player_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    playerTapTimer_ = new QTimer(this);
    playerTapTimer_->setSingleShot(true);
    connect(playerTapTimer_, &QTimer::timeout, this, [this] { togglePlayerChrome(); });

    // HomeView's progress/error toasts now render as our window-level overlay so they stay visible over any
    // theme (a themed home is a native QQuickView the view's own toast couldn't cover).
    connect(home_, &HomeView::toastRequested, this, &MainWindow::notify);
    connect(home_, &HomeView::toastHideRequested, this, &MainWindow::hideNotice);
    connect(home_, &HomeView::requestOpenFile, this, &MainWindow::onRequestOpenFile);
    connect(home_, &HomeView::openRecent, this, &MainWindow::openRecent);
    connect(home_, &HomeView::startChannelRequested, this, &MainWindow::startChannel);
    connect(home_, &HomeView::channelPickResolved, this, &MainWindow::onChannelPickResolved);
    connect(home_, &HomeView::channelPickDetoured, this, &MainWindow::onChannelPickDetoured);
    connect(home_, &HomeView::switchProfileRequested, this, &MainWindow::onSwitchProfile);
    connect(home_, &HomeView::themeChanged, this, &MainWindow::onThemeChanged);
    connect(home_, &HomeView::settingsRequested, this, &MainWindow::openSettingsHub);
    // At the classic home root, Back has nowhere further to go -> the app pause menu (the one Back rule).
    connect(home_, &HomeView::backRequested, this, &MainWindow::showEscMenu);
    connect(book_, &EbookView::homeRequested, this, &MainWindow::openHome);
    connect(pdf_, &PdfView::homeRequested, this, &MainWindow::openHome);
    connect(comic_, &ComicView::homeRequested, this, &MainWindow::openHome);
    connect(library_, &LibraryView::homeRequested, this, &MainWindow::openHome);
    // Reader "‹ Back": return to the HomeView WITHOUT refreshing it, so the chapter/catalog list you came
    // from is still there (openHome() rebuilds Home from the root, which loses that position).
    auto returnFromReader = [this] {
#ifdef MMV_HAVE_QML
        // Drop any themed reader level + hide chrome on whichever reader host was up (all idempotent).
        if (readerHost_) readerHost_->onLeaving();
        if (pdfHost_)    pdfHost_->onLeaving();
        if (comicHost_)  comicHost_->onLeaving();
#endif
        book_->persist(); pdf_->persist(); comic_->persist();
        // Return to the surface the reader was opened FROM, not a hardcoded home (B2 Task 6, item 1). The reader
        // is a separate stack page, so the themed home/browse it was launched off still holds its exact view
        // (detail if opened from detail, browse otherwise) and cursor — just switch back to it. A classic-mode
        // or unknown origin falls back to the classic HomeView (the original behaviour, which also preserves the
        // list position by NOT rebuilding it).
        QWidget* origin = readerOrigin_;
        readerOrigin_ = nullptr;
#ifdef MMV_HAVE_QML
        if (themedHomeEnabled())
        {
            if (origin && (origin == themedHome_ || origin == themedBrowse_))
            {
                stack_->setCurrentWidget(origin);
                origin->setFocus();
                return;
            }
            // Null/foreign origin in THEMED mode (e.g. a reader opened off the Library page or before any
            // capture): route through showHomeScreen() — the themed home, or its LOGGED classic fallback —
            // instead of silently landing on the raw classic home_ (B2 Task 6 fix round).
            showHomeScreen();
            return;
        }
#endif
        // Classic mode: the original behaviour — the classic HomeView WITHOUT a refresh, so the chapter/catalog
        // list you came from keeps its position.
        stack_->setCurrentWidget(home_);
        home_->focusContent();
    };
    connect(comic_, &ComicView::backRequested, this, returnFromReader);
    connect(book_,  &EbookView::backRequested, this, returnFromReader);
    connect(pdf_,   &PdfView::backRequested,   this, returnFromReader);
#ifdef MMV_HAVE_QML
    // The themed reader hosts' Back router pops the "reader" level with the chrome hidden -> leave the reader.
    if (readerHost_) connect(readerHost_, &ReaderChromeHost::exitRequested, this, returnFromReader);
    if (pdfHost_)    connect(pdfHost_,    &ReaderChromeHost::exitRequested, this, returnFromReader);
    if (comicHost_)  connect(comicHost_,  &ReaderChromeHost::exitRequested, this, returnFromReader);
#endif
    // Reader "Issue with Streaming": ask the file provider for the next source and re-open the new file.
    connect(book_, &EbookView::streamIssueRequested, this, [this] {
        showNextSourceFeedback(tr("Finding another source…")); home_->requestNextSource(); });
    connect(pdf_,  &PdfView::streamIssueRequested,   this, [this] {
        showNextSourceFeedback(tr("Finding another source…")); home_->requestNextSource(); });
    // Result of a next-source request: on success the new file opens itself; on failure show why.
    connect(home_, &HomeView::nextSourceResult, this, [this](bool ok, const QString& msg) {
        if (ok) { notifier_->hidePlayerNotice(); }
        else    { showNextSourceFeedback(msg); }
    });
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
    connect(speedBtn_, &QPushButton::clicked, this, [this] { cyclePlaybackSpeed(+1); revealMediaControls(); });
    connect(subsBtn, &QPushButton::clicked, this, [this] { showSubtitleMenu(); });
    connect(shotBtn, &QPushButton::clicked, this, [this] { captureVideoScreenshot(); revealMediaControls(); });
    connect(castBtn, &QPushButton::clicked, this, [this, castBtn] { showCastMenu(castBtn); });
    connect(player_, &MpvWidget::endReached, this, &MainWindow::onTrackEnded);
    connect(retro_, &RetroView::statusMessage, this, [this](const QString& t) { statusBar()->showMessage(t, 3000); });
    // J10: a core crash is an error the user must notice, not a 3 s ambient save/load blip — route it through the
    // window-level notify() overlay (over ANY view, incl. the full-screen emulator) at kFeedbackLong.
    connect(retro_, &RetroView::coreError, this, [this](const QString& t) { notify(t, kFeedbackLong); });
    connect(retro_, &RetroView::exitRequested, this, [this] { retro_->stop(); openHome(); });
    // (Play-time banking on RetroView::gameStopped now lives in GameLauncher, which owns the session state.)
    connect(playPause, &QPushButton::clicked, player_, &MpvWidget::togglePause);
    connect(stop, &QPushButton::clicked, this, [this] {
        player_->stop(); mediaControls_->hide(); session_->clearQueue(); openHome(); });
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
    auto* splitShortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
    connect(splitShortcut, &QShortcut::activated, this, [this] { if (splitMode_) exitSplitScreen(); else enterSplitScreen(); });

    // Form-factor adaptivity (D1 Task 3): re-derive every widget-side size whenever the mode changes, and once
    // now so the initial build carries the current tokens (desktop identity: a pixel-for-pixel no-op).
    connect(&FormFactor::instance(), &FormFactor::changed, this, &MainWindow::applyFormFactorWidgets);
    applyFormFactorWidgets();

    // Quit provenance: EVERY clean shutdown routes through aboutToQuit, and each explicit QApplication::quit()
    // site also logs its trigger (grep "quit:" in this file). So a harness "clean exit" always names itself in
    // stream_debug.log instead of recurring as an unexplained footnote. Harmless in production (append-only log).
    connect(qApp, &QCoreApplication::aboutToQuit, this, [] { mwLog(QStringLiteral("aboutToQuit (clean exit path)")); });

    // Dominant home-build cost (classic HomeView, or the themed QML home if enabled). The HomeView ctor
    // sits in the unspanned gap just after the startup.addons region (cheap today), so it's excluded here.
    PerfTrace::begin(QStringLiteral("startup.home"));
    showHomeScreen(); // the catalog landing screen (classic, or the themed home if enabled) is shown first
    PerfTrace::end(QStringLiteral("startup.home"));

    // Pull another device's "continue watching" progress and merge it in, shortly after startup so it doesn't
    // block launch or hit the network before the UI is up. No-op if not signed into cloud sync.
    QTimer::singleShot(1500, this, [this] { pullAndMergeProgress(); });

    // Local video library: scan off-thread at startup, install the index + refresh the home on the main
    // thread. Dormant (instant, empty) when no library/folder is configured. Shares the single async-scan
    // site with the Settings folder-picker / Rescan action.
    rescanLocalLibrary();
}

// Read the configured library root on the MAIN thread (QSettings is not thread-safe — a prior review caught
// a race from reading it inside the worker), capture it by value, then scan off-thread and install the
// rebuilt index + refresh the home on completion. Called from startup and the Settings picker / Rescan.
void MainWindow::rescanLocalLibrary()
{
    const QString libRoot = LocalLibrary::root();               // read Settings on the MAIN thread
    const quint64 gen = ++libScanGen_;                          // main thread only (rescan is main-thread)
    // Snapshot the resolver's cached matches on the MAIN thread (QHash by value → thread-safe in the worker) so
    // the freshly rebuilt index already carries any previously-resolved online IDs; new movies resolve below.
    const QHash<QString, QStringList> extra = resolveCache_ ? resolveCache_->matchedIdsByPath()
                                                            : QHash<QString, QStringList>{};
    auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
    connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w, gen] {
        if (gen == libScanGen_) {                               // ignore a scan superseded by a newer rescan
            LocalLibrary::installIndex(w->result());
            if (home_) home_->onLocalLibraryChanged();
            if (resolver_) resolver_->enqueue(LocalLibrary::index().all());   // resolve uncached movies in the background
        }
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([libRoot, extra] {
        return LocalLibrary::buildIndex(LocalLibrary::scanFolder(libRoot), extra);
    }));
}

MainWindow::~MainWindow() = default; // AddonManager is complete in this translation unit

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
#ifdef MMV_HAVE_QML
    // Themed input-mapping CAPTURE: while active this is installed on qApp, so it sees keys before the themed
    // QQuickWidget's QML nav. Swallow all key traffic (modal capture): Esc cancels, a keyboard key binds, the
    // pad button arrives via the poll timer. (ShortcutOverride too, so a key can't trigger a shortcut instead.)
    // MOUSE is modal too: SettingsPanel.qml's MouseAreas drive the graph directly (row clicks + the header
    // "‹ Back"), so an unswallowed click would navigate/activate WITHOUT ending capture — leaking this filter
    // (every keystroke app-wide swallowed), leaving the pad timer polling, and arming the NEXT key as a silent
    // binding write for a row that may no longer be shown. A press CANCELS the capture and is consumed (the
    // click does not also activate — the first click just cancels, like Esc); release/dblclick/wheel are
    // consumed so no orphan half-click reaches the page behind.
    if (remap_.active)
    {
        const QEvent::Type t = event->type();
        if (t == QEvent::KeyPress)   return inputCaptureKeyFilter(static_cast<QKeyEvent*>(event));
        if (t == QEvent::KeyRelease || t == QEvent::ShortcutOverride) return true;
        if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick)
        { endInputCapture(/*cancelled*/ true); return true; }
        if (t == QEvent::MouseButtonRelease || t == QEvent::Wheel) return true;
    }
#endif

    // Touch on the bare video surface: tap toggles chrome, double-tap seeks (touch-only; mouse path unchanged).
    if (obj == player_ && (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
                           || event->type() == QEvent::TouchEnd))
        return handlePlayerTouch(static_cast<QTouchEvent*>(event));

    if (event->type() == QEvent::MouseMove && (obj == player_ || obj == mediaControls_ || obj == videoBack_
                                               || obj == streamIssueBtn_))
        revealMediaControls();

    // A click on the subtitle overlay's scrim (i.e. outside the card, whose child widgets consume their own
    // clicks) dismisses the panel.
    if (obj == subOverlay_ && event->type() == QEvent::MouseButtonPress) { hideSubtitleMenu(); return true; }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if ((mediaControls_ && mediaControls_->isVisible()) || subOverlay_)
        positionMediaControls();
    if (notifier_) notifier_->reposition();
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    QMainWindow::moveEvent(event);
    // The notice is now a child overlay of the central area, so it tracks a resize on its own; this
    // reposition only matters for DPI-change edge cases (a screen move can shift device-pixel metrics).
    if (notifier_) notifier_->reposition();
}

void MainWindow::leaveFullScreen()
{
    showNormal();
    player_->unsetCursor();    // restore the cursor
}

// ---- App pause menu (Esc) -------------------------------------------------------------------------------

bool MainWindow::escMenuVisible() const { return escMenuOverlay_ != nullptr; }

void MainWindow::showEscMenu()
{
    if (escMenuVisible()) return;
    // A NavMenu overlay: an in-window child, so it renders over the themed QML surface with no separate OS
    // window (no focus tug-of-war, no black flash), and restores the previous selection when it closes.
    auto* menu = new NavMenu(tr("My Media Vault"), { tr("Resume"), tr("Exit My Media Vault") },
                             [this](int row) { if (row == 1) close(); }, // close() runs the Drive-push exit
                             this);
    escMenuOverlay_ = menu;
    // Over a themed screen the menu mirrors itself as a level on that screen's NavGraph so the graph depth
    // tracks reality. Closing it revives the QML scene's focus through NavOverlay::dismiss's single kick — the
    // old duplicate closed-handler focus kick here is gone.
    menu->setNavGraph(currentThemedGraph());
}

void MainWindow::hideEscMenu()
{
    if (auto* overlay = qobject_cast<NavOverlay*>(escMenuOverlay_.data())) overlay->dismiss(-1);
}

NavGraph* MainWindow::currentThemedGraph() const
{
#ifdef MMV_HAVE_QML
    QWidget* cur = stack_->currentWidget();
    if (cur == themedHome_ || cur == themedBrowse_) return ThemeEngine::navGraph(cur);
#endif
    return nullptr;
}

// ---- Controller navigation of the menus (EmulationStation-style) ---------------------------------------

// libretro RETRO_DEVICE_ID_JOYPAD_* ids used for menu navigation (B/south = confirm, A/east = back — the
// Gamepad's default mapping; the left stick also drives the d-pad ids past a deadzone).
namespace { constexpr int PAD_B = 0, PAD_START = 3, PAD_UP = 4, PAD_DOWN = 5, PAD_LEFT = 6, PAD_RIGHT = 7, PAD_A = 8; }

void MainWindow::sendNavKey(int key)
{
    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right)
        PerfTrace::begin(QStringLiteral("nav.select")); // ended in HomeView::requestThemedMeta; overwritten if no selection change
    // Mark everything below as controller-origin, so widgets can tell a pad "Back" from a typed Backspace.
    NavContext::SyntheticScope synth;
    auto deliver = [](QObject* target, int k) {
        if (!target) return;
        // The press can delete the target: a confirm key on a menu row runs its handler synchronously, and
        // opening a new panel (showPanel) deletes the focused row. Guard with a QPointer so we don't then send
        // the release to a freed object — that dereferenced a dangling receiver in QCoreApplication and crashed.
        QPointer<QObject> guard(target);
        QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &press);
        if (!guard) return;
        QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &release);
    };
    // 1. An open in-window overlay (menu / confirm / on-screen keyboard) owns every key.
    if (NavOverlay::routeTopmost(key)) return;
    // 2. A popup (a QMenu / an open combo dropdown) grabs input while open — Qt routes real key events to
    //    it, but our synthetic ones must be aimed at it explicitly. This must beat the screen's ring, or
    //    arrows would move the selection BEHIND an open dropdown. Backspace becomes Escape so B closes it.
    if (QWidget* popup = QApplication::activePopupWidget())
    { deliver(popup, key == Qt::Key_Backspace ? Qt::Key_Escape : key); return; }
    // 3. A modal dialog (a stray OS dialog not yet on the kit) must receive nav keys, not the view behind it.
    if (QWidget* modal = QApplication::activeModalWidget())
    {
        QWidget* f = QApplication::focusWidget();
        QWidget* tgt = (f && modal->isAncestorOf(f)) ? f : modal;
        deliver(tgt, key == Qt::Key_Backspace ? Qt::Key_Escape : key);
        return;
    }
    // 3.5. A text widget that's being INTERACTED with (a line edit being edited, or the Debug log in scroll
    //      mode) owns its keys: deliver straight to it so its two-state filter handles Escape (leave to the
    //      selection, NOT the screen's Back), arrows (scroll / move the cursor), and Enter (commit). Physical
    //      keys reach the filter directly; the controller's synthetic ones have to be aimed here.
    {
        QWidget* fwi = QApplication::focusWidget();
        if (!fwi) fwi = focusWidget();
        if ((qobject_cast<QLineEdit*>(fwi) || qobject_cast<QTextEdit*>(fwi) || qobject_cast<QPlainTextEdit*>(fwi))
            && NavTextField::isInteracting(fwi))
        { deliver(fwi, key); return; }
    }
    QWidget* cur = stack_->currentWidget();
    // 4. The themed home/browse is a QQuickWidget — hand it the key directly; its QML Keys handler does the
    //    arrow nav AND its own multi-level Back (drill up, then the pause menu), matching goBack's rule.
    if (cur && (cur == themedHome_ || cur == themedBrowse_)) { deliver(cur, key); return; }
#ifdef MMV_HAVE_QML
    // 4.2. The themed settings-panel host is a QQuickWidget too — hand it the key; SettingsPanel.qml's Keys
    //      handler drives its NavGraph (arrows / Enter) AND its own Back (nav.back() pops one panel level).
    if (themedPanelHost_ && cur == themedPanelHost_) { deliver(themedPanelHost_->quickWidget(), key); return; }
    // 4.5. The themed reader hosts: hand the key to the wrapped reader widget — its installed chrome event
    //      filter arbitrates (drive the graph while the chrome is visible, reveal on Up, else let the reader
    //      page/zoom). Back is caught by that same filter, so this covers the reader's Back rule too.
    if (readerHost_ && cur == readerHost_) { deliver(book_,  key); return; }
    if (pdfHost_    && cur == pdfHost_)    { deliver(pdf_,   key); return; }
    if (comicHost_  && cur == comicHost_)  { deliver(comic_, key); return; }
#endif
    // 5. The one Back rule: the controller's Back (B) / Start map to Backspace / Escape, and both "go back"
    //    on every widget screen exactly like the keyboard does — previous screen, or the pause menu at the
    //    home root. (Overlays/popups/modals above already consumed their own Back.)
    if (key == Qt::Key_Backspace || key == Qt::Key_Escape) { goBack(); return; }
    // 6. The active screen's ring (arrow nav + Enter). Screens on the kit need no other wiring.
    if (navCtx_ && navCtx_->routeKey(key)) return;
    QWidget* w = QApplication::focusWidget();
    if (!w) w = focusWidget(); // window inactive (UI-test injection): it still remembers its focus child
    if (!w || !isAncestorOf(w)) w = cur; // keep injection within our own window
    // 7. Enter on a focused text box (a search field / settings input) opens the on-screen keyboard.
    if (auto* edit = qobject_cast<QLineEdit*>(w))
    {
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { NavOverlay::editLineEdit(edit); return; }
    }
    // 7. Arrows aimed at a closed dropdown / spinner on a screen without a ring would CYCLE ITS VALUE just
    //    by walking over it. Hover-over instead: hop the focus geometrically; Enter opens the dropdown (or
    //    the OSK on a spinner) — the same behaviour ring screens get from NavRing.
    if (qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w))
    {
        if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right)
        {
            NavRing pageRing(cur ? cur : static_cast<QWidget*>(this));
            if (QWidget* next = NavRing::pickNext(qobject_cast<QWidget*>(w), pageRing.widgets(), key))
                next->setFocus(Qt::OtherFocusReason);
            return; // nowhere to hop: swallow rather than let the value change
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
        {
            if (auto* combo = qobject_cast<QComboBox*>(w)) combo->showPopup();
            else NavOverlay::editSpinBox(qobject_cast<QAbstractSpinBox*>(w));
            return;
        }
    }
    deliver(w, key);
}

// Android OS lifecycle handler (connected only under Q_OS_ANDROID; see the ctor). Backgrounding the app must
// freeze a running emulator core and any playing video so they don't burn battery / hold audio focus while
// off-screen; returning to the foreground resumes ONLY what we froze. The remember-and-restore decision is
// the pure LifecyclePolicy, so "a user-paused video stays paused on return" is enforced there (and pinned by
// probe_formfactor). Unguarded so a probe/test can invoke it directly with any state.
void MainWindow::onApplicationStateChanged(Qt::ApplicationState state)
{
    if (state == Qt::ApplicationActive)
    {
        const auto act = lifecycle_.onResume();
        if (act.core  && retro_)  retro_->setPaused(false);
        if (act.video && player_) player_->setPaused(false);
        return;
    }
    // Suspended / Inactive / Hidden are all "leaving the foreground" on Android.
    const bool coreRunning = retro_ && retro_->running();
    const bool corePaused  = retro_ && retro_->paused();
    const bool videoActive = stack_ && stack_->currentWidget() == playerPage_;
    const bool videoPaused = player_ && player_->isPaused();
    const auto act = lifecycle_.onSuspend(coreRunning, corePaused, videoActive, videoPaused);
    if (act.core  && retro_)  retro_->setPaused(true);
    if (act.video && player_) player_->setPaused(true);
}

// The one Back rule for the whole app: Escape, Backspace and the controller's Back all call this, so a
// "go back" gesture behaves identically on every screen — it always takes you to the previous screen, and
// only at the home root does it open the app pause menu (Resume / Exit). Anything layered on top (an
// overlay/menu/keyboard, or the pause menu itself) is dismissed first.
void MainWindow::goBack()
{
    playRouteOverride_ = PlayRoute::Default; // defensive: a one-off armed but backed out of can't leak
    if (NavOverlay* top = NavOverlay::topmost()) { top->dismiss(-1); return; } // close the thing on top
    if (subOverlay_ && subOverlay_->isVisible()) { hideSubtitleMenu(); return; }
    if (escMenuVisible()) { hideEscMenu(); return; }                            // pause menu -> resume

    QWidget* cur = stack_->currentWidget();

    // Themed (QML) home/browse: drive its NavGraph back stack, exactly as the QML's own nav.back() does. The
    // graph's levels (catalog + browse drills) unwind one at a time; its rootBack (empty stack) fires the
    // screen's root action (the pause menu on the home, the themed home from a browse view).
#ifdef MMV_HAVE_QML
    if (cur == themedHome_ || cur == themedBrowse_)
        if (NavGraph* g = ThemeEngine::navGraph(cur)) { g->back(); return; }
#endif
    // Classic home: pop a drill level, or (at the root) emit backRequested -> the pause menu.
    if (cur == home_) { home_->goBack(); return; }
    // Settings / dialog panels: their header Back = the previous panel or the screen we came from.
    if (cur == panelPage_) { if (panelBack_) panelBack_->click(); return; }
    // Library: unwind a pushed sub-page (registry/addon settings), else back to the Settings hub.
    if (cur == library_) { if (!library_->navBack()) openSettingsHub(); return; }
    // The themed reader host owns its own Back rule (chrome visible -> hide; hidden -> pop the reader level,
    // which returns us home). Route to it before the plain reader case below.
#ifdef MMV_HAVE_QML
    // The themed settings-panel host owns its own Back (pop one panel level; at the root, its onBack leaves to
    // the home screen). Route to it before the plain-reader/other cases below.
    if (themedPanelHost_ && cur == themedPanelHost_) { themedPanelHost_->handleBack(); return; }
    if (readerHost_ && cur == readerHost_) { readerHost_->handleBack(); return; }
    if (pdfHost_    && cur == pdfHost_)    { pdfHost_->handleBack();    return; }
    if (comicHost_  && cur == comicHost_)  { comicHost_->handleBack();  return; }
#endif
    // CLASSIC-mode readers only: in themed mode the reader is a ReaderChromeHost page (handled above), whose
    // exit routes through returnFromReader with origin-restore. A raw book_/pdf_/comic_ page is reachable only
    // in a non-QML build (or classic mode), where the classic HomeView is always the right destination.
    if (cur == book_ || cur == pdf_ || cur == comic_)
    { book_->persist(); pdf_->persist(); comic_->persist(); stack_->setCurrentWidget(home_); home_->focusContent(); return; }
    // Standalone-emulator wait page: close the emulator.
    if (cur == emuPage_) { if (emuStopBtn_) emuStopBtn_->click(); return; }
    // Split screen: leave it.
    if (splitMode_) { exitSplitScreen(); return; }
    // Player (and any other content page): stop playback and return home. A Back from the player is a user stop,
    // so the channel ends here (also covered by openHome below — explicit for the trace, idempotent).
    exitChannel();
    player_->stop();
    if (mediaControls_) mediaControls_->hide();
    if (videoBack_) videoBack_->hide();
    session_->clearQueue();
    openHome();
}

// Re-register the nav kit for the page the stack just switched to: which ring drives the arrow keys and
// what the pad's Back does there. Screens with their own complete internal navigation (home, themed QML,
// readers — which already map Backspace to back — and the emulator) register neither, so their key
// handling is untouched; overlays still route above them.
// Show the just-opened book: themed mode wraps it in the chrome host (themed strips on the Nav Contract),
// classic mode shows it with its own widget chrome. The host is a persistent stack page that already holds
// book_; present() only toggles which chrome is live, so there's no reparenting churn per open.
// Remember which surface a reader is being opened from, so its exit can return there (B2 Task 6, item 1). Only
// captured when we're NOT already inside a reader: a stream-issue re-open (next source) re-presents while a
// reader host is current, and must keep the ORIGINAL origin, not overwrite it with the reader page itself.
void MainWindow::captureReaderOrigin()
{
    QWidget* cur = stack_->currentWidget();
    bool inReader = (cur == book_ || cur == pdf_ || cur == comic_);
#ifdef MMV_HAVE_QML
    inReader = inReader || (readerHost_ && cur == readerHost_) || (pdfHost_ && cur == pdfHost_)
                        || (comicHost_ && cur == comicHost_);
#endif
    if (!inReader) readerOrigin_ = cur;
}

void MainWindow::presentBook()
{
    captureReaderOrigin();
#ifdef MMV_HAVE_QML
    if (readerHost_)
    {
        readerHost_->present(themedHomeEnabled());
        stack_->setCurrentWidget(readerHost_);
        return;
    }
#endif
    stack_->setCurrentWidget(book_);
}

void MainWindow::presentPdf()
{
    captureReaderOrigin();
#ifdef MMV_HAVE_QML
    if (pdfHost_)
    {
        pdfHost_->present(themedHomeEnabled());
        stack_->setCurrentWidget(pdfHost_);
        return;
    }
#endif
    stack_->setCurrentWidget(pdf_);
}

void MainWindow::presentComic()
{
    captureReaderOrigin();
#ifdef MMV_HAVE_QML
    if (comicHost_)
    {
        comicHost_->present(themedHomeEnabled());
        stack_->setCurrentWidget(comicHost_);
        return;
    }
#endif
    stack_->setCurrentWidget(comic_);
}

void MainWindow::updateNavForPage()
{
    if (!navCtx_) return;
    QWidget* cur = stack_->currentWidget();
    if (cur == panelPage_)
    {
        // Panels: the ring covers the header Back button + whatever rows showPanel built (including an
        // embedded dialog's fields). The input-mapping dialog runs its own keyboard nav and raw-input
        // capture, so the ring stays off there — pad Back still works via the panel header below.
        const bool remapOwnsKeys = panelDialog_ && panelDialog_->inherits("ControllerRemapDialog");
        navCtx_->setActiveRing(remapOwnsKeys ? nullptr : panelRing_);
        navCtx_->setBackAction([this] { if (panelBack_) panelBack_->click(); });
    }
    else if (cur == playerPage_)
    {
        // The player keeps its transport-row Left/Right stepping; pad Back exits the way the on-screen
        // "‹ Back" overlay does (stop + clear the queue + Home).
        navCtx_->setActiveRing(nullptr);
        navCtx_->setBackAction([this] { if (videoBack_) videoBack_->click(); });
    }
    else if (cur == library_)
    {
        // The Library: ring nav over its lists/buttons/search (lists keep their own Up/Down inside; the
        // ring steps out at the ends). Back unwinds a pushed sub-page, then leaves to the Settings hub.
        if (!libraryRing_) libraryRing_ = new NavRing(library_, this);
        navCtx_->setActiveRing(libraryRing_);
        navCtx_->setBackAction([this] { if (!library_->navBack()) openSettingsHub(); });
    }
    else
    {
        // The themed reader host (book / pdf / comic) has no ring, so routeKey never consults a back action for
        // it (Nav.cpp checks backAction_ only inside `if (activeRing_)`); its Back arrives through the host's own
        // event filter (ReaderChromeHost::arbitrateKey -> handleBack). Nothing to register here.
        navCtx_->setActiveRing(nullptr);
        navCtx_->setBackAction(nullptr);
    }

#ifdef MMV_HAVE_QML
    // A themed (QML) page owns its own focus (no ring), but it DOES have a real selection surface — register
    // its NavGraph so the kit knows the page is navigable (presence), instead of the null-ring "nothing here".
    // Any other page clears it. (Set after the ring choice above so it applies on every page.)
    NavGraph* pageGraph = (cur == themedHome_ || cur == themedBrowse_) ? ThemeEngine::navGraph(cur) : nullptr;
    ReaderChromeHost* curHost = (readerHost_ && cur == readerHost_) ? readerHost_
                              : (pdfHost_    && cur == pdfHost_)    ? pdfHost_
                              : (comicHost_  && cur == comicHost_)  ? comicHost_ : nullptr;
    if (!pageGraph && curHost && curHost->themed())
        pageGraph = curHost->navGraph();
    // The themed settings-panel host owns its own selection surface too — register its graph so the kit marks
    // the page navigable (its Back arrives via sendNavKey delivering to the QQuickWidget, so no back action here).
    if (!pageGraph && themedPanelHost_ && cur == themedPanelHost_)
        pageGraph = themedPanelHost_->navGraph();
    navCtx_->setActiveGraph(pageGraph);
#endif
}

// Create or tear down the UI-test channel (core/UiTestServer) to match its enablement: MMV_UITEST=1,
// --uitest, or the Settings ▸ Debug toggle. Lets a test agent drive navigation and capture the window
// WITHOUT bringing it to the front or giving it OS focus — injected keys ride the app's own sendNavKey
// routing, and grab() renders the widget tree even while occluded/backgrounded.
// Report the themed (QML) home/browse selection into the UI-test `state` snapshot: the QQuickWidget
// hides its internal selection behind an opaque focus, so read the scene root's live properties instead.
// No-op unless `page` is the themed home/browse (and only compiled with the QML engine).
void MainWindow::addThemedSelection(QJsonObject& o, QWidget* page)
{
#ifdef MMV_HAVE_QML
    if (page != themedHome_ && page != themedBrowse_) return;
    QQuickItem* r = ThemeEngine::rootItem(page);
    if (!r) return;
    // Read plain-string/int properties only (the arrays are reduced to strings QML-side, see ThemeView.qml's
    // uitestSelection/uitestCategory) — marshaling the `var` item arrays across the boundary is unsafe.
    const QString sel = r->property("uitestSelection").toString();
    o.insert(QStringLiteral("themedView"), r->property("currentView").toString());
    o.insert(QStringLiteral("themedIndex"), r->property("currentIndex").toInt());
    o.insert(QStringLiteral("themedSelection"), sel);
    const QString cat = r->property("uitestCategory").toString();
    if (!cat.isEmpty()) o.insert(QStringLiteral("themedCategory"), cat);
    // The audio now-playing page: surface its zone/cursor + live playback so audio automation is as precise as
    // the Qt panels (the QQuickWidget focus is opaque). themedFocus names what Enter would fire right now.
    if (r->property("currentView").toString() == QStringLiteral("nowplayingAudio"))
    {
        const QString zone = r->property("audioZone").toString();
        const int tIdx = r->property("audioTransportIndex").toInt();
        const int qIdx = r->property("audioQueueIndex").toInt();
        o.insert(QStringLiteral("audioZone"), zone);
        o.insert(QStringLiteral("audioTransportIndex"), tIdx);
        o.insert(QStringLiteral("audioQueueIndex"), qIdx);
        o.insert(QStringLiteral("audioQueueCurrent"), r->property("audioQueueCurrent").toInt());
        o.insert(QStringLiteral("audioQueueCount"), r->property("audioQueueCount").toInt());
        o.insert(QStringLiteral("audioPosition"), r->property("audioPosition").toDouble());
        o.insert(QStringLiteral("audioDuration"), r->property("audioDuration").toDouble());
        o.insert(QStringLiteral("audioPaused"), r->property("audioPaused").toBool());
        o.insert(QStringLiteral("audioSpeed"), r->property("audioSpeed").toDouble());
        const QVariantList verbs = r->property("audioTransportList").toList();
        QString focus;
        if (zone == QStringLiteral("transport") && tIdx >= 0 && tIdx < verbs.size())
            focus = QStringLiteral("transport:") + verbs[tIdx].toString();
        else if (zone == QStringLiteral("queue"))
            focus = QStringLiteral("queue:") + QString::number(qIdx);
        o.insert(QStringLiteral("themedFocus"), focus);
    }
    // What Enter would act on right now: a corner button, the inline action chooser, or the tile above.
    else if (r->property("focusZone").toInt() == 1)
        o.insert(QStringLiteral("themedFocus"), r->property("focusedButtonAction").toString());
    else if (r->property("actionsOpen").toBool())
        o.insert(QStringLiteral("themedFocus"),
                 QStringLiteral("action:") + QString::number(r->property("actionIndex").toInt()));
    else
        o.insert(QStringLiteral("themedFocus"), sel);
#else
    Q_UNUSED(o); Q_UNUSED(page);
#endif
}

// ---- uitest `touch` verb: synthesize REAL touch sequences on the top-level window --------------------------
// The whole point is to exercise the QML touch handlers (MouseAreas, Flickables, the edge-back MouseArea)
// through real hit-testing — NOT to shortcut into the NavGraph. So we post QWindowSystemInterface touch events
// against the window handle exactly as a physical touchscreen would, and let Qt route + hit-test them. Multi-
// frame gestures run on a QTimer state machine so the pipe handler is never blocked mid-sequence.
namespace {

// The touchscreen device Qt attributes the synthetic points to. Registered ONCE (Qt owns it thereafter).
QPointingDevice* uitestTouchDevice()
{
    static QPointingDevice* dev = [] {
        auto* d = new QPointingDevice(
            QStringLiteral("uitest-touchscreen"), 0x0107E57,
            QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::Area
                | QInputDevice::Capability::Pressure,
            10 /*maxTouchPoints*/, 0 /*buttonCount*/);
        QWindowSystemInterface::registerInputDevice(d);
        return d;
    }();
    return dev;
}

// One synthetic touch point. Callers pass a window-local point in LOGICAL (device-independent) coordinates —
// the same space `state` reports the window `size` in, so `touch tap X Y` uses the coordinates an agent already
// sees. QWindowSystemInterface::TouchPoint.area is in NATIVE (physical-pixel) global screen coordinates, so we
// map local -> global (logical) and scale up by the window's device-pixel ratio (single-origin screen). Qt maps
// it back to window-local logical during delivery, so the point lands exactly where the caller intended.
QWindowSystemInterface::TouchPoint uitestTP(QWindow* win, int id, QEventPoint::State st, QPointF local)
{
    QWindowSystemInterface::TouchPoint tp;
    tp.id = id;
    tp.state = st;
    const QPointF gLogical = win->mapToGlobal(local);
    const qreal dpr = win->devicePixelRatio();
    const QPointF gNative = gLogical * dpr;                 // -> native pixels for the QPA layer
    tp.area = QRectF(gNative.x() - 1, gNative.y() - 1, 2, 2);
    if (QScreen* sc = win->screen())
    {
        const QRect sg = sc->geometry();                   // logical; the ratio is DPR-invariant
        if (sg.width() > 0 && sg.height() > 0)
            tp.normalPosition = QPointF(qreal(gLogical.x() - sg.x()) / sg.width(),
                                        qreal(gLogical.y() - sg.y()) / sg.height());
    }
    tp.pressure = (st == QEventPoint::State::Released) ? 0.0 : 1.0;
    return tp;
}

// Build the gesture's frames, then drive them one per timer tick (first frame delivered immediately). Parses
// "tap X Y", "flick X1 Y1 X2 Y2 [MS]" (default 150ms, >=6 interpolated moves), "pinch CX CY SCALE [MS]".
// Returns false if a sequence is already in flight — overlapping gestures share touch id 0 and would interleave
// press/update/release into corrupt Qt touch state (a stuck point), so a second command is rejected (`err busy`)
// rather than queued. The busy latch is a GUI-thread-only static (the pipe + the QTimer both run there).
bool uitestRunTouch(QWindow* win, const QString& arg, QObject* parent)
{
    static bool inFlight = false;
    if (inFlight) return false;                            // a gesture is mid-flight: reject (caller retries)
    if (!win) return true;                                 // no window to touch: accepted, nothing to do
    using S = QEventPoint::State;
    const QStringList t = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString sub = t.value(0).toLower();

    auto* frames = new QVector<QList<QWindowSystemInterface::TouchPoint>>();
    int intervalMs = 30;
    auto tp = [win](int id, S st, qreal x, qreal y) { return uitestTP(win, id, st, QPointF(x, y)); };

    if (sub == QStringLiteral("tap") && t.size() >= 3)
    {
        const qreal x = t[1].toDouble(), y = t[2].toDouble();
        frames->append({ tp(0, S::Pressed, x, y) });
        frames->append({ tp(0, S::Released, x, y) });
        intervalMs = 30;                                   // 2 frames, ~30ms apart
    }
    else if (sub == QStringLiteral("flick") && t.size() >= 5)
    {
        const qreal x1 = t[1].toDouble(), y1 = t[2].toDouble(), x2 = t[3].toDouble(), y2 = t[4].toDouble();
        const int ms = t.size() >= 6 ? t[5].toInt() : 150;
        const int steps = 8;                               // 8 interpolated moves (>= the required 6)
        frames->append({ tp(0, S::Pressed, x1, y1) });
        for (int i = 1; i <= steps; ++i)
        {
            const qreal f = qreal(i) / steps;
            frames->append({ tp(0, S::Updated, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f) });
        }
        frames->append({ tp(0, S::Released, x2, y2) });
        intervalMs = qMax(1, ms / (steps + 1));
    }
    else if (sub == QStringLiteral("pinch") && t.size() >= 4)
    {
        const qreal cx = t[1].toDouble(), cy = t[2].toDouble(), scale = t[3].toDouble();
        const int ms = t.size() >= 5 ? t[4].toInt() : 150;
        const int steps = 8;
        const qreal half0 = 60.0;                          // initial half-separation of the two fingers (px)
        auto frame = [&](qreal half, S s0, S s1) {
            return QList<QWindowSystemInterface::TouchPoint>{ tp(0, s0, cx - half, cy), tp(1, s1, cx + half, cy) };
        };
        frames->append(frame(half0, S::Pressed, S::Pressed));
        for (int i = 1; i <= steps; ++i)
        {
            const qreal f = qreal(i) / steps;
            frames->append(frame(half0 * (1.0 + (scale - 1.0) * f), S::Updated, S::Updated));
        }
        frames->append(frame(half0 * scale, S::Released, S::Released));
        intervalMs = qMax(1, ms / (steps + 1));
    }
    else { delete frames; return true; }                   // malformed: accepted, nothing to deliver

    // A real sequence is starting: latch busy until its final frame is delivered (cleared in the terminal tick).
    inFlight = true;
    // Deliver frame 0 now; a repeating single-shot-style QTimer walks the rest, then self-destructs.
    QWindowSystemInterface::handleTouchEvent(win, uitestTouchDevice(), frames->at(0));
    auto* idx = new int(1);
    auto* timer = new QTimer(parent);
    timer->setInterval(intervalMs);
    QPointer<QWindow> alive(win);
    QObject::connect(timer, &QTimer::timeout, timer, [alive, frames, idx, timer] {
        if (!alive || *idx >= frames->size())
        {
            inFlight = false;                              // sequence done (or window gone): the channel re-opens
            timer->stop(); timer->deleteLater(); delete frames; delete idx; return;
        }
        QWindowSystemInterface::handleTouchEvent(alive, uitestTouchDevice(), frames->at(*idx));
        ++(*idx);
    });
    timer->start();
    return true;
}

} // namespace

void MainWindow::updateUiTestServer()
{
    if (!UiTestServer::wanted())
    {
        if (uiTest_) { delete uiTest_; uiTest_ = nullptr; mwLog(QStringLiteral("uitest: control channel stopped")); }
        if (blackWatchdog_) { delete blackWatchdog_; blackWatchdog_ = nullptr; }
        return;
    }
    if (uiTest_) return; // already listening (the black-frame watchdog is created alongside it, below)
    UiTestServer::Hooks h;
    h.sendKey = [this](int k) {
        // Qt-INTERNAL activation only (no OS foreground change): focus events + :focus styling then
        // behave exactly as they would live, while another app keeps the real foreground.
        if (!isActiveWindow()) QApplication::setActiveWindow(this);
        sendNavKey(k);
    };
    h.state = [this]() -> QString {
        QJsonObject o;
        QWidget* cur = stack_->currentWidget();
        o.insert(QStringLiteral("page"), cur ? QString::fromLatin1(cur->metaObject()->className()) : QString());
        o.insert(QStringLiteral("pageName"), cur ? cur->objectName() : QString());
        if (cur == panelPage_ && panelTitle_) o.insert(QStringLiteral("panelTitle"), panelTitle_->text());
        QWidget* fw = QApplication::focusWidget();
        if (!fw) fw = focusWidget();
        o.insert(QStringLiteral("focus"), fw ? QString::fromLatin1(fw->metaObject()->className()) : QString());
        o.insert(QStringLiteral("focusName"), fw ? fw->objectName() : QString());
        QString ft;
        if (auto* b = qobject_cast<QAbstractButton*>(fw)) ft = b->text();
        else if (auto* e = qobject_cast<QLineEdit*>(fw)) ft = e->text();
        else if (auto* l = qobject_cast<QListWidget*>(fw)) ft = l->currentItem() ? l->currentItem()->text() : QString();
        o.insert(QStringLiteral("focusText"), ft);
        if (NavOverlay* top = NavOverlay::topmost())
        {
            o.insert(QStringLiteral("overlay"), QString::fromLatin1(top->metaObject()->className()));
            o.insert(QStringLiteral("overlaySelection"), top->describe());
        }
        // Themed (QML) home/browse: focus is opaque (just "QQuickWidget"), so read the scene's own
        // selection state — the highlighted tile's title, the view, and (XMB) the category / (button
        // zone) the focused corner button — so QML-side automation is as precise as the Qt panels.
        addThemedSelection(o, cur);
#ifdef MMV_HAVE_QML
        // Themed reader host (book / pdf / comic): the chrome strips are opaque QQuickWidgets, so surface the
        // graph selection + chrome visibility + page info for reader-chrome automation.
        ReaderChromeHost* rh = (readerHost_ && cur == readerHost_) ? readerHost_
                             : (pdfHost_    && cur == pdfHost_)    ? pdfHost_
                             : (comicHost_  && cur == comicHost_)  ? comicHost_ : nullptr;
        if (rh)
        {
            static const char* kKindName[] = { "book", "pdf", "comic" };
            o.insert(QStringLiteral("readerKind"), QString::fromLatin1(kKindName[int(rh->kind())]));
            o.insert(QStringLiteral("readerThemed"), rh->themed());
            o.insert(QStringLiteral("readerChrome"), rh->chromeVisible());
            if (NavGraph* rg = rh->navGraph())
            {
                o.insert(QStringLiteral("readerZone"), rg->zone());
                o.insert(QStringLiteral("readerIndex"), rg->index());
            }
            o.insert(QStringLiteral("readerPage"), rh->readerPage());
            o.insert(QStringLiteral("readerPageCount"), rh->readerPageCount());
            if (rh->kind() == ReaderKind::Book) o.insert(QStringLiteral("readerFont"), book_->fontPt());
            if (rh->kind() == ReaderKind::Comic) o.insert(QStringLiteral("readerTwoUp"), rh->readerTwoUp());
        }
        // The themed settings-panel host: its QQuickWidget focus is opaque, so surface the graph selection +
        // panel title + the focused row's label so panel automation is as precise as the Qt panels.
        if (themedPanelHost_ && cur == themedPanelHost_)
        {
            o.insert(QStringLiteral("panelTitle"), themedPanelHost_->panelTitle());
            o.insert(QStringLiteral("panelDepth"), themedPanelHost_->levelDepth());
            if (NavGraph* pg = themedPanelHost_->navGraph())
            {
                o.insert(QStringLiteral("panelZone"), pg->zone());
                o.insert(QStringLiteral("panelIndex"), pg->index());
            }
            o.insert(QStringLiteral("panelFocus"), themedPanelHost_->focusedRowLabel());
        }
#endif
        if (cur == playerPage_)
        {
            // Player-touch automation (D1 Task 5): chrome visibility (tap toggle) + seek position in permille of
            // the duration (double-tap ±10 s seek) — read straight off the live transport widgets.
            o.insert(QStringLiteral("mediaControls"), mediaControls_ && mediaControls_->isVisible());
            o.insert(QStringLiteral("playerPermille"), seek_ ? seek_->value() : 0);
            o.insert(QStringLiteral("playerDur"), duration_);
            // Sync-controls automation (player/sync-controls Task 2): the live mpv offsets (seconds), the boost
            // slider's value (0..200), the resume key the card writes offsets under, and whether the card is up.
            o.insert(QStringLiteral("audioDelay"), player_->audioDelay());
            o.insert(QStringLiteral("subDelay"), player_->subtitleDelay());
            o.insert(QStringLiteral("volume"), volume_ ? volume_->value() : 0);
            o.insert(QStringLiteral("syncKey"), syncKey_);
            o.insert(QStringLiteral("subCard"), subOverlay_ && subOverlay_->isVisible());
        }
        o.insert(QStringLiteral("escMenu"), escMenuVisible());
        o.insert(QStringLiteral("fullscreen"), isFullScreen());
        o.insert(QStringLiteral("active"), isActiveWindow());
        o.insert(QStringLiteral("size"), QStringLiteral("%1x%2").arg(width()).arg(height()));
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    h.screenshot = [this](const QString& path) { return grab().save(path); };
    h.openDoc = [this](const QString& path) {
        // reader tests open by path (pdf/cbz/epub -> the readers). Route video/audio extensions to the player so
        // the player-touch tests (tap chrome, double-tap seek) can reach playerPage_ headlessly too. m3u/m3u8
        // route through openVideoPath's isM3uRef branch -> a multi-file playlist queue (lets a test drive queue
        // advances + per-track sync keying).
        static const QSet<QString> media = { QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
            QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("mp3"),
            QStringLiteral("flac"), QStringLiteral("m4a"), QStringLiteral("wav"),
            QStringLiteral("m3u"), QStringLiteral("m3u8") };
        if (media.contains(QFileInfo(path).suffix().toLower())) { openVideoPath(path); return true; }
        return openDocumentPath(path);
    };
    h.touch = [this](const QString& arg) -> bool {
        // Qt-INTERNAL activation only (parity with sendKey) — no OS foreground change. Real touch on the
        // top-level window handle: Qt hit-tests + routes it into the widget tree / embedded QML scene exactly
        // as a physical touchscreen would (the whole point — no shortcut into the graph). Returns false (=> the
        // server replies `err busy`) when a sequence is already in flight, so overlaps can't corrupt touch state.
        if (!isActiveWindow()) QApplication::setActiveWindow(this);
        return uitestRunTouch(windowHandle(), arg, this);
    };
    uiTest_ = new UiTestServer(h, this);
    mwLog(QStringLiteral("uitest: control channel listening (%1)").arg(UiTestServer::serverName()));

    // Black-frame watchdog (debug-gated, same conditions as the UI-test channel): catch + self-heal the
    // intermittent all-black app state and name where it came from.
    //   sampler — the CURRENT THEMED PAGE's grab() scaled to 64x36. QWidget::grab() drives the widget through
    //             the software backend, so it renders the true frame even when the window is occluded (the
    //             uitest screenshot trick). Sampling only the themed QQuickWidget — the ONE surface that
    //             actually goes black — instead of rendering the whole ~1.8MP widget tree keeps the 1 Hz cost
    //             trivial (the Settings ▸ Debug toggle makes this a long-term telemetry path). Off the themed
    //             pages (classic panels etc.) fall back to the full-window grab.
    //   skip    — the contexts where an (all-)black grab is EXPECTED and must not trip the watchdog:
    //             * inContent_  — a game / video / e-reader / the emulator wait-page (see the currentChanged
    //               handler; emuPage_ + retro_ are the launch-handoff pages, so inContent_ IS that window);
    //             * minimised   — handed off to a standalone emulator (window down, nothing of ours to render);
    //             * escMenu     — the pause overlay is opening/animating over content.
    auto sampler = [this]() -> QImage {
        QWidget* cur = stack_ ? stack_->currentWidget() : nullptr;
        QWidget* src = (cur && (cur == themedHome_ || cur == themedBrowse_)) ? cur : this;
        return src->grab().scaled(64, 36, Qt::IgnoreAspectRatio, Qt::FastTransformation).toImage();
    };
    auto skip = [this]() -> bool {
        return inContent_ || isMinimized() || escMenuVisible();
    };
    blackWatchdog_ = new BlackFrameWatchdog(sampler, skip, this);
    connect(blackWatchdog_, &BlackFrameWatchdog::blackFrameDetected, this, [this](int consecutive) {
        // Telemetry on EVERY detection: name the screen + window state so a captured log says where the
        // blackout came from (the uitest `state` idiom, compacted to the load-bearing fields).
        QWidget* cur = stack_ ? stack_->currentWidget() : nullptr;
        const QString summary = QStringLiteral("page=%1 name=%2 esc=%3 min=%4 active=%5 fs=%6 size=%7x%8")
            .arg(cur ? QString::fromLatin1(cur->metaObject()->className()) : QStringLiteral("<null>"),
                 cur ? cur->objectName() : QString())
            .arg(escMenuVisible()).arg(isMinimized()).arg(isActiveWindow()).arg(isFullScreen())
            .arg(width()).arg(height());
        mwLog(QStringLiteral("watchdog: BLACK frame x%1 — %2").arg(consecutive).arg(summary));
        // The host fires the recovery kick on the SECOND consecutive black frame (a single stray black sample
        // is ignored — a mid-transition frame shouldn't force a repaint).
        if (consecutive == 2) kickThemedRepaint();
    });
    blackWatchdog_->start();
}

// Recovery kick for the black-frame watchdog: force the themed QML scene(s) to re-render. On the Qt 6.8
// SOFTWARE backend there is no GL context to lose, so the failure is a stale backing image / a scene the
// render loop thinks is clean — not a dropped graphics context. We therefore use the light, flicker-free
// invalidation path rather than QQuickWindow::releaseResources() (which drops scene-graph node caches and is
// aimed at GL context loss): a widget-level update(), a QQuickWindow scene update(), and a root polish() —
// together they re-run layout + repaint the scene into the widget's backing image. No-op off the themed home.
void MainWindow::kickThemedRepaint()
{
#ifdef MMV_HAVE_QML
    for (QWidget* w : { themedHome_, themedBrowse_ })
    {
        if (!w) continue;
        w->update();
        if (auto* qw = qobject_cast<QQuickWidget*>(w))
            if (QQuickWindow* win = qw->quickWindow()) win->update();
        if (QQuickItem* r = ThemeEngine::rootItem(w)) r->polish();
    }
#endif
}

void MainWindow::pollMenuPad()
{
    Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
    if (!pad || !pad->available()) return;
    // In a game the emulator polls and owns the pad; don't double-poll or inject keys over gameplay.
    if (retro_->running() || stack_->currentWidget() == retro_) return;
    // Only act when our window is focused, so we never steal input from another app. (All menus/overlays
    // are in-window children now, so the main window stays the active one while they're open.)
    if (!isActiveWindow()) return;
    // Someone is capturing raw input (the input-mapping dialog grabs the keyboard while binding a button):
    // injecting nav keys would fight the capture. Our own overlays also grab — those keep routing normally.
    if (QWidget* grabber = QWidget::keyboardGrabber(); grabber && !qobject_cast<NavOverlay*>(grabber)) return;
    // Don't fight MULTI-LINE text entry (a log viewer / notes field) where arrows must scroll/move the caret.
    // Single-line boxes stay pad-navigable: sendNavKey opens the on-screen keyboard on Enter and turns Back
    // into "leave the box" — the pad no longer goes dead when the selector lands on a search field.
    QWidget* fw = QApplication::focusWidget();
    if (!NavOverlay::topmost())
    {
        // A line edit being inline-edited on the keyboard keeps the pad out. But a nav-kit text view (the
        // Debug log) stays pad-driven EVEN in scroll mode — that's how the controller scrolls it and Escapes
        // back out (sendNavKey routes the pad keys into it). Only an UNGUARDED raw multi-line editor keeps
        // the pad out, as before.
        const bool textView = qobject_cast<QTextEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw);
        if (qobject_cast<QLineEdit*>(fw) && !static_cast<QLineEdit*>(fw)->isReadOnly()) return;
        if (textView && !fw->property("mmvNavText").toBool()) return;
    }

    pad->poll();
    padTick_ += 16;

    struct Nav { int id; int key; bool repeat; };
    static const Nav navs[] = {
        { PAD_UP,    Qt::Key_Up,        true  }, { PAD_DOWN,  Qt::Key_Down,      true  },
        { PAD_LEFT,  Qt::Key_Left,      true  }, { PAD_RIGHT, Qt::Key_Right,     true  },
        { PAD_B,     Qt::Key_Return,    false }, { PAD_A,     Qt::Key_Backspace, false },
        { PAD_START, Qt::Key_Escape,    false },
    };
    const int n = int(sizeof(navs) / sizeof(navs[0]));
    for (int i = 0; i < n; ++i)
    {
        bool held = false;
        for (int p = 0; p < Gamepad::kMaxPlayers; ++p) if (pad->button(unsigned(p), unsigned(navs[i].id))) { held = true; break; }
        if (held)
        {
            if (!padPrev_[i]) { sendNavKey(navs[i].key); padNext_[i] = padTick_ + 420; }        // press edge
            else if (navs[i].repeat && padTick_ >= padNext_[i]) { sendNavKey(navs[i].key); padNext_[i] = padTick_ + 110; } // hold-repeat
        }
        padPrev_[i] = held;
    }
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

// Focusable widgets of a layout in visual (top-to-bottom) order, recursing into nested layouts / container
// widgets. Used to build the panel's arrow-nav order so it matches what the user sees (findChildren doesn't
// guarantee order, which let the focused row fall out of the ring and jump focus to Back).
static void collectPanelRows(QLayout* lay, QVector<QWidget*>& out)
{
    if (!lay) return;
    for (int i = 0; i < lay->count(); ++i)
    {
        QLayoutItem* it = lay->itemAt(i);
        if (QWidget* wdg = it->widget())
        {
            if (wdg->isVisible() && (wdg->focusPolicy() & Qt::TabFocus)) out.push_back(wdg);
            else if (wdg->layout()) collectPanelRows(wdg->layout(), out);
        }
        else if (QLayout* sub = it->layout()) collectPanelRows(sub, out);
    }
}

// The settings/dialog panel's navigation order: header Back first, then the focusable rows top-to-bottom.
QVector<QWidget*> MainWindow::panelNavRing() const
{
    QVector<QWidget*> ring;
    if (panelBack_) ring.push_back(panelBack_);
    if (panelScroll_ && panelScroll_->widget()) collectPanelRows(panelScroll_->widget()->layout(), ring);
    return ring;
}

// Tab / directional focus (a remote's D-pad often maps "down" to focus-next, handled by Qt BEFORE
// keyPressEvent) would wrap from the last row back to the header Back. In a plain settings panel, clamp it to
// the same strict top-to-bottom list instead: never wrap, so the last row stays put on "next".
bool MainWindow::focusNextPrevChild(bool next)
{
    if (stack_->currentWidget() == panelPage_ && !panelDialog_)
    {
        const QVector<QWidget*> ring = panelNavRing();
        const int idx = ring.indexOf(focusWidget());
        if (idx >= 0)
        {
            ring[qBound(0, idx + (next ? 1 : -1), ring.size() - 1)]->setFocus(Qt::TabFocusReason);
            return true; // handled - suppress Qt's wrap-around
        }
    }
    return QMainWindow::focusNextPrevChild(next);
}

void MainWindow::keyPressEvent(QKeyEvent* e)
{
    // One Back rule for the whole app: Escape and Backspace both "go back" — to the previous screen on any
    // page, and to the app pause menu at the home root (see goBack). A focused text field consumes its own
    // Backspace before this (so typing still deletes); its Escape reaching here still means "leave", which is
    // right. The subtitle overlay (handled just below) and the emulator (consumes Esc itself) are the
    // exceptions that manage their own Back. F11 still toggles full screen independently.
    // Qt::Key_Back is Android's hardware/gesture Back: route it into the SAME unified Back so it lands on the
    // previous screen and, at the home root, opens the exit-confirm pause menu — never an instant app kill. It
    // is a distinct key from Backspace, so it never deletes text and is never treated as "typing" below. On
    // desktop the media/browser Back key (VK_BROWSER_BACK on Windows, XF86Back on X11) also maps to Key_Back;
    // handling it here as "go back" is the intended behavior, so this branch is not Android-only.
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Back)
    {
        QWidget* fw = focusWidget();
        // Backspace belongs to a text widget only while it's actually being INTERACTED with (a live cursor,
        // or a scrollable view in scroll mode). A widget that's merely SELECTED (the two-state NavTextField
        // outline) isn't typing/scrolling, so its Backspace/Escape goes back like anywhere else.
        auto* le = qobject_cast<QLineEdit*>(fw);
        const bool textView = qobject_cast<QTextEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw);
        // "Busy" = a widget actively taking keys: an editable line edit being edited, or ANY text widget in
        // its interacting (cursor/scroll) state. Its Backspace stays in the widget instead of going back.
        const bool typing = e->key() == Qt::Key_Backspace
                            && ((le && !le->isReadOnly())
                                || ((le || textView) && NavTextField::isInteracting(fw))
                                || qobject_cast<QAbstractSpinBox*>(fw));
        const bool subOpen = subOverlay_ && subOverlay_->isVisible(); // its own handler (below) closes it
        if (!typing && !subOpen) { goBack(); e->accept(); return; }
    }

    // Arrow-key / remote navigation for the inline settings AND dialog panels (Settings, Profiles, …): route
    // physical keys through the SAME geometric ring the controller uses (navCtx_ -> panelRing_), so both input
    // paths behave identically. Geometric nav places a row's side buttons Left/Right of it (e.g. a profile's
    // ✎/✕ beside the name) and the next row Down — matching where you'd expect to land, unlike the old linear
    // walk that dropped into the tiny edit button on the way down. Skipped while a text field is focused (so
    // typing keeps its keys); Backspace/Escape already went to the unified Back above.
    if (stack_->currentWidget() == panelPage_ && navCtx_)
    {
        QWidget* fw = focusWidget();
        const bool editing = qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw)
                           || qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QAbstractSpinBox*>(fw)
                           || (qobject_cast<QComboBox*>(fw) && qobject_cast<QComboBox*>(fw)->isEditable());
        if (!editing && navCtx_->routeKey(e->key())) { e->accept(); return; }
    }

    // Arrow-key / remote navigation for the media player transport. Left/Right move across the buttons,
    // Up reaches the top-left Back, Down returns to the transport row, Enter/Select activates, Space
    // toggles pause, Backspace exits. (A focused seek slider keeps Left/Right for scrubbing.)
    // The subtitle overlay, when open, captures navigation: arrows move across its controls, Enter activates,
    // Esc/Back closes it (rather than exiting the video).
    if (subOverlay_ && subOverlay_->isVisible())
    {
        auto* fw = qobject_cast<QPushButton*>(focusWidget());
        const bool inRight = subRightCol_.contains(fw);
        QVector<QPushButton*>& col = inRight ? subRightCol_ : subLeftCol_;
        int idx = col.indexOf(fw);
        switch (e->key())
        {
        case Qt::Key_Escape: case Qt::Key_Backspace: case Qt::Key_Back: hideSubtitleMenu(); return;
        case Qt::Key_Down: if (!col.isEmpty()) col[qMin(idx < 0 ? 0 : idx + 1, col.size() - 1)]->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Up:   if (!col.isEmpty()) col[qMax(idx < 0 ? 0 : idx - 1, 0)]->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Left: if (!subLeftCol_.isEmpty()) subLeftCol_[qBound(0, idx < 0 ? 0 : idx, subLeftCol_.size() - 1)]->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Right: if (!subRightCol_.isEmpty()) subRightCol_[qBound(0, idx < 0 ? 0 : idx, subRightCol_.size() - 1)]->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Select:
            if (fw) fw->click(); return;
        default: return; // swallow other keys while the panel is up
        }
    }

    if (stack_->currentWidget() == playerPage_)
    {
        switch (e->key())
        {
        case Qt::Key_Right: revealMediaControls(); stepPlayerFocus(+1); return;
        case Qt::Key_Left:  revealMediaControls(); stepPlayerFocus(-1); return;
        case Qt::Key_Up:    revealMediaControls(); if (videoBack_) videoBack_->setFocus(Qt::TabFocusReason); return;
        case Qt::Key_Down:  revealMediaControls(); stepPlayerFocus(0); return;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Select:
            revealMediaControls();
            if (auto* b = qobject_cast<QAbstractButton*>(focusWidget())) { b->click(); return; }
            player_->togglePause(); return;          // nothing focused -> play/pause
        case Qt::Key_Space: player_->togglePause(); revealMediaControls(); return;
        case Qt::Key_F12: captureVideoScreenshot(); revealMediaControls(); return;
        case Qt::Key_BracketRight: cyclePlaybackSpeed(+1); revealMediaControls(); return; // ]  faster
        case Qt::Key_BracketLeft:  cyclePlaybackSpeed(-1); revealMediaControls(); return; // [  slower
        default: break; // Backspace/Escape exit via the unified Back above (stop + return home)
        }
    }
    QMainWindow::keyPressEvent(e);
}

// Move keyboard focus across the visible transport buttons (dir +1/-1), or land on the row (dir 0).
void MainWindow::stepPlayerFocus(int dir)
{
    QVector<QPushButton*> vis;
    for (QPushButton* b : playerButtons_) if (b && b->isVisible()) vis.push_back(b);
    if (vis.isEmpty()) return;
    int idx = vis.indexOf(qobject_cast<QPushButton*>(focusWidget()));
    if (idx < 0) idx = (dir < 0 ? vis.size() - 1 : 0); // entering the row from Back / nowhere
    else if (dir != 0) idx = (idx + dir + vis.size()) % vis.size();
    vis[idx]->setFocus(Qt::TabFocusReason);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (focusedOnShow_) return;
    focusedOnShow_ = true;
    // Kick the background catalog warm-up, but only after the window has painted its first frame (see
    // PrefetchPaintKick) so the sweep's first request never delays the first paint.
    if (prefetcher_) qApp->installEventFilter(new PrefetchPaintKick(this, prefetcher_));
    // Become the active window and drop keyboard focus into the home view so arrow keys work without a
    // click first. Deferred a tick so it runs after the window is actually on screen / activated.
    raise();
    activateWindow();
    QTimer::singleShot(0, this, [this] {
        activateWindow();
        if (startupChooseProfile_)
        {
            // First run offers Restore-from-Drive vs. a new library BEFORE the profile picker. The pure router
            // (also pinned headlessly by probe_onboarding, and re-consulted by T2's restore flow) makes the call
            // so the branch can never drift: no local profiles + nothing picked yet ⇒ ChoiceScreen; existing
            // profiles ⇒ straight to the picker. onboarding/done short-circuits ahead of it so an already-onboarded
            // (or existing) user is byte-for-byte today's behavior — the choice screen is a pure first-run prepend.
            const bool hasLocal = !ProfileStore::list().isEmpty();
            const auto route = mmv::onboardingRoute(hasLocal, /*restorePicked*/ false, /*signInOk*/ false,
                                                    /*remoteHasProfiles*/ false, CloudSync::signInAvailable());
            if (!Settings::onboardingDone() && route == mmv::OnboardingRoute::ChoiceScreen)
                presentOnboardingChoice();
            else
                promptStartupProfile();                  // pick a user before anything else (unchanged path)
            return;
        }
        if (stack_->currentWidget() == home_ && home_) home_->focusContent();
        // No startup picker in the way: offer TV mode once, a tick later so any pending overlay settles first
        // (maybeOfferTvMode itself bails if a modal/overlay is up — same "no modal up" guard as the picker path).
        QTimer::singleShot(0, this, [this] { maybeOfferTvMode(); });
    });
}

// When the window is re-activated (alt-tab back, restore from minimised), the themed QQuickWidget doesn't
// automatically restore its QML scene's active focus, leaving arrow keys dead until a click. Re-focus it AND
// force focus onto the QML root so navigation keeps working. (The Esc menu is an in-window NavMenu overlay
// nowadays — closing it produces no ActivationChange; its revival runs in NavOverlay::dismiss instead.)
void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        QTimer::singleShot(0, this, [this] {
            QWidget* w = stack_ ? stack_->currentWidget() : nullptr;
            if (w && (w == themedHome_ || w == themedBrowse_))
            {
                w->setFocus(Qt::ActiveWindowFocusReason);
                // Restoring from a minimise (show() after showMinimized) can leave the themed QQuickWidget's
                // backing image stale until something invalidates it — the software backend won't repaint a
                // scene it thinks is clean. Schedule an explicit repaint so the home never comes back blank.
                w->update();
#ifdef MMV_HAVE_QML
                if (auto* r = ThemeEngine::rootItem(w)) r->forceActiveFocus();
#endif
            }
            // Alt-Tabbing away drops the focus widget; on the way back, put the selection right back where it
            // was (or the nearest valid row) so the selector is NEVER lost to a window switch. Runs on every
            // ring-managed screen (panels/library); other screens keep their own focus handling above.
            if (navCtx_) navCtx_->ensureFocus();
        });
}

// Inline "Who's using My Media Vault?" picker, shown once the window is up (replaces the pre-window popup).
void MainWindow::promptStartupProfile()
{
    startupChooseProfile_ = false;
#ifdef MMV_HAVE_QML
    // Themed mode: render the picker on the Nav Contract (ThemedPanelHost) instead of hosting the classic
    // ProfileDialog. The host is a persistent stack page constructed in the ctor (independent of themedHome_,
    // which openHome builds AFTER a profile is chosen) — so it can present pre-home. mustChoose = no Back escape.
    if (themedHomeEnabled() && themedPanelHost_) { presentProfilePicker(/*mustChoose*/ true); return; }
#endif
    auto* dlg = new ProfileDialog(/*mustChoose*/ true, this);
    showDialogPanel(tr("Who's using My Media Vault?"), dlg, [this, dlg](int result) {
        if (result == QDialog::Accepted && !dlg->selectedId().isEmpty())
        {
            ProfileStore::setCurrent(dlg->selectedId());
            openHome();                    // render for the chosen profile
            // The startup picker took this path, so showEvent returned before its own maybeOfferTvMode
            // singleShot — make the one-time offer now that home has landed (guards keep it idempotent).
            QTimer::singleShot(0, this, [this] { maybeOfferTvMode(); });
        }
        else
        {
            mwLog(QStringLiteral("quit: startup-picker declined"));
            QApplication::quit(); // declined to choose -> exit (matches the old must-choose behaviour)
        }
    }, [this] { mwLog(QStringLiteral("quit: startup-picker backed out")); QApplication::quit(); }); // Back == decline -> exit
}

// The ONE widget-side sizing chokepoint (D1 Task 3). Re-derives every widget metric from the FormFactor
// tokens and pushes them to the surfaces (overlays/OSK read the statics at construction; live widgets are
// resized in place). ALL token math lives here — the surfaces expose plain setters. Desktop is identity:
// with default tokens (uiScale 1.0, minHitPx 0) hitClamp(n)==n and int(px*1.0)==px, so every value below is
// exactly today's, pixel-for-pixel. Connected to FormFactor::changed + called once at startup.
void MainWindow::applyFormFactorWidgets()
{
    FormFactor& ff = FormFactor::instance();
    const qreal s   = ff.uiScale();
    const int   hit = ff.minHitPx();

    // Overlay / OSK: body fonts scale with uiScale; OSK key boxes go through hitClamp (the probed path).
    NavOverlay::setPanelFontPx(int(14 * s), int(16 * s));           // desktop: 14 / 16
    Osk::setKeyMetrics(ff.hitClamp(46), ff.hitClamp(40), int(15 * s)); // desktop: 46 / 40 / 15

    // Player transport chrome: floor each button (and the Back overlay) to the hit target when one is set,
    // otherwise clear the floor (desktop identity — Qt's default minimum, no size change).
    const QSize floorSz = hit > 0 ? QSize(hit, hit) : QSize(0, 0);
    for (QPushButton* b : playerButtons_) if (b) b->setMinimumSize(floorSz);
    if (videoBack_)     videoBack_->setMinimumSize(floorSz);
    if (streamIssueBtn_) streamIssueBtn_->setMinimumSize(floorSz);
    if (seek_) seek_->setMinimumHeight(hit); // desktop: 0 (no change); mobile: a grabbable track

    // Split-screen pane bars (only if the split view has been built): floor the pause/close hit targets.
    if (splitView_)
    {
        if (splitView_->paneA()) splitView_->paneA()->applyBarMinHit(hit);
        if (splitView_->paneB()) splitView_->paneB()->applyBarMinHit(hit);
    }
}

// One-time "this looks like a TV" suggestion (D1 Task 3). Fires at most once, only when the display really
// reads as a big living-room screen, and only while nothing else is on top. Either answer marks it done so it
// never asks again. Guards: auto mode (the user hasn't chosen), not already prompted, full screen, and a
// primary screen whose reported physical width is >= 700mm — an empty/zero physicalSize (unreliable EDID)
// fails silent (no prompt), never a false positive.
void MainWindow::maybeOfferTvMode()
{
    if (Settings::displayMode() != QStringLiteral("auto")) return;
    if (Settings::tvPromptDone()) return;
    // Themed mode only: the Display-mode revert row lives in the THEMED Appearance panel, so a classic user who
    // accepts "Use TV mode" would have no in-classic way back. Don't offer what they can't undo where they are.
    if (!themedHomeEnabled()) return;
    if (!isFullScreen()) return;
    if (NavOverlay::topmost() || QApplication::activeModalWidget()) return; // never over the startup picker/an overlay
    QScreen* scr = QGuiApplication::primaryScreen();
    if (!scr) return;
    qreal physWidthMm = scr->physicalSize().width(); // millimetres; empty/zero => unreliable, do not guess
    // Test-only seam (parity with the MMV_UITEST channel): the physical-size guard is hardware-bound and can't
    // be seeded from an ini, so let the UI-test harness substitute a screen width to exercise this prompt. Never
    // active in production — qEnvironmentVariableIsSet("MMV_UITEST") is only ever set by the test launcher.
    if (qEnvironmentVariableIsSet("MMV_UITEST") && qEnvironmentVariableIsSet("MMV_TEST_SCREEN_MM"))
        physWidthMm = qEnvironmentVariableIntValue("MMV_TEST_SCREEN_MM");
    if (physWidthMm < 700.0) return;

    const int r = NavConfirm::ask(
        tr("Big screen detected"),
        tr("This looks like a TV. Switch to TV mode for larger text and controls you can read from the couch?"),
        { tr("Not now"), tr("Use TV mode") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    Settings::setTvPromptDone(true); // either answer settles it — never ask again
    if (r == 1) { Settings::setDisplayMode(QStringLiteral("tv")); FormFactor::instance().refresh(); }
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
    if (currentNextSourceCapable_) { streamIssueBtn_->show(); streamIssueBtn_->raise(); }
    controlsHideTimer_->start(4000); // a comfortable "few seconds" of grace before fading out
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
    streamIssueBtn_->adjustSize();
    streamIssueBtn_->move(margin + videoBack_->width() + 10, margin); // just right of Back
    notifier_->reposition();
    if (subOverlay_) subOverlay_->setGeometry(player_->rect()); // keep the subtitle scrim covering the player
}

void MainWindow::notify(const QString& text, int ms)
{
    if (notifier_) notifier_->notify(text, ms);
}

// Hide the transport chrome NOW — the shared body of the idle-timeout and the touch tap-toggle. Clears keyboard
// focus off a transport button (so the next arrow press cleanly re-reveals + re-focuses) and, in full screen,
// blanks the cursor (never while the subtitle panel is open).
void MainWindow::hideMediaControls()
{
    QWidget* fw = focusWidget();
    if (fw && (fw == videoBack_ || fw == streamIssueBtn_ || (mediaControls_ && mediaControls_->isAncestorOf(fw))))
        fw->clearFocus();
    if (mediaControls_) mediaControls_->hide();
    if (videoBack_) videoBack_->hide();
    if (streamIssueBtn_) streamIssueBtn_->hide();
    if (isFullScreen() && stack_->currentWidget() == playerPage_ && !subOverlay_)
        player_->setCursor(Qt::BlankCursor);
}

// Touch tap on the player: a shown chrome hides, a hidden chrome reveals (revealMediaControls re-arms the idle
// timer). Only over an open media item.
void MainWindow::togglePlayerChrome()
{
    if (stack_->currentWidget() != playerPage_) return;
    if (mediaControls_ && mediaControls_->isVisible()) { controlsHideTimer_->stop(); hideMediaControls(); }
    else revealMediaControls();
}

// Player touch filter (touch-only — the mouse path is frozen). Single-finger tap on the BARE video is a tap
// candidate; a touch that lands on the visible transport chrome (slider/buttons) is DEFERRED (return false) so it
// rides synthesized mouse (QSlider drag / QPushButton tap need nothing new). Consuming the bare-video tap stops
// mouse synthesis so a tap can't double-fire. Movement past a slop cancels the tap (a bare-video drag is inert).
bool MainWindow::handlePlayerTouch(QTouchEvent* te)
{
    const auto pts = te->points();
    auto overControls = [this](const QPointF& p) {
        const QPoint ip = p.toPoint();
        return (mediaControls_ && mediaControls_->isVisible() && mediaControls_->geometry().contains(ip))
            || (videoBack_ && videoBack_->isVisible() && videoBack_->geometry().contains(ip))
            || (streamIssueBtn_ && streamIssueBtn_->isVisible() && streamIssueBtn_->geometry().contains(ip));
    };
    switch (te->type())
    {
    case QEvent::TouchBegin:
        if (pts.size() != 1 || overControls(pts.first().position())) { playerTouchTap_ = false; return false; }
        playerTouchTap_ = true;
        playerTouchStart_ = pts.first().position();
        return true;
    case QEvent::TouchUpdate:
        if (playerTouchTap_ && !pts.isEmpty()
            && (pts.first().position() - playerTouchStart_).manhattanLength() > 24)
            playerTouchTap_ = false;
        return playerTouchTap_;
    case QEvent::TouchEnd:
        if (!playerTouchTap_) return false;
        playerTouchTap_ = false;
        onPlayerTap(pts.isEmpty() ? playerTouchStart_ : pts.first().position());
        return true;
    default:
        return false;
    }
}

// Resolve a bare-video tap: a second tap within 350 ms is a double-tap seek (left third −10 s / right third
// +10 s via the EXACT relative-seek the ⏪/⏩ transport buttons fire, with a transient notify flash); a lone tap
// (the timer expires) toggles the chrome. A centre double-tap is a net single toggle.
void MainWindow::onPlayerTap(const QPointF& pos)
{
    if (playerTapTimer_->isActive())
    {
        playerTapTimer_->stop();
        const int w = player_->width();
        const qreal x = pos.x();
        if (x < w / 3.0)              { player_->seekRelative(-10.0); notify(tr("⏪  −10s"), 900); revealMediaControls(); }
        else if (x > 2.0 * w / 3.0)   { player_->seekRelative(10.0);  notify(tr("⏩  +10s"), 900); revealMediaControls(); }
        else                          togglePlayerChrome();
        return;
    }
    playerTapTimer_->start(350);
}

void MainWindow::hideNotice()
{
    if (notifier_) notifier_->hideNotice();
}

void MainWindow::showNextSourceFeedback(const QString& msg)
{
    // Over the player while playing (the status bar may be hidden in full screen); otherwise the status bar
    // (the book/PDF readers keep it visible).
    if (stack_->currentWidget() == playerPage_) notifier_->playerNotice(msg);
    else                                        statusBar()->showMessage(msg, kFeedbackLong);
}

void MainWindow::openFile()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Open Video"), QString(),
        tr("Video (*.mkv *.mp4 *.avi *.mov *.webm *.m4v *.wmv *.flv *.ts *.m2ts);;"
           "Playlists (*.m3u *.m3u8);;All files (*.*)"));
    if (f.isEmpty()) return;
    openVideoPath(f);
}

// Decide whether a VIDEO play hands off to a configured external player (VLC/MPC/Custom) instead of the
// built-in libmpv player. The single choke point every video entry point consults. Returns true iff the media
// was handed off (caller records Recent and returns). A restricted (kids) profile ALWAYS stays built-in — the
// external action is hidden and this refuses the handoff, so a restricted profile can't escape into another
// app. A configured external whose launch fails (missing/broken exe) notifies once and returns false, so the
// built-in player still gets the media. playRouteOverride_ (consumed here, one play only) lets a detail-view
// one-off action force this play external / built-in regardless of the default.
bool MainWindow::routePlay(const QString& urlOrPath, PlayRoute explicitRoute)
{
    const PlayRoute member = playRouteOverride_;
    playRouteOverride_ = PlayRoute::Default;                // consume the member — affects exactly one play
    // An explicit route (from MediaItem::playRouteHint on an async catalog leaf) wins; else the member (a
    // synchronous local/recents one-off); else Default (honour the configured player).
    const PlayRoute ov = (explicitRoute != PlayRoute::Default) ? explicitRoute : member;

    if (ProfileStore::current().restricted) return false;  // restricted profiles never leave the app
    if (ov == PlayRoute::ForceBuiltin)      return false;   // "Play with built-in player" one-off
    const ExternalPlayer::Kind k = ExternalPlayer::configuredKind();
    const bool forceExt = (ov == PlayRoute::ForceExternal);
    const bool wantExternal = forceExt || (k != ExternalPlayer::Kind::Builtin);
    if (!wantExternal) return false;                        // default is built-in and no override -> libmpv
#ifndef Q_OS_ANDROID
    if (!forceExt && k == ExternalPlayer::Kind::AndroidIntent) return false; // no intent target on desktop
#endif
    // A one-off forces a CONCRETE target even when the default is Built-in (configuredPath() would be empty):
    // resolveForceTarget() picks the configured kind, else a Custom path, else the first detected player.
    const bool ok = forceExt
        ? ExternalPlayer::launchExe(urlOrPath, ExternalPlayer::resolveForceTarget())
        : ExternalPlayer::launch(urlOrPath);
    if (ok) return true;                                   // handed off to the external app
    notify(tr("No app can play this."));                   // the external target couldn't take it
    return false;                                          // fall through to the built-in player
}

void MainWindow::openVideoPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.video"));
    if (StreamResolver::isM3uRef(path)) { streams_->resolve(path, QFileInfo(path).completeBaseName()); return; } // playlist, not a plain file
    if (splitTarget_) { splitTarget_->openVideo(path, QFileInfo(path).completeBaseName()); finishSplitOpen(); return; }
    // External-player handoff: a configured external player takes the file (Recent still recorded on both routes).
    if (routePlay(path)) {
        PerfTrace::end(QStringLiteral("open.video")); // close the span we opened above (no built-in load follows)
        RecentStore::add({ path, QFileInfo(path).completeBaseName(), QStringLiteral("video"), QString() });
        return;
    }
    notePlaybackStart();               // channel guard: the channel's own pick keeps it alive; a manual play ends it
    subCtx_ = {};                      // a local file isn't matched to a catalog title/IMDB id for subtitles
    currentNextSourceCapable_ = false; // a local file has no Allarr alternate source
    themedAudioSession_ = false;       // openVideoPath is VIDEO — keep the classic player page
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    session_->clearQueue();      // saves+clears any previous timed media (also clears syncKey_)
    session_->setMediaVideo(true); // consumption-stats: a local video accrues "watch" seconds
    session_->beginResume(path); // track this video's position (and resume it if we've watched it before)
    syncKey_ = path;             // a local file keys its sync offsets by its own path
    stack_->setCurrentWidget(playerPage_);
    player_->play(path);
    revealMediaControls();
    RecentStore::add({ path, QFileInfo(path).completeBaseName(), QStringLiteral("video"), QString() });
}

// Find a sibling cover image for a local audio file (cover.*/folder.*/front.*/albumart.* in the same folder),
// as a file URL — so a local audiobook shows real art on the themed now-playing page. "" when there is none.
static QString localCoverFor(const QFileInfo& fi)
{
    static const QStringList stems = { QStringLiteral("cover"), QStringLiteral("folder"),
                                       QStringLiteral("front"), QStringLiteral("albumart") };
    static const QStringList exts  = { QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                       QStringLiteral("png"), QStringLiteral("webp") };
    const QDir dir = fi.absoluteDir();
    for (const QString& s : stems)
        for (const QString& e : exts)
        {
            const QString p = dir.absoluteFilePath(s + QLatin1Char('.') + e);
            if (QFile::exists(p)) return QUrl::fromLocalFile(p).toString();
        }
    return QString();
}

// Build the `selected`-shaped data the themed now-playing page binds (title/subtitle + a `poster` art role so
// the cover art resolves through the same MediaArt chain the detail poster uses, with a graceful placeholder).
static QVariantMap makeThemedAudioData(const QString& title, const QString& subtitle, const QString& image)
{
    QVariantMap m;
    m.insert(QStringLiteral("title"), title);
    if (!subtitle.isEmpty()) m.insert(QStringLiteral("subtitle"), subtitle);
    if (!image.isEmpty())
    {
        m.insert(QStringLiteral("poster"), image);   // the primary art role the page reads
        m.insert(QStringLiteral("image"), image);
    }
    return m;
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
    const QString first = sel.first();
    const QFileInfo firstFi(first);
    themedAudioSession_ = themedHomeEnabled();
    themedAudioData_ = makeThemedAudioData(firstFi.completeBaseName(), QString(), localCoverFor(firstFi));
    session_->setQueue(sel, 0); // exactly the selected tracks, in the order the dialog returned them
    // consumption-stats: set the kind AFTER setQueue — setQueue's internal playIndex→persistResume flushes the
    // OUTGOING track's tail, which must accrue under ITS kind (not this audio's). No new-track heartbeat fires
    // synchronously (mpv loads async), so the new track's first accrual still sees "audio".
    session_->setMediaVideo(false);
    RecentStore::add({ first, firstFi.completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::openAudioPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.audio"));
    notePlaybackStart();               // channel guard: keep the channel iff this is its own audio pick
    currentNextSourceCapable_ = false; // a local file/folder has no Allarr alternate source
    const QFileInfo fi(path);
    QStringList queue;
    int start = 0;
    if (fi.suffix().toLower() == QStringLiteral("m4b"))
    {
        // An audiobook is one self-contained file (often with chapters) - don't queue the rest of the folder.
        // (Resume is handled generically for all timed media by PlaybackSession.)
        queue = { fi.absoluteFilePath() };
    }
    else
    {
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
    // Themed mode: this audio session shows the QML now-playing page (mpv plays invisibly). Seed its data from
    // what we hold locally — the file's base name, and a sibling cover image if the folder carries one.
    themedAudioSession_ = themedHomeEnabled();
    themedAudioData_ = makeThemedAudioData(fi.completeBaseName(), QString(), localCoverFor(fi));
    session_->setQueue(queue, start);
    // consumption-stats: kind AFTER setQueue — the outgoing track's flush (setQueue→playIndex→persistResume)
    // must accrue under its own kind; the new audio track's first heartbeat (mpv loads async) still sees "audio".
    session_->setMediaVideo(false);
    RecentStore::add({ fi.absoluteFilePath(), fi.completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::onTrackEnded()
{
    session_->handleTrackEnd(); // scrobble-stop / next-episode now hang off PlaybackSession::queueFinished
}

// Resolve and play the episode after the one that just finished. The current episode's id is "ttShow:s:e";
// try same-season ep+1 first, then next-season ep1 (best-effort: the stream resolver is the source of truth
// for whether an episode exists / is available). No-op for anything that isn't a TV episode.
void MainWindow::tryPlayNextEpisode()
{
    if (stack_->currentWidget() != playerPage_) return;      // only while a video is on screen
    const QStringList parts = subCtx_.imdbStreamId.split(QLatin1Char(':'));
    if (parts.size() < 3) return;                            // not "ttShow:season:episode"
    const QString show = parts.value(0);
    const int s = parts.value(1).toInt(), e = parts.value(2).toInt();
    if (show.isEmpty() || s <= 0 || e <= 0) return;

    const QString nextEp = QStringLiteral("%1:%2:%3").arg(show).arg(s).arg(e + 1);
    const QString nextSeason = QStringLiteral("%1:%2:%3").arg(show).arg(s + 1).arg(1);
    notifier_->playerNotice(tr("Up next — finding the next episode…"), 20000);
    addons_->resolveStreamByImdb(QStringLiteral("series"), nextEp,
        [this, nextEp, nextSeason](const QString& url, const QString& mime) {
        if (!url.isEmpty()) { playResolvedEpisode(nextEp, url, mime); return; }
        // End of season? Try the first episode of the next one before giving up.
        addons_->resolveStreamByImdb(QStringLiteral("series"), nextSeason,
            [this, nextSeason](const QString& url2, const QString& mime2) {
            if (!url2.isEmpty()) playResolvedEpisode(nextSeason, url2, mime2);
            else { notifier_->hidePlayerNotice(); notify(tr("No next episode found — that looks like the finale."), kFeedbackLong); }
        });
    });
}

void MainWindow::playResolvedEpisode(const QString& imdbStreamId, const QString& url, const QString& mime)
{
    notifier_->hidePlayerNotice();
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    MediaItem it;
    it.url = url;
    it.mime = mime;
    it.type = QStringLiteral("episode");
    it.imdbStreamId = imdbStreamId;
    it.title = tr("Season %1 · Episode %2").arg(p.value(1), p.value(2));
    it.id = imdbStreamId; // stable resume/Recent key for this episode
    notify(tr("Up next: %1").arg(it.title), 4000);
    openLibraryItem(it); // plays it, and re-arms subCtx_ so the following episode auto-advances too
}

// ---- Channel mode ------------------------------------------------------------------------------------------
// A channel turns a video/audio playlist into a personal shuffle-TV network: it airs a random item, and on each
// NATURAL end (the EOF-gated queueFinished seam) shows a cancelable 5 s countdown then airs the next bag pick.
// The bag draws every item once before repeating (no immediate repeat across a reshuffle). State is session-
// only and is cleared on every user stop / Back / manual play (see exitChannel + notePlaybackStart).

void MainWindow::startChannel(const QString& playlistId)
{
    Playlist p;
    if (!PlaylistStore::get(playlistId, p) || p.items.isEmpty())
    { notify(tr("This playlist is empty."), kFeedbackShort); return; }
    channelPlaylistId_ = playlistId;
    channelBag_.reset(int(p.items.size())); // a fresh shuffled bag for this run
    channelSkips_ = 0;
    notify(tr("Channel started — playing “%1” on shuffle.").arg(p.name), kFeedbackStandard);
    airChannelPick(channelBag_.next());      // air the first pick immediately (no interstitial before item one)
}

// After a pick ends naturally: draw the next bag index, confirm the playlist still exists, show the countdown
// interstitial, and air the pick (or exit the channel on Cancel/Back). Runs deferred (queued) from the EOF seam.
void MainWindow::advanceChannel()
{
    if (!channelActive()) return;            // a stop/Back between the EOF and this queued call already exited
    Playlist p;
    if (!PlaylistStore::get(channelPlaylistId_, p) || p.items.isEmpty())
    { exitChannel(); openHome(); return; }   // playlist vanished under us -> end the channel, land on Home

    // Draw the next pick, but SKIP any that would only open a detail page (an addon movie/episode, a container,
    // a stream-less item) BEFORE the interstitial — so the countdown only ever names something that will really
    // play (no interstitial for skips). If every remaining pick detours, nothing plays directly: stop with a
    // notice. A remote leaf counts as playable here; a rare async-resolve miss is caught after airing.
    int next = channelBag_.next();
    for (int skipped = 0; !home_->channelItemPlaysDirectly(channelPlaylistId_, next); ++skipped)
    {
        if (skipped + 1 >= int(p.items.size()))
        {
            exitChannel();
            notify(tr("Channel stopped — nothing in this playlist can play directly."), kFeedbackLong);
            return;
        }
        next = channelBag_.next();
    }
    if (next < 0 || next >= p.items.size()) { exitChannel(); openHome(); return; }
    const QString title = p.items[next].title.isEmpty() ? tr("the next item") : p.items[next].title;

    // The interstitial: "Next: <title> — starting in N s", {Cancel, Play now}, focus + cancel on Cancel (the
    // house-safe default), auto-accepting "Play now" (index 1) at zero. `%1` is pre-substituted with the title;
    // NavCountdown fills the remaining `%2` with the live seconds each tick.
    const QString msgTmpl = tr("Next: %1 — starting in %2 s").arg(title);
    const int choice = NavCountdown::ask(tr("Channel"), msgTmpl, { tr("Cancel"), tr("Play now") },
                                         /*seconds=*/5, /*acceptIndex=*/1, /*focusIndex=*/0,
                                         /*cancelIndex=*/0, this);
    if (choice != 1)                         // Cancel or Back: end the channel and return Home (least-surprising
    { exitChannel(); openHome(); return; }   // resting place — the previous item already finished on the player)
    if (!channelActive()) return;            // defensive: a stop path fired during the modal loop
    airChannelPick(next);
}

// Drive one playlist entry through HomeView's per-entry open path (identical to activating that row). Sets the
// synchronous latch so the play sink this pick reaches keeps the channel alive; a NEW gen tags any async result.
// The pick can resolve three ways:
//   * Played  (sync): a local file / already-resolved url -> the sink consumed the latch during this call.
//   * Detoured(sync): an info-page/container/stream-less item would open a DETAIL page -> HomeView suppressed it;
//                     the channel SKIPS to the next pick (no interstitial), never wedging on a detail surface.
//   * Pending (async): a remote /stream resolve is in flight -> onChannelPickResolved / onChannelPickDetoured
//                     decide later, gated on the gen so a superseded airing is dropped.
void MainWindow::airChannelPick(int index)
{
    if (index < 0) { exitChannel(); return; }
    channelAiring_ = true;
    const int gen = ++channelAirGen_;               // this airing's identity (gates its async result)
    const HomeView::ChannelAir r = home_->playChannelItem(channelPlaylistId_, index, gen);
    // Clear the latch unconditionally now the synchronous dispatch has returned. A normal in-app play already
    // consumed it in notePlaybackStart (this re-clear is a no-op). But an external-player divert also reports
    // Played — its sink early-returns from routePlay BEFORE notePlaybackStart, leaving the latch SET. Without
    // this clear the next unrelated in-app play (e.g. openAudioPath, no routePlay) would be adopted as the
    // channel's pick. The divert is not distinguishable from a real in-app play here (both report Played), so
    // the unconditional clear is the fix; a channel simply can't EOF-chain through an external player.
    channelAiring_ = false;
    if (r == HomeView::ChannelAir::Detoured) channelSkip(); // this pick can't play directly -> next pick, silently
    // Played: the channel stays live (channelPlaylistId_ still set) so a natural in-app end chains to the next
    //         pick; an external-divert Played leaves nothing to chain, which is the by-design channel exit.
    // Pending: the gen-gated async slot decides.
}

// A pick detoured (a detail page / no stream). Skip to the next bag pick immediately — NO interstitial (skips are
// silent) — until one plays. If every pick in the playlist skips (consecutive skips reach the playlist size),
// nothing can play directly: stop the channel with a notice. channelSkips_ is reset to 0 by the next real play.
void MainWindow::channelSkip()
{
    const int n = channelBag_.size();
    if (n <= 0) { exitChannel(); return; }
    if (++channelSkips_ >= n)
    {
        exitChannel();
        notify(tr("Channel stopped — nothing in this playlist can play directly."), kFeedbackLong);
        return;
    }
    airChannelPick(channelBag_.next());
}

// Async pick got a stream. If this airing is still the current one (gen match) and the channel is still live,
// play it (arming the latch just before the sink so it's kept). A stale result — a manual play or exit bumped the
// gen — is dropped, so a superseded pick never interrupts nor secretly revives the channel.
void MainWindow::onChannelPickResolved(int gen, const MediaItem& item)
{
    if (gen != channelAirGen_ || !channelActive()) return; // superseded / channel gone -> drop
    channelAiring_ = true;                                  // consumed synchronously by openLibraryItem's sink
    openLibraryItem(item);
    channelAiring_ = false;                                 // an external-player divert never reaches notePlaybackStart
                                                            // (routePlay early-returns), leaving the latch set -> the next
                                                            // in-app play would be adopted. Clear unconditionally; the
                                                            // in-app Played case already consumed it (harmless re-clear).
}

// Async pick produced no stream. If still current, skip it like a sync detour; a stale result is dropped.
void MainWindow::onChannelPickDetoured(int gen)
{
    if (gen != channelAirGen_ || !channelActive()) return;
    channelAiring_ = false;
    channelSkip();
}

// Every user-stop / Back / queue-clear-to-home path AND the start of any non-channel playback clears the channel,
// so the next natural end after ANY exit does not chain. Bumping the gen invalidates any in-flight async pick.
// Idempotent (safe when no channel is live).
void MainWindow::exitChannel()
{
    channelPlaylistId_.clear();
    channelAiring_ = false;
    channelSkips_ = 0;
    ++channelAirGen_;            // a pending async pick's result is now stale -> it will be dropped
    channelBag_.reset(0);
}

// A play sink was reached. If the channel armed this play (its own pick, sync or a just-resolved async one),
// consume the latch, reset the skip run, and keep the channel. Otherwise this is a user-initiated play, which
// ends any live channel (a manual play kills it). Because channelAiring_ is only ever true across a synchronous
// dispatch, a manual play interleaved during a pending async resolve sees it FALSE here and correctly exits.
void MainWindow::notePlaybackStart()
{
    if (channelAiring_) { channelAiring_ = false; channelSkips_ = 0; return; } // the channel's own pick — keep it
    if (channelActive()) exitChannel();                                         // a manual play supersedes the channel
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

void MainWindow::openGamePath(const QString& rom, const QString& title, const QString& thumb,
                              const QString& key, const QString& systemHint)
{
    PerfTrace::begin(QStringLiteral("open.game")); // ended in GameLauncher (libretro openGame or external runEmulator)
    notePlaybackStart();  // a manual game launch is non-channel playback -> ends any idle channel (never airs games)
    if (splitTarget_) // run the ROM in the focused pane's own emulator instead of the full-screen one
    {
        const GameLauncher::CorePlan plan = launcher_->prepareCore(rom, systemHint);
        if (!plan.error.isEmpty()) { notifier_->notify(plan.error, plan.errorMs); return; }
        if (!plan.externalEmulatorId.isEmpty()) // a standalone emulator owns its own window; it can't embed in a split pane
        {
            const ExternalEmulator* em = EmulatorRegistry::byId(plan.externalEmulatorId);
            if (!em)
            {
                const GameSystem* sys = SystemCatalog::byId(plan.systemId); // old launchExternalGame used the system NAME here
                // J11: match the sibling error above (notifier_->notify) — one channel for the whole split branch,
                // since the status bar may be hidden in split view. "No emulator" is an error-class failure.
                notifier_->notify(tr("No emulator is configured for %1.").arg(sys ? sys->name : plan.systemId), kFeedbackLong);
                return;
            }
            // Informational: the game DID launch (full-screen), it just can't embed in a pane → ambient/standard.
            notifier_->notify(tr("%1 opens in its own window, not a split pane.").arg(em->displayName), kFeedbackStandard);
            finishSplitOpen();
            // Full-screen launch, as before. We route back through open() (a cheap re-resolve of the already-
            // extracted descriptor) rather than calling runEmulator directly, because open()'s external branch
            // also applies the Android guard and the empty-title→file-name fallback for the Recent entry.
            launcher_->open(plan.launchRom, title, thumb, key, systemHint);
            return;
        }
        // Download the core (if missing), then any BIOS the system needs (3DO, Saturn, PlayStation), then load
        // the pane (best-effort BIOS; a failure falls back to the core's own message). Both fetches are
        // asynchronous, so the GUI thread never waits on the network: we return to the split view right away
        // with progress on the toast, and the pane's core loads once the files land (immediately, when nothing
        // is missing). The fetch context is parented to the pane — a pane closed mid-download cancels the
        // load — and recreated per open so a newer game supersedes a still-downloading one.
        const QString recentTitle = title.isEmpty() ? QFileInfo(plan.launchRom).completeBaseName() : title;
        MediaPane* pane = splitTarget_;
        delete splitLaunchCtx_;
        splitLaunchCtx_ = new QObject(pane);
        launcher_->ensureCoreThen(plan, splitLaunchCtx_,
            [this, pane, recentTitle, thumb, key](const GameLauncher::CorePlan& ready) {
            CoreManager::ensureBiosAsync(ready.systemId, CoreManager::systemDir(), splitLaunchCtx_,
                [this](const QString& s) {
                    statusBar()->showMessage(s);
                    notifier_->notify(s, 8000); // the status bar is hidden app-wide; the toast is the visible channel
                },
                [this, pane, ready, recentTitle, thumb, key] {
                    mwLog(QStringLiteral("game: launching in split pane"));
                    pane->openGame(ready.corePath, ready.launchRom, ready.core);
                    RecentStore::add({ ready.launchRom, recentTitle, QStringLiteral("game"), thumb, key, ready.systemId });
                    PlayStats::markPlayed(PlayStats::identity(key, ready.launchRom)); // split panes aren't session-timed
                });
        });
        finishSplitOpen();
        return;
    }
    launcher_->open(rom, title, thumb, key, systemHint);
}

// ---- External (standalone) emulators: the RetroBat / ES-DE launch-and-monitor model -----------------

void MainWindow::ensureEmuPage()
{
    if (emuPage_) return;
    emuPage_ = new QWidget(this);
    emuPage_->setStyleSheet(QStringLiteral("background:#0e1014;"));
    auto* v = new QVBoxLayout(emuPage_);
    v->addStretch(1);
    emuLabel_ = new QLabel(tr("Starting…"), emuPage_);
    emuLabel_->setAlignment(Qt::AlignCenter);
    emuLabel_->setWordWrap(true);
    emuLabel_->setStyleSheet(QStringLiteral("color:#e8e8e8;font-size:20px;"));
    v->addWidget(emuLabel_);
    v->addSpacing(18);
    emuStopBtn_ = new QPushButton(tr("Force-close emulator"), emuPage_);
    emuStopBtn_->setFixedWidth(240);
    emuStopBtn_->setVisible(false);
    connect(emuStopBtn_, &QPushButton::clicked, this, [this] { launcher_->forceCloseEmulator(); });
    v->addWidget(emuStopBtn_, 0, Qt::AlignHCenter);
    v->addStretch(1);
    stack_->addWidget(emuPage_);
}

void MainWindow::openEmulatorManager()
{
#ifdef MMV_HAVE_QML
    // Themed mode: folder Info row + Change… Action (native QFileDialog — the documented exception) + fullscreen
    // Toggle (same setter) + per-emulator Separator (display name) + status Info row (installed path / "Not
    // installed.") + Download/Update Action + Launch Action — the SAME launcher_->install()/runEmulator() calls
    // as classic. Install progress: GameLauncher forwards EmulatorManager::status; we patch the installing
    // emulator's status row in place (top-gated via panelPageConns_ per the lifetime model at openCloudSync's
    // connect block), and completion/failure rebuilds so the status + button reflect the new state. Hub child ->
    // nested present(), Back -> openSettingsHub. The classic builder below is UNTOUCHED (classic mode).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // this present replaces the pool (lifetime model at openCloudSync's connect block)
        themedPanelHost_->setStyle(settingsPanelStyle());

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("emu.folder"); r.label = tr("Folder");
          r.value = EmulatorManager::emulatorsRoot(); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("emu.changefolder");
          r.label = tr("Change folder…"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("emu.fullscreen");
          r.label = tr("Launch emulators full screen"); r.checked = EmulatorManager::launchFullscreen(); rows << r; }

        for (const ExternalEmulator& em : EmulatorRegistry::all())
        {
            const QString bin = EmulatorManager::resolveBinary(em);
            { PanelRow r; r.kind = PanelRow::Separator; r.id = QStringLiteral("emu.sep:") + em.id;
              r.label = em.displayName; rows << r; }
            { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("emu.status:") + em.id; r.label = tr("Status");
              r.value = bin.isEmpty() ? tr("Not installed.") : bin; rows << r; }
            { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("emu.install:") + em.id;
              r.label = bin.isEmpty() ? tr("Download %1").arg(em.displayName)
                                      : tr("Re-download / Update %1").arg(em.displayName); rows << r; }
            { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("emu.launch:") + em.id;
              r.label = tr("Launch %1").arg(em.displayName); rows << r; }
        }

        auto onAct = [this](const QString& id, const QString& val) {
            if (id == QStringLiteral("emu.changefolder")) {
                const QString d = QFileDialog::getExistingDirectory(this, tr("Emulators folder"),
                                                                    EmulatorManager::emulatorsRoot());
                if (!d.isEmpty()) { EmulatorManager::setEmulatorsRoot(d); openEmulatorManager(); }
            }
            else if (id == QStringLiteral("emu.fullscreen")) {
                EmulatorManager::setLaunchFullscreen(val == QStringLiteral("1"));
            }
            else if (id.startsWith(QStringLiteral("emu.install:"))) {
                const QString emId = id.mid(id.indexOf(QLatin1Char(':')) + 1);   // emulator ids carry no colon
                const ExternalEmulator* em = EmulatorRegistry::byId(emId);
                if (!em) return;
                if (launcher_->emulatorBusy()) {
                    statusBar()->showMessage(tr("An emulator operation is already running."), kFeedbackLong); return; }
                emInstallId_ = emId;
                PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("emu.status:") + emId; r.label = tr("Status");
                r.value = tr("Downloading…"); themedPanelHost_->updateRow(r.id, r);
                statusBar()->showMessage(tr("Downloading %1…").arg(em->displayName));
                launcher_->install(*em);
            }
            else if (id.startsWith(QStringLiteral("emu.launch:"))) {
                const QString emId = id.mid(id.indexOf(QLatin1Char(':')) + 1);
                if (const ExternalEmulator* em = EmulatorRegistry::byId(emId)) launcher_->runEmulator(*em);
            }
        };
        auto onBack = [this] { openSettingsHub(); };   // Emulators is a hub child

        if (themedPanelHost_->panelTitle() == tr("Emulators"))
            themedPanelHost_->replaceTop(tr("Emulators"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("Emulators"), rows, onAct, onBack);

        // Install stream (Settings ▸ Emulators). Progress ticks patch the installing emulator's status row in
        // place; completion rebuilds (installed path + "Re-download" label); failure shows the error on the row.
        // Top-gated per the lifetime model — a status tick from a game-launch install (panel not up) is dropped.
        panelPageConns_ << connect(launcher_, &GameLauncher::emulatorInstallProgress, this,
                                   [this](const QString& t, int pct) {
            if (!themedPanelIsTop(tr("Emulators")) || emInstallId_.isEmpty()) return;
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("emu.status:") + emInstallId_;
            r.label = MainWindow::tr("Status"); r.value = pct >= 0 ? MainWindow::tr("%1 — %2%").arg(t).arg(pct) : t;
            themedPanelHost_->updateRow(r.id, r); });
        panelPageConns_ << connect(launcher_, &GameLauncher::emulatorInstallFinished, this, [this](const QString&) {
            emInstallId_.clear();
            if (themedPanelIsTop(tr("Emulators"))) openEmulatorManager(); });   // rebuild: now-installed state
        panelPageConns_ << connect(launcher_, &GameLauncher::emulatorInstallFailed, this, [this](const QString& msg) {
            if (themedPanelIsTop(tr("Emulators")) && !emInstallId_.isEmpty()) {
                PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("emu.status:") + emInstallId_;
                r.label = MainWindow::tr("Status"); r.value = MainWindow::tr("Failed: %1").arg(msg);
                themedPanelHost_->updateRow(r.id, r); }
            emInstallId_.clear(); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("Emulators"), [this](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("Standalone emulators are kept in their own folder and launched to play "
            "their systems (Dolphin runs GameCube/Wii). They open in their own window — close the emulator "
            "to come back here."));
        intro->setWordWrap(true);
        intro->setStyleSheet(QStringLiteral("color:#bbb;font-size:13px;"));
        v->addWidget(intro);

        // The emulators folder (RetroBat/ES-DE "emulators/<name>" layout); repoint it at an existing copy.
        auto* folderRow = new QHBoxLayout();
        folderRow->addWidget(new QLabel(tr("Folder:")));
        auto* folderVal = new QLabel(EmulatorManager::emulatorsRoot());
        folderVal->setStyleSheet(QStringLiteral("color:#9cf;"));
        folderVal->setWordWrap(true);
        folderRow->addWidget(folderVal, 1);
        auto* change = new QPushButton(tr("Change…"));
        connect(change, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(this, tr("Emulators folder"),
                                                                EmulatorManager::emulatorsRoot());
            if (!d.isEmpty()) { EmulatorManager::setEmulatorsRoot(d); openEmulatorManager(); }
        });
        folderRow->addWidget(change);
        v->addLayout(folderRow);

        auto* fs = new QCheckBox(tr("Launch emulators full screen"));
        fs->setChecked(EmulatorManager::launchFullscreen());
        connect(fs, &QCheckBox::toggled, this, [](bool on) { EmulatorManager::setLaunchFullscreen(on); });
        v->addWidget(fs);

        v->addSpacing(10);
        for (const ExternalEmulator& em : EmulatorRegistry::all())
        {
            auto* name = new QLabel(QStringLiteral("<b>%1</b>").arg(em.displayName));
            name->setStyleSheet(QStringLiteral("font-size:16px;"));
            v->addWidget(name);

            const QString bin = EmulatorManager::resolveBinary(em);
            auto* st = new QLabel(bin.isEmpty() ? tr("Not installed.") : tr("Installed: %1").arg(bin));
            st->setStyleSheet(bin.isEmpty() ? QStringLiteral("color:#e0a030;") : QStringLiteral("color:#7fc77f;"));
            st->setWordWrap(true);
            v->addWidget(st);

            const ExternalEmulator emCopy = em;
            auto* btnRow = new QHBoxLayout();
            auto* dl = new QPushButton(bin.isEmpty() ? tr("Download %1").arg(em.displayName)
                                                     : tr("Re-download / Update %1").arg(em.displayName));
            connect(dl, &QPushButton::clicked, this, [this, emCopy] {
                if (launcher_->emulatorBusy()) { statusBar()->showMessage(tr("An emulator operation is already running."), kFeedbackLong); return; }
                statusBar()->showMessage(tr("Downloading %1…").arg(emCopy.displayName));
                launcher_->install(emCopy);
            });
            btnRow->addWidget(dl, 1);
            // Launch the emulator with no game - opens its own UI. Primary use for launcher-style emulators
            // (TeknoParrot); for the others it's handy for first-run setup (BIOS/firmware/keys).
            auto* launchBtn = new QPushButton(tr("Launch"));
            connect(launchBtn, &QPushButton::clicked, this, [this, emCopy] { launcher_->runEmulator(emCopy); });
            btnRow->addWidget(launchBtn);
            v->addLayout(btnRow);
            v->addSpacing(8);
        }
    }, [this] { openSettingsHub(); });
}

// Inline form (no popup) to paste a link and stream it. libmpv handles http(s) and most streaming
// protocols (HLS, etc.) for both audio and video; audio-only streams show the "now playing" overlay.
void MainWindow::openStreamPrompt()
{
#ifdef MMV_HAVE_QML
    // Themed mode: a URL TextField (via the OSK) + a Play Action that feeds openStreamUrl() exactly. This is a
    // ROOT panel reached from the home flows (onRequestOpenFile "stream"), NOT a hub child — so it is a fresh
    // reset()+present() with Back returning to home (its classic onBack).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // reset() below is a lifetime boundary too — no panel may keep async listeners
        themedPanelHost_->reset();                        // fresh root presentation from home
        themedPanelHost_->setStyle(settingsPanelStyle());

        auto pending = std::make_shared<QString>();
        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("stream.hint");
          r.label = tr("Paste a direct http(s), HLS or .m3u/.m3u8 link"); rows << r; }
        { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("stream.url"); r.label = tr("Link"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("stream.err"); r.label = tr("Status"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("stream.play"); r.label = tr("▶  Play"); rows << r; }

        themedPanelHost_->present(tr("Stream from a link"), rows,
            [this, pending](const QString& id, const QString& val) {
                if (id == QStringLiteral("stream.url")) *pending = val;
                else if (id == QStringLiteral("stream.play")) {
                    const QString link = pending->trimmed();
                    if (!link.contains(QStringLiteral("://"))) {
                        PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("stream.err"); r.label = tr("Status");
                        r.value = tr("Enter a full http(s) link."); themedPanelHost_->updateRow(QStringLiteral("stream.err"), r);
                        return;
                    }
                    openStreamUrl(link);   // leaves the host for the player page
                }
            },
            [this] { openHome(); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("Stream from a link"), [this](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("Paste a direct audio or video link (http/https, HLS, or an .m3u/.m3u8 "
                                    "playlist) to stream it."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);

        auto* url = new QLineEdit();
        url->setMinimumHeight(34);
        url->setPlaceholderText(tr("https://example.com/stream.m3u8"));
        v->addWidget(url);

        auto* err = new QLabel(); err->setStyleSheet(QStringLiteral("color:#c0392b;font-size:13px;"));
        v->addWidget(err);

        auto play = [this, url, err] {
            const QString link = url->text().trimmed();
            if (!link.contains(QStringLiteral("://"))) { err->setText(tr("Enter a full http(s) link.")); return; }
            openStreamUrl(link);
        };
        auto* playBtn = panelRow(tr("▶  Play"));
        connect(playBtn, &QPushButton::clicked, this, play);
        connect(url, &QLineEdit::returnPressed, this, play);
        v->addWidget(playBtn);
    }, [this] { openHome(); });
}

void MainWindow::openStreamUrl(const QString& url, const QString& resumeKey, const QString& title)
{
    if (splitTarget_) { splitTarget_->openVideo(url, title); finishSplitOpen(); return; }
    // Playlists need fetching + dispatch (HLS stream vs. channel list vs. disc set); everything else is a
    // single link libmpv can play straight away. streams_->resolve() classifies it and emits back on a signal:
    // an HLS master → playDirect (→ playStream), a channel/media list → playQueue (→ setQueue).
    if (StreamResolver::isM3uRef(url)) { streams_->resolve(url, title); return; }
    playStream(url, resumeKey, title);
}

void MainWindow::playStream(const QString& url, const QString& resumeKey, const QString& title)
{
    PerfTrace::begin(QStringLiteral("open.video"));
    // External-player handoff: hand the link straight to the configured external player (Recent on both routes).
    if (routePlay(url)) {
        PerfTrace::end(QStringLiteral("open.video")); // close the span we opened above (no built-in load follows)
        const QUrl u(url);
        QString t = title;
        if (t.isEmpty()) t = u.fileName();
        if (t.isEmpty()) t = u.host();
        if (t.isEmpty()) t = url;
        RecentStore::add({ url, t, QStringLiteral("video"), QString(), resumeKey });
        return;
    }
    notePlaybackStart();               // channel guard (built-in stream play): keep the channel iff this is its pick
    subCtx_ = {};                      // a pasted/Recent link has no catalog metadata to match a subtitle by
    themedAudioSession_ = false;       // playStream is VIDEO — keep the classic player page
    stopScrobble();                    // leaving whatever was playing
    castUrl_ = url; castTitle_ = title; castMime_.clear(); // a pasted/Recent link is castable as-is
    currentNextSourceCapable_ = false; // a pasted/Recent stream link isn't a swappable Allarr source
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    session_->clearQueue();      // saves+clears any previous timed media
    session_->setMediaVideo(true); // consumption-stats: a streamed video accrues "watch" seconds
    // Resume + Recent keyed by the stable id when given (a re-opened catalog stream), else by the link itself.
    const QString rkey = resumeKey.isEmpty() ? url : resumeKey;
    session_->beginResume(rkey);
    syncKey_ = rkey;             // sync offsets follow the stable resume key (survives a re-resolved debrid URL)
    stack_->setCurrentWidget(playerPage_);
    player_->play(url);
    revealMediaControls();
    // A readable title for the Recent list: the supplied title, else the link's file name / host / raw link.
    const QUrl u(url);
    QString t = title;
    if (t.isEmpty()) t = u.fileName();
    if (t.isEmpty()) t = u.host();
    if (t.isEmpty()) t = url;
    RecentStore::add({ url, t, QStringLiteral("video"), QString(), resumeKey });
}

void MainWindow::openAudioStream(const QString& url, const QString& resumeKey, const QString& title,
                                 const QString& thumbnailUrl)
{
    PerfTrace::begin(QStringLiteral("open.audio"));
    if (splitTarget_) { splitTarget_->openVideo(url, title); finishSplitOpen(); return; }
    notePlaybackStart();    // channel guard: keep the channel iff this is its own audio-stream pick
    subCtx_ = {};           // audio has no subtitles to fetch
    stopScrobble();         // leaving whatever video was playing
    retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
    session_->clearQueue();      // saves+clears any previous timed media, then we build a one-track queue
    const QString t = !title.isEmpty() ? title : QUrl(url).fileName();
    const QString rkey = resumeKey.isEmpty() ? url : resumeKey;
    // Themed mode: this streamed audio shows the QML now-playing page (the catalog thumbnail is its cover art).
    themedAudioSession_ = themedHomeEnabled();
    themedAudioData_ = makeThemedAudioData(t, QString(), thumbnailUrl);
    session_->setMediaVideo(false); // consumption-stats: streamed audio/audiobook accrues "listen" seconds
    // The now-playing list (vs. the bare video surface) marks this as audio. resumeKey re-keys the track to the
    // stable id atomically (a long audiobook must resume where you left off even as its debrid URL changes).
    session_->setQueue({ url }, 0, { t }, rkey);
    syncKey_ = rkey;             // AFTER setQueue: the playRequested choke point just set syncKey_ to the volatile
                                 // url; override the initial track with the stable id (audio uses the same
                                 // MpvWidget — sub offset harmless). fileLoaded fires async, so this wins the apply.
    RecentStore::add({ url, t, QStringLiteral("audio"), thumbnailUrl, rkey });
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

bool MainWindow::openDocumentPath(const QString& f)
{
    PerfTrace::begin(QStringLiteral("open.reader"));
    const QString ext = QFileInfo(f).suffix().toLower();
    QString err;

    if (splitTarget_)
    {
        if (ext == QStringLiteral("pdf")) splitTarget_->openPdf(f);
        else if (ext == QStringLiteral("cbz")) splitTarget_->openComic(f);
        else splitTarget_->openBook(f); // .epub
        PerfTrace::end(QStringLiteral("open.reader"), ext);
        finishSplitOpen();
        return true; // routed into the split target (its own error surfacing handles a bad file)
    }

    if (ext == QStringLiteral("pdf"))
    {
        if (!pdf_->openPdf(f, &err)) { notify(tr("Can't open PDF: %1").arg(err), kFeedbackLong); return false; }
        player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); session_->clearQueue();
        presentPdf();
        PerfTrace::end(QStringLiteral("open.reader"), ext);
    }
    else if (ext == QStringLiteral("cbz"))
    {
        if (!comic_->openComic(f, &err)) { notify(tr("Can't open comic: %1").arg(err), kFeedbackLong); return false; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); session_->clearQueue();
        presentComic();
        PerfTrace::end(QStringLiteral("open.reader"), ext);
    }
    else // treat everything else as an EPUB (the reader validates and reports if it isn't one)
    {
        if (!book_->openBook(f, &err)) { notify(tr("Can't open book: %1").arg(err), kFeedbackLong); return false; }
        player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); session_->clearQueue();
        presentBook();
        PerfTrace::end(QStringLiteral("open.reader"), ext);
    }
    RecentStore::add({ f, QFileInfo(f).completeBaseName(), QStringLiteral("document"), QString() });
    return true;
}

void MainWindow::openLibrary()
{
#ifdef MMV_HAVE_QML
    // Themed mode (B2 Task 6.5): the Add-ons manager's SOURCE MANAGEMENT surface on the Nav Contract. Root rows =
    // Browse/Install-from-file/Add-by-URL/Reload actions + a Separator + one Action per installed source (drill
    // into presentAddonDetail). Catalog browsing / Local ROMs stay OUT of scope (the themed home covers content).
    // Refresh is IMPERATIVE: installPackage/removeAddon/reload do NOT emit sourcesChanged (only remote add/remove
    // + background updates do), so each mutating op re-presents the root (this is a hub child — panelTitle gates
    // present vs replaceTop). The classic LibraryView below is UNTOUCHED (classic mode / no host).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // hub child present replaces the pool (lifetime model at openCloudSync's connect block)
        themedPanelHost_->setStyle(settingsPanelStyle());

        QVector<PanelRow> rows;
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        action(QStringLiteral("lib.browse"),  tr("Browse Add-ons…"));
        action(QStringLiteral("lib.install"), tr("Install from file…"));
        action(QStringLiteral("lib.addurl"),  tr("Add by URL…"));
        action(QStringLiteral("lib.reload"),  tr("Reload"));
        { PanelRow r; r.kind = PanelRow::Separator; r.id = QStringLiteral("lib.sep"); r.label = tr("Sources"); rows << r; }

        const QVector<LoadedAddon*>& srcs = addons_->sources();
        if (srcs.isEmpty())
        {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("lib.none");
            r.label = tr("No add-on sources installed yet."); rows << r;
        }
        else for (LoadedAddon* s : srcs)
        {
            QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
            if (s->transport == LoadedAddon::RemoteHttp) name += tr("  (remote)"); // distinguish URL-based sources
            PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("lib.src:") + s->manifest.id;
            r.label = name;
            r.value = addons_->isEnabled(s->manifest.id) ? QString() : tr("disabled"); // enabled-state annotation
            rows << r;
        }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("lib.status"); rows << r; } // add-by-URL / op results

        auto onAct = [this](const QString& id, const QString&) {
            if (id == QStringLiteral("lib.browse")) presentAddonRegistry();
            else if (id == QStringLiteral("lib.install"))
            {
                // Native file picker is the documented exception (like Emulator Manager's Change folder…).
                const QString f = QFileDialog::getOpenFileName(this, tr("Install add-on"), QString(),
                    tr("Addon packages (*.addon *.zip);;All files (*.*)"));
                if (f.isEmpty()) return;
                QString err;
                if (!addons_->installPackage(f, &err)) { setAddonsStatus(tr("Install failed: %1").arg(err)); return; }
                openLibrary();                       // root is top -> replaceTop with the new source
                setAddonsStatus(tr("Add-on installed."));
            }
            else if (id == QStringLiteral("lib.addurl")) presentAddByUrl();
            else if (id == QStringLiteral("lib.reload"))
            {
                addons_->reload();
                openLibrary();                       // rebuild the source list from the re-scanned root
                setAddonsStatus(tr("Reloaded."));
            }
            else if (id.startsWith(QStringLiteral("lib.src:")))
                presentAddonDetail(id.mid(id.indexOf(QLatin1Char(':')) + 1));
        };
        auto onBack = [this] { openSettingsHub(); };   // Add-ons is a hub child

        if (themedPanelHost_->panelTitle() == tr("Add-ons"))
            themedPanelHost_->replaceTop(tr("Add-ons"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("Add-ons"), rows, onAct, onBack);

        // Remote source changes (a successful add-by-URL/registry-remote install, a background manifest refresh /
        // self-update) that land while THIS root is the live top rebuild the list. When a child add-flow panel is
        // top instead, this drops (its own top-gated handler owns the row/status); the child returns to a freshly
        // rebuilt root on success. Armed in panelPageConns_ per the lifetime model (top-gated, replaced on re-present).
        panelPageConns_ << connect(addons_.get(), &AddonManager::sourcesChanged, this, [this] {
            if (themedPanelIsTop(tr("Add-ons"))) openLibrary(); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    // Classic fallback: the Add-ons manager is the classic LibraryView QWidget. This log marks that the classic
    // surface was ACTUALLY shown (the themed branch above returns first, so themed mode never logs it — the Task 7
    // walk greps a themed run to prove zero classic surfaces). Classic mode reaches here and logs, as expected.
    mwLog(QStringLiteral("deprecated-classic: addons"));
    library_->refreshSources();
    stack_->setCurrentWidget(library_);
}

#ifdef MMV_HAVE_QML
// Patch the root "Add-ons" panel's status Info row in place (op results / add-by-URL outcomes). No-ops harmlessly
// (updateRow returns false) when the root isn't the model's top panel.
void MainWindow::setAddonsStatus(const QString& msg) { updatePanelInfo(QStringLiteral("lib.status"), msg); }

// Patch ANY Info row's value in place by id (status lines on the add-by-URL / remove / registry panels). The row
// keeps its (empty) label; updateRow patches every stack entry carrying the id, so a backgrounded panel stays fresh.
void MainWindow::updatePanelInfo(const QString& id, const QString& value)
{
    PanelRow r; r.kind = PanelRow::Info; r.id = id; r.value = value;
    themedPanelHost_->updateRow(id, r);
}

// Per-addon nested panel: Toggle Enabled + Configure… (or an Info when the manifest declares no settings) + a
// destructive Remove + version/author/description Info rows. All data ops reuse AddonManager verbatim. Enable/
// disable rides mgr_->setEnabled (which emits sourceEnabledChanged → the prefetcher resweeps) and patches the
// backgrounded root's source row so its enabled-state annotation stays correct on pop-restore.
void MainWindow::presentAddonDetail(const QString& sourceId)
{
    LoadedAddon* s = addons_->sourceById(sourceId);
    if (!s) { openLibrary(); return; }   // vanished (e.g. removed underneath us) — back to the root
    const AddonManifest m = s->manifest;
    const bool remote = (s->transport == LoadedAddon::RemoteHttp);
    const QString name = m.name.isEmpty() ? m.id : m.name;

    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("ad.enabled"); r.label = tr("Enabled");
      r.checked = addons_->isEnabled(m.id); rows << r; }
    if (!m.settings.isEmpty())
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("ad.configure"); r.label = tr("Configure…"); rows << r; }
    else
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.noconfig"); r.label = tr("Configure");
      r.value = tr("No configurable settings."); rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("ad.remove");
      r.label = remote ? tr("Remove source") : tr("Remove add-on"); r.destructive = true; rows << r; }
    if (!m.version.isEmpty())
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.version"); r.label = tr("Version"); r.value = m.version; rows << r; }
    if (!m.author.isEmpty())
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.author"); r.label = tr("Author"); r.value = m.author; rows << r; }
    if (!m.description.isEmpty())
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.about"); r.label = tr("About"); r.value = m.description; rows << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ad.kind"); r.label = tr("Kind");
      r.value = remote ? tr("Remote source — %1").arg(s->baseUrl) : tr("Local add-on"); rows << r; }

    auto onAct = [this, sourceId, m, name, remote](const QString& id, const QString& val) {
        if (id == QStringLiteral("ad.enabled"))
        {
            const bool on = (val == QStringLiteral("1"));
            addons_->setEnabled(m.id, on);   // emits sourceEnabledChanged → prefetcher resweep (verified via log)
            // Keep the backgrounded root's source-row annotation correct for the pop-restore.
            QString label = m.name.isEmpty() ? m.id : m.name;
            if (remote) label += tr("  (remote)");
            PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("lib.src:") + m.id; r.label = label;
            r.value = on ? QString() : tr("disabled");
            themedPanelHost_->updateRow(r.id, r);
        }
        else if (id == QStringLiteral("ad.configure")) presentAddonConfig(m);
        else if (id == QStringLiteral("ad.remove"))    confirmRemoveAddon(sourceId);
    };
    themedPanelHost_->present(name, rows, onAct, [this] { openLibrary(); }); // defensive root onBack
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
}

// Manifest-driven config form (nested on the addon detail): one row per AddonSetting — checkbox → Toggle,
// password → masked TextField (OSK masks during editing too), number/text → TextField. Values load via
// AddonContext::readConfig; each commit writes via AddonContext::writeConfig IMMEDIATELY (the themed-panel
// convention — the addon's script picks the new value up on its next run, like classic's post-Save re-fetch). A
// footer Info row carries the plaintext-storage note (the landmine). NO devid/devpassword special-casing — the
// form is strictly manifest-driven (absent fields simply don't appear).
void MainWindow::presentAddonConfig(const AddonManifest& manifest)
{
    const QString name = manifest.name.isEmpty() ? manifest.id : manifest.name;

    QVector<PanelRow> rows;
    if (manifest.settings.isEmpty())
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cfg.none");
      r.label = tr("This add-on has no configurable settings."); rows << r; }
    else for (const AddonSetting& sset : manifest.settings)
    {
        const QString stored = AddonContext::readConfig(manifest.id, sset.key, sset.defaultValue);
        PanelRow r; r.id = QStringLiteral("cfg:") + sset.key; r.label = sset.label.isEmpty() ? sset.key : sset.label;
        if (sset.type == QStringLiteral("checkbox"))
        {
            r.kind = PanelRow::Toggle;
            r.checked = (stored.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || stored == QStringLiteral("1"));
        }
        else
        {
            r.kind = PanelRow::TextField;
            r.value = stored;
            if (sset.type == QStringLiteral("password")) r.masked = true; // dots in the row AND in the OSK editor
        }
        rows << r;
    }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cfg.note"); r.label = tr("Note");
      r.value = tr("Credentials are stored on this device (plaintext in mymediavault.ini) and are only sent "
                   "where the add-on's script chooses to use them."); rows << r; }

    themedPanelHost_->present(tr("%1 — Settings").arg(name), rows,
        [this, manifest](const QString& id, const QString& val) {
            if (!id.startsWith(QStringLiteral("cfg:"))) return;
            const QString key = id.mid(4);
            QString value = val;
            // Toggle delivers "1"/"0"; convert ONLY for a checkbox setting (a text/password field's literal
            // "0"/"1" must be stored verbatim). Look the type up from the manifest to disambiguate.
            for (const AddonSetting& sset : manifest.settings)
                if (sset.key == key && sset.type == QStringLiteral("checkbox"))
                { value = (val == QStringLiteral("1")) ? QStringLiteral("true") : QStringLiteral("false"); break; }
            AddonContext::writeConfig(manifest.id, key, value);
        },
        [] { /* nested: Back pops to the addon detail */ });
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
}

// Remove confirm: the canonical NavConfirm::ask card — Cancel FOCUSED (focusIndex 0), Back = Cancel — matching
// confirmDeleteProfile. NOT a panel whose only selectable row is the destructive Action: there the cursor
// auto-lands on Remove, so a rapid double-activate (open confirm → confirm) could remove with the card only
// flashed; classic's QDialogButtonBox never pre-focused the destructive button either. On confirm:
// removeAddon (delete files) or removeRemoteSource (drop the URL), pop the detail, rebuild the root.
void MainWindow::confirmRemoveAddon(const QString& sourceId)
{
    LoadedAddon* s = addons_->sourceById(sourceId);
    if (!s) { openLibrary(); return; }
    const QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
    const bool remote = (s->transport == LoadedAddon::RemoteHttp);
    const QString addonId = s->manifest.id;
    const QString baseUrl = s->baseUrl;

    const int choice = NavConfirm::ask(tr("Remove add-on"),
        remote ? tr("Remove the remote source \"%1\"? Only the saved URL is removed.").arg(name)
               : tr("Remove \"%1\" and delete its files?").arg(name),
        { tr("Cancel"), tr("Remove") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (choice != 1) return;                                     // cancelled / backed out — nothing touched

    const bool ok = remote ? addons_->removeRemoteSource(baseUrl) : addons_->removeAddon(addonId);
    if (!ok) { notify(tr("Couldn't remove \"%1\".").arg(name)); return; }
    // Removed: pop the detail (deferred so the activation unwinds first), landing on a rebuilt root.
    // removeRemoteSource emits sourcesChanged synchronously, but the detail is top at that instant so the
    // root's top-gated handler drops it — this deferred rebuild is the refresh.
    QTimer::singleShot(0, this, [this, name] {
        themedPanelHost_->handleBack();   // pop the detail -> the root ("Add-ons")
        openLibrary();                    // root now top -> replaceTop with the source gone
        setAddonsStatus(tr("Removed \"%1\".").arg(name));
    });
}

// Add-by-URL nested panel: a URL TextField (via the OSK) + an Add Action → mgr_->addRemoteSource (ASYNC). The
// result arrives on AddonManager::remoteSourceResult; a top-gated handler (armed here in panelPageConns_) shows
// it on the status Info row — an invalid URL surfaces the error with no wedge. On success it pops back to a
// freshly rebuilt root so the new source appears.
void MainWindow::presentAddByUrl()
{
    auto pending = std::make_shared<QString>();

    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("au.hint"); r.label = tr("Add-on URL");
      r.value = tr("Its manifest.json or base URL"); rows << r; }
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("au.url"); r.label = tr("URL");
      r.value = *pending; rows << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("au.status"); rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("au.add"); r.label = tr("Add"); rows << r; }

    themedPanelHost_->present(tr("Add by URL"), rows,
        [this, pending](const QString& id, const QString& val) {
            if (id == QStringLiteral("au.url")) *pending = val;
            else if (id == QStringLiteral("au.add"))
            {
                const QString url = pending->trimmed();
                if (url.isEmpty()) { updatePanelInfo(QStringLiteral("au.status"), tr("Enter an add-on URL.")); return; }
                updatePanelInfo(QStringLiteral("au.status"), tr("Fetching add-on…"));
                addons_->addRemoteSource(url);   // async → remoteSourceResult (handled below)
            }
        },
        [] { /* nested: Back pops to the root */ });

    // Result handler, top-gated on THIS panel (per the lifetime model). Failure surfaces on the status row and
    // stays put (no wedge); success shows the message then returns to a rebuilt root so the new source is visible.
    panelPageConns_ << connect(addons_.get(), &AddonManager::remoteSourceResult, this,
        [this](bool ok, const QString& msg) {
            if (!themedPanelIsTop(tr("Add by URL"))) return;   // a different add-flow (registry) is top — not ours
            updatePanelInfo(QStringLiteral("au.status"), msg);
            if (ok) QTimer::singleShot(700, this, [this] {
                if (!themedPanelIsTop(tr("Add by URL"))) return;
                themedPanelHost_->handleBack();   // pop to the root
                openLibrary();                    // rebuild with the new source (sourcesChanged also fired, but
            });                                   // the root wasn't top then, so this is the refresh)
        });

    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
}

namespace {
// The built-in add-on registry (the always-present cubman3134 store index).
QString addonsRegistryDefaultUrl()
{ return QStringLiteral("https://raw.githubusercontent.com/cubman3134/mymediavault-addons/main/index.json"); }
// The directory an index URL lives in (its files are resolved relative to this).
QString registryBaseUrl(const QString& indexUrl)
{ const int slash = indexUrl.lastIndexOf(QLatin1Char('/')); return slash > 0 ? indexUrl.left(slash) : indexUrl; }
// A registry entry with a "url" is a remote (HTTP) add-on: installing it just subscribes to the URL.
bool registryEntryIsRemote(const QJsonObject& e) { return !e.value(QStringLiteral("url")).toString().isEmpty(); }
QString registryNormalizeUrl(QString u)
{
    u = u.trimmed();
    if (u.endsWith(QStringLiteral("/manifest.json"))) u.chop(int(qstrlen("/manifest.json")));
    while (u.endsWith(QLatin1Char('/'))) u.chop(1);
    return u;
}
// Blocking file download (20 s cap) — classic RegistryBrowser::downloadTo parity for packaged entries.
bool registryDownloadTo(QNetworkAccessManager* nam, const QString& url, const QString& destPath, QString* error)
{
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);
    QEventLoop loop;
    QTimer to; to.setSingleShot(true);
    QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    to.start(20000);
    loop.exec();
    if (!reply->isFinished() || reply->error() != QNetworkReply::NoError)
    {
        if (error) *error = reply->isFinished() ? reply->errorString() : QStringLiteral("timed out");
        reply->abort(); reply->deleteLater();
        return false;
    }
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    QFileInfo fi(destPath);
    QDir().mkpath(fi.absolutePath());
    QFile f(destPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = QStringLiteral("can't write file"); return false; }
    f.write(data);
    return true;
}
} // namespace

// The add-on registry "store" as a nested panel: fetch the configured registry index(es) (the built-in
// cubman3134 add-on index + saved extras), render one Action row per entry ("Install" / "Installed ✓" disabled).
// A remote entry subscribes via addRemoteSource (async → the top-gated remoteSourceResult rebuilds the rows); a
// packaged entry downloads its files (blocking, classic parity) then reload()s. Reachable only when a registry is
// reachable — an unreachable/empty fetch degrades to a graceful "unreachable" Info row (the brief's failure case).
void MainWindow::presentAddonRegistry()
{
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    registryInstallRowId_.clear();

    // Present a loading placeholder immediately; the entries replace it once every index has been fetched.
    QVector<PanelRow> loading;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("reg.status"); r.label = tr("Registry");
      r.value = tr("Loading…"); loading << r; }
    themedPanelHost_->present(tr("Browse Add-ons"), loading, [](const QString&, const QString&) {}, [] {});
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();

    // Configured registries: the built-in add-on index + any user-saved extras (kept in the ini by the classic
    // browser). We render entries from them but omit the add/remove-registry management UI (source management only).
    QSettings iniStore(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    QStringList regs; regs << addonsRegistryDefaultUrl();
    for (const QString& u : iniStore.value(QStringLiteral("registry/addonsExtras")).toStringList())
        if (!u.trimmed().isEmpty() && !regs.contains(u.trimmed())) regs << u.trimmed();

    struct RegFetch { int pending = 0; QVector<QPair<QJsonObject, QString>> entries; };
    auto st = std::make_shared<RegFetch>();
    st->pending = regs.size();

    auto finish = [this, st] {
        if (!themedPanelIsTop(tr("Browse Add-ons"))) return;   // navigated away while fetching — drop
        QVector<PanelRow> rows;
        if (st->entries.isEmpty())
        {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("reg.status"); r.label = tr("Registry");
            r.value = tr("No add-ons found — the registry may be unreachable."); rows << r;
        }
        else
        {
            { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("reg.status"); r.label = tr("Registry");
              r.value = tr("%n add-on(s) available.", "", int(st->entries.size())); rows << r; }
            for (int i = 0; i < st->entries.size(); ++i)
            {
                const QJsonObject& e = st->entries[i].first;
                const QString name = e.value(QStringLiteral("name")).toString();
                const QString author = e.value(QStringLiteral("author")).toString();
                bool installed = false;
                if (registryEntryIsRemote(e))
                    installed = addons_->remoteSourceUrls().contains(registryNormalizeUrl(e.value(QStringLiteral("url")).toString()));
                else
                {
                    const QString eid = e.value(QStringLiteral("id")).toString();
                    installed = !eid.isEmpty() && QFile::exists(addons_->addonsRoot() + QStringLiteral("/") + eid + QStringLiteral("/manifest.json"));
                }
                PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("reg:") + QString::number(i);
                r.label = name.isEmpty() ? e.value(QStringLiteral("id")).toString() : name;
                r.value = installed ? tr("Installed ✓") : (author.isEmpty() ? QString() : tr("by %1").arg(author));
                r.enabled = !installed;
                rows << r;
            }
        }
        themedPanelHost_->replaceTop(tr("Browse Add-ons"), rows,
            [this, st](const QString& id, const QString&) {
                if (!id.startsWith(QStringLiteral("reg:"))) return;
                const int i = id.mid(4).toInt();
                if (i < 0 || i >= st->entries.size()) return;
                installRegistryEntry(st->entries[i].first, st->entries[i].second, id);
            },
            [] {});
    };

    // Guard against a hung registry: render whatever arrived after 15 s so "Loading…" never sticks.
    QTimer::singleShot(15000, this, [st, finish] { if (st->pending > 0) { st->pending = 0; finish(); } });

    for (const QString& indexUrl : regs)
    {
        QNetworkRequest req((QUrl(indexUrl)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = docNam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, indexUrl, st, finish] {
            reply->deleteLater();
            if (st->pending <= 0) return;   // already finished (timeout) — ignore a late arrival
            if (reply->error() == QNetworkReply::NoError)
            {
                const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
                for (const QJsonValue& v : root.value(QStringLiteral("addons")).toArray())
                    if (v.isObject()) st->entries << qMakePair(v.toObject(), indexUrl);
            }
            if (--st->pending <= 0) finish();
        });
    }

    // A remote entry's addRemoteSource result (kicked from installRegistryEntry) rebuilds the rows so the installed
    // entry flips to "Installed ✓"; the status line carries the message. Top-gated on THIS panel (lifetime model).
    panelPageConns_ << connect(addons_.get(), &AddonManager::remoteSourceResult, this,
        [this, finish](bool, const QString& msg) {
            if (!themedPanelIsTop(tr("Browse Add-ons")) || registryInstallRowId_.isEmpty()) return;
            registryInstallRowId_.clear();
            finish();                                                 // rebuild (installed entry now shows ✓)
            updatePanelInfo(QStringLiteral("reg.status"), msg);
        });
}

// Install one registry entry: remote → addRemoteSource (async, the panel's remoteSourceResult handler rebuilds);
// packaged → blocking download of its files into addons/<id>/ then reload(). The row shows "Installing…" while it runs.
void MainWindow::installRegistryEntry(const QJsonObject& entry, const QString& indexUrl, const QString& rowId)
{
    const QString name = entry.value(QStringLiteral("name")).toString();
    { PanelRow r; r.kind = PanelRow::Action; r.id = rowId; r.label = name; r.value = tr("Installing…");
      r.enabled = false; themedPanelHost_->updateRow(rowId, r); }

    if (registryEntryIsRemote(entry))
    {
        registryInstallRowId_ = rowId;                              // the remoteSourceResult handler patches this row
        addons_->addRemoteSource(entry.value(QStringLiteral("url")).toString());
        return;
    }

    const QString id = entry.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) { updatePanelInfo(QStringLiteral("reg.status"), tr("Registry entry has no id.")); return; }
    const QString destDir = addons_->addonsRoot() + QStringLiteral("/") + id;
    const QString base = registryBaseUrl(indexUrl);
    QStringList files;
    for (const QJsonValue& fv : entry.value(QStringLiteral("files")).toArray()) files << fv.toString();
    if (files.isEmpty()) { updatePanelInfo(QStringLiteral("reg.status"), tr("Nothing to download for this entry.")); return; }

    for (const QString& rel : files)
    {
        if (rel.isEmpty()) continue;
        QString err;
        if (!registryDownloadTo(docNam_, base + QStringLiteral("/") + rel,
                                destDir + QStringLiteral("/") + QFileInfo(rel).fileName(), &err))
        {
            updatePanelInfo(QStringLiteral("reg.status"), tr("Download failed: %1").arg(err));
            PanelRow r; r.kind = PanelRow::Action; r.id = rowId; r.label = name; r.value = tr("Retry");
            r.enabled = true; themedPanelHost_->updateRow(rowId, r);
            return;
        }
    }
    addons_->reload();                                              // pick up the new add-on folder
    { PanelRow r; r.kind = PanelRow::Action; r.id = rowId; r.label = name; r.value = tr("Installed ✓");
      r.enabled = false; themedPanelHost_->updateRow(rowId, r); }
    updatePanelInfo(QStringLiteral("reg.status"), tr("Installed \"%1\".").arg(name));
}
#endif // MMV_HAVE_QML

void MainWindow::openHome()
{
    playRouteOverride_ = PlayRoute::Default; // defensive: a one-off armed but abandoned can't leak past home
    exitChannel();      // returning Home is a hard channel stop — the shared choke for every stop/Back path that
                        // lands on Home (goBack's player branch, the player stop buttons, waitPageDone, …)
    // Leaving whatever was open: stop playback/emulation, save reader positions.
    hideSubtitleMenu(); // dismiss the subtitle overlay if it was up
    stopScrobble();     // Trakt: close out the current watch
    player_->stop();
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    session_->clearQueue();
    // Restore the full-screen state we had before opening content: a full-screen browser stays full screen
    // after exiting the emulator/movie; one that went full screen only for a movie drops back to a window.
    // ONLY when actually returning FROM content — a menu-to-menu hop (Profiles/Settings -> Home) must never
    // touch the window state (the stale flag used to drop a full-screen browser to a window).
    if (inContent_)
    {
        if (fsBeforeContent_ && !isFullScreen()) showFullScreen();
        else if (!fsBeforeContent_ && isFullScreen()) leaveFullScreen();
    }
    home_->refresh();
    showHomeScreen(); // classic HomeView, or the themed home if the user enabled it
}

// Route the Home screen to the themed (QML) home or the classic HomeView per the user's setting. When the
// setting is off (default) this is exactly the old behaviour: show home_.
void MainWindow::showHomeScreen()
{
#ifdef MMV_HAVE_QML
    // Keep the in-window overlays (Esc pause menu, action choosers, confirms) matched to the active theme: push
    // the current settingsPanel block on every return home (theme changes rebuild the home, so this re-runs after
    // an Appearance switch). Classic mode -> empty map -> the overlays keep their original hardcoded darks.
    NavOverlay::setThemeColors(themedHomeEnabled() ? settingsPanelStyle() : QVariantMap());

    // Rebuild a fresh themed view on return so it reflects the current theme/catalogs. The themed pages are
    // QQuickWidgets (plain widgets, no native child window), so this is safe: no compositing tricks needed.
    if (themedHomeEnabled())
    {
        if (ThemeEngine::hasInstalledTheme()) { showThemedHome(); return; }
        // Themed home is on but no theme exists on disk (wiped/broken themes2): building the empty theme
        // renders an all-black home whose navigation still works invisibly. Fall back to the classic home
        // (setting untouched — restoring the themes folder restores the themed home) and say why, once.
        if (!warnedNoThemes_)
        {
            warnedNoThemes_ = true;
            notifier_->notify(tr("No themes found in %1 — using the classic home screen.")
                                  .arg(QDir::toNativeSeparators(ThemeEngine::themesRoot())), kFeedbackLong);
        }
        // Deprecation signal (B2 Task 6, item 2): themed home is on but we fell through to the classic HomeView
        // (no themes on disk). The Task 7 walk greps this to prove no classic home in a normal themed run.
        mwLog(QStringLiteral("deprecated-classic: home"));
    }
#endif
    stack_->setCurrentWidget(home_);
}

#ifdef MMV_HAVE_QML
// After (re)building the themed home, schedule a repaint. The themed page is a plain QQuickWidget, so this is
// just a widget update — kept as a hook point (and for the windowed first-show).
void MainWindow::nudgeThemedHome()
{
    themedHomeShownOnce_ = true;
    if (themedHome_) themedHome_->update();
}
#else
void MainWindow::nudgeThemedHome() {}
#endif

bool MainWindow::themedHomeEnabled() const
{
#ifdef MMV_HAVE_QML
    // Default ON (B2 Task 6, item 3). The default only applies when the key is ABSENT: QSettings returns the
    // stored value whenever the key exists, so a user who explicitly chose classic (wrote `false` via the
    // Appearance toggle) is respected — only fresh installs and users who never touched the toggle now get the
    // themed home. Semantics: absent -> true (themed wins), stored false -> false (classic), stored true -> true.
    return store().value(QStringLiteral("themedHome/enabled"), true).toBool();
#else
    return false;
#endif
}

#ifdef MMV_HAVE_QML
// Text prompt for a themed-mode search: the in-window on-screen keyboard, so it's typeable from the couch
// and never spawns a separate window. Null return = cancelled; "" = user cleared the box (full list).
QString MainWindow::promptThemedSearch(const QString& scope)
{
    const QString label = scope.isEmpty() ? tr("Search") : tr("Search %1").arg(scope);
    // Over a themed screen: mirror the OSK as a level on that screen's back stack, so Back inside the OSK closes
    // the OSK only and its close revives the themed scene focus (the second-search-focus / OSK-close-focus fix).
    const QString q = Osk::getText(label, QString(), QLineEdit::Normal, this, currentThemedGraph());
    return q.isNull() ? QString() : q.trimmed();
}

// Build + show the themed "system view": the media-type catalogs as a themed carousel/grid, plus an
// Appearance tile. Activating a catalog enters the classic browser for it; Appearance / Esc opens settings;
// T cycles themes. Rebuilt each time so it reflects the current catalogs + theme.
void MainWindow::showThemedHome()
{
    // An XMB theme drives the home as a two-axis cross (categories + live column) instead of a carousel/grid.
    {
        const QStringList themes = ThemeEngine::availableThemes();
        QString tn = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
        if (!themes.contains(tn)) tn = themes.value(0, QStringLiteral("Default"));
        themedHomeBuiltTheme_ = tn; // remember what we built, so showHomeScreen can reuse vs. rebuild
        if (ThemeEngine::homeIsXmb(ThemeEngine::themesRoot() + QStringLiteral("/") + tn)) { showThemedXmb(); return; }
    }
    themedHomeIsXmb_ = false;

    const QStringList themes = ThemeEngine::availableThemes();
    QString themeName = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
    if (!themes.contains(themeName)) themeName = themes.value(0, QStringLiteral("Default"));
    const QString themeDir = ThemeEngine::themesRoot() + QStringLiteral("/") + themeName;

    QVariantList items = home_->systemItems();
    QStringList navKeys;
    for (const QVariant& v : items) navKeys << v.toMap().value(QStringLiteral("navKey")).toString();
    // The Appearance tile is a catalog-grid stand-in for the theme picker. A theme with its own settings /
    // appearance button (e.g. Channels) sets "hideAppearanceTile": true so it isn't shown twice.
    int appearanceIdx = -1;
    if (!ThemeEngine::homeHidesAppearanceTile(themeDir))
    {
        items << QVariantMap{ { QStringLiteral("title"), tr("Appearance") },
                              { QStringLiteral("subtitle"), tr("Themes & layout") },
                              { QStringLiteral("accent"), QStringLiteral("#5B6470") } };
        navKeys << QString();
        appearanceIdx = int(items.size()) - 1;
    }

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("My Media Vault"));
    auto onActivated = [this, navKeys, appearanceIdx](int idx) {
        if (idx == appearanceIdx) { openAppearance(); return; }
        if (idx >= 0 && idx < navKeys.size() && !navKeys[idx].isEmpty())
        { themedHomeIndex_ = idx; home_->activateNav(navKeys[idx]); showThemedBrowse(); } // remember + open catalog
    };
    // Esc at the themed root: the theme is the BOTTOM of the stack — there is nothing to go "back" to, so
    // bring up the app pause menu (Resume / Exit), exactly like the XMB home. (This used to jump to the
    // Appearance settings as "a reliable way back out", which read as Esc randomly opening settings.
    // Appearance stays reachable via Settings ▸ Appearance and a theme's own settings/appearance buttons.)
    // rootBack for the grid home: the theme is the BOTTOM of the stack (a flat grid — no drill levels), so
    // nav.back()'s empty-stack rootBack lands here and brings up the app pause menu.
    auto onBack  = [this] { showEscMenu(); };
    auto onCycle = [this, themes, themeName] {
        if (themes.isEmpty()) return;
        const QString next = themes[(qMax(0, int(themes.indexOf(themeName))) + 1) % themes.size()];
        store().setValue(QStringLiteral("themedHome/theme"), next); store().sync();
        showThemedHome();
    };
    // "/" on the highlighted catalog: prompt for a query, open that catalog and search within it.
    auto onSearch = [this, navKeys, items, appearanceIdx] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int idx = r ? r->property("currentIndex").toInt() : -1;
        if (idx < 0 || idx == appearanceIdx || idx >= navKeys.size() || navKeys[idx].isEmpty()) return;
        const QString name = items.value(idx).toMap().value(QStringLiteral("title")).toString();
        const QString q = promptThemedSearch(name);
        if (q.isNull()) return; // cancelled
        home_->activateNav(navKeys[idx]);
        home_->searchInBrowse(q);
        showThemedBrowse();
    };

    // A theme may place `button` elements (e.g. the Channels theme's Wii-style corner buttons): route their
    // named actions to the real screens. Unknown actions are ignored.
    auto onButton = [this](const QString& a) {
        if (a == QStringLiteral("settings"))     openSettingsHub();
        else if (a == QStringLiteral("profile")) onSwitchProfile();
        else if (a == QStringLiteral("appearance")) openAppearance();
    };

    QWidget* w = ThemeEngine::buildView(themeDir,
                                        items, system, this, onActivated, onBack, onCycle, onSearch,
                                        {}, {}, {}, {}, {}, onButton,
                                        [this] { openThemedDetail(-1); },
                                        [this](const QString& v) { runThemedDetailAction(v); },
                                        [this](const QString& v) { runThemedAudioTransport(v); },
                                        [this](int row) { if (session_) session_->playIndex(row); });
    // Re-highlight the system we last opened (so returning from a catalog lands back on it, not the top).
    if (QQuickItem* r = ThemeEngine::rootItem(w))
        r->setProperty("currentIndex", qBound(0, themedHomeIndex_, int(items.size()) - 1));
    QWidget* old = themedHome_;
    themedHome_ = w;
    applyThemeMusic(themeDir); // ship this theme's default menu music (used when the user has no music folder)
    updateThemedNowPlaying(); // seed the Triple theme's now-playing readout
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    w->setFocus();
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
    nudgeThemedHome(); // repaint the rebuilt themed home

    // Hot-reload: rebuild the themed home whenever its theme.json is edited (while it's the visible screen).
    if (!themeWatcher_)
    {
        themeWatcher_ = new QFileSystemWatcher(this);
        connect(themeWatcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
            if (themedHomeEnabled() && stack_->currentWidget() == themedHome_)
                QTimer::singleShot(150, this, [this] { // settle after the editor finishes writing
                    if (themedHomeEnabled() && stack_->currentWidget() == themedHome_) showThemedHome();
                });
        });
    }
    themeWatcher_->removePaths(themeWatcher_->files());
    const QString themeFile = ThemeEngine::themesRoot() + QStringLiteral("/") + themeName + QStringLiteral("/theme.json");
    if (QFile::exists(themeFile)) themeWatcher_->addPath(themeFile);
}

// Themed PS3-style XMB home, two-step: the horizontal axis is the four inherent categories (Video / Game /
// Audio / Reading, + a synthetic Settings); the vertical column shows that category's catalogs, and Enter on a
// catalog drills the column into its live items (Esc returns to the catalog list). Moving across categories
// resets the column to that bucket's catalog list. Everything stays on the one cross screen.
// Set the XMB metadata panel's instant skeleton for the browse-item at `idx` (the title/poster/subtitle we
// already hold), then queue the debounced addon fetch that enriches it with the synopsis + facts. No-op when
// not on the XMB cross inside a catalog.
void MainWindow::refreshThemedMeta(int idx)
{
    QQuickItem* r = ThemeEngine::rootItem(themedHome_);
    if (!r || !themedHomeIsXmb_ || !themedXmbInCatalog_) return;
    const QVariantList col = r->property("items").toList();
    if (idx < 0 || idx >= col.size()) { r->setProperty("selectedMeta", QVariantMap()); return; }
    const QVariantMap it = col[idx].toMap();
    // Synthetic rows (the Playlists folder, a playlist, the New entry) aren't media - show no metadata panel.
    if (it.value(QStringLiteral("type")).toString().startsWith(QLatin1Char('_')))
        { r->setProperty("selectedMeta", QVariantMap()); themedMetaWant_ = -1; return; }
    QVariantMap sk; // overwrites the previous row's data (so no stale synopsis lingers while the fetch runs)
    sk.insert(QStringLiteral("title"), it.value(QStringLiteral("title")));
    sk.insert(QStringLiteral("subtitle"), it.value(QStringLiteral("subtitle")));
    sk.insert(QStringLiteral("image"), it.value(QStringLiteral("image")));
    sk.insert(QStringLiteral("type"), it.value(QStringLiteral("type")));
    sk.insert(QStringLiteral("favorite"), home_->isThemedLeafFavorite(idx));
    r->setProperty("selectedMeta", sk);
    themedMetaWant_ = idx;
    // Resolve the LOCAL data (session cache / gamelist.xml / MetaCache) right now — it's in-memory/fast, so
    // the logo + facts + video show instantly with no plain-title flash. Only the networked enrichment
    // (online scrape + achievements) is debounced below, so fast scrolling doesn't hit the network per row.
    home_->requestThemedMeta(idx);
    if (themedMetaTimer_) themedMetaTimer_->start();
}

// Open the themed DETAIL view (on the Nav Contract) for the current selection — the replacement for the
// retired classic info page. Populates the detail data FIRST (so the page never flashes empty), switches the
// view, then pushes a "detail" nav level whose onPop restores the previous view: the Back router owns the exit.
// A no-op off a themed surface, on a theme with no `detail` view, or on a non-media row (returns empty data).
void MainWindow::openThemedDetail(int browseIndex)
{
    QWidget* cur = stack_->currentWidget();
    if (cur != themedHome_ && cur != themedBrowse_) return;
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r) return;
    // The theme must define a `detail` view ("I" is simply inert on a theme without one).
    if (!r->property("theme").toMap().value(QStringLiteral("views")).toMap().contains(QStringLiteral("detail")))
        return;
    // On the XMB home the column is only browse rows while drilled INTO a catalog; on the catalog list /
    // Profiles / Settings columns currentIndex indexes a different list entirely (browseRowMap_ would be
    // stale), so "I" is inert there.
    if (cur == themedHome_ && themedHomeIsXmb_ && !themedXmbInCatalog_) return;
    const int bi = (browseIndex >= 0) ? browseIndex : r->property("currentIndex").toInt();
    const QVariantMap data = home_->themedDetailData(bi);
    if (data.isEmpty()) return; // a divider / synthetic / non-media row: nothing to detail

    themedDetailIndex_ = bi;
    themedDetailKey_ = home_->themedLeafKey(bi); // marks key: hide/status/tags act on this, not the volatile index
    themedDetailMarksDirty_ = false;             // set true by a Hide/Show in this detail -> rebuild rows on pop
    r->setProperty("detailData", data);
    r->setProperty("detailActionIndex", 0);
    r->setProperty("detailChildIndex", 0);
    r->setProperty("detailZone", QStringLiteral("actions"));
    const QString ret = r->property("currentView").toString();
    r->setProperty("currentView", QStringLiteral("detail")); // -> ThemeBridge::syncDetailZone counts the detail zones up
    if (NavGraph* g = ThemeEngine::navGraph(cur))
        g->pushLevel(QStringLiteral("detail"), [this, cur, ret] {
            themedDetailIndex_ = -1;
            themedDetailKey_.clear();
            QQuickItem* rr = ThemeEngine::rootItem(cur);
            if (!rr) { themedDetailMarksDirty_ = false; return; }
            rr->setProperty("currentView", ret); // -> detail zones hidden
            // A Hide/Show toggle happened while this detail was open: rebuild the browse model NOW (browseItems
            // re-applies the hidden filter) so returning to the list shows the row gone / back — no re-fetch. The
            // nav graph's set-index clamp absorbs a removed row; we keep the cursor near where it was.
            if (themedDetailMarksDirty_)
            {
                themedDetailMarksDirty_ = false;
                const int keep = rr->property("currentIndex").toInt();
                rr->setProperty("items", home_->browseItems());
                rr->setProperty("currentIndex", keep); // clamped by the QML model if it now overshoots
            }
        });
}

// The themed grid browse opens an info-page leaf (movie/series/book/comic/…) in the detail view instead of the
// old classic-page drill. Returns true when it did; false for a container (drill) or a direct-open leaf
// (game/track), or when the theme has no detail view — the caller then falls back to browseActivate().
bool MainWindow::openThemedDetailForInfoLeaf(int browseIndex)
{
    QWidget* cur = stack_->currentWidget();
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r) return false;
    if (!r->property("theme").toMap().value(QStringLiteral("views")).toMap().contains(QStringLiteral("detail")))
        return false;
    if (!home_->isThemedInfoLeaf(browseIndex)) return false;
    openThemedDetail(browseIndex);
    return true;
}

// Run an action-row verb on the item the detail view is showing — reusing the SAME HomeView methods the XMB
// inline chooser uses, so play/download/favourite/playlist behave identically. Favourite stays on the page and
// nudges the row's heart; the others (play launches, playlist opens the NavMenu, download queues) act in place.
void MainWindow::runThemedDetailAction(const QString& verb)
{
    const int idx = themedDetailIndex_;
    if (idx < 0) return;
    if (verb == QStringLiteral("play"))          home_->playThemedLeaf(idx);
    else if (verb == QStringLiteral("download")) home_->downloadThemedLeaf(idx);
    else if (verb == QStringLiteral("playlist")) home_->addBrowseItemToPlaylist(idx);
    // One-off external/built-in override for THIS play. Two leak-free channels (see routePlay): the member
    // carries a SYNCHRONOUS local/recents leaf (consumed inside playThemedLeaf, then cleared right after so it
    // can't survive to a later play); the hint rides an ASYNC catalog leaf on the resolved MediaItem.
    else if (verb == QStringLiteral("external") || verb == QStringLiteral("builtin")) {
        const PlayRoute r = (verb == QStringLiteral("external")) ? PlayRoute::ForceExternal : PlayRoute::ForceBuiltin;
        playRouteOverride_ = r;                       // synchronous local/recents path reads this in routePlay
        home_->playThemedLeaf(idx, hintFromRoute(r)); // async catalog path stamps the hint on its item instead
        playRouteOverride_ = PlayRoute::Default;      // already consumed if synchronous; cleared so async can't leak
    }
    else if (verb == QStringLiteral("favorite"))
    {
        home_->favoriteThemedLeaf(idx);
        QWidget* cur = stack_->currentWidget();
        if (QQuickItem* r = ThemeEngine::rootItem(cur))
        {
            QVariantMap d = r->property("detailData").toMap();
            d.insert(QStringLiteral("favorite"), home_->isThemedLeafFavorite(idx));
            r->setProperty("detailData", d); // the action row re-reads selected.favorite -> ★/☆ flips in place
        }
    }
    // ---- Library-management verbs (act on themedDetailKey_ so a row-index shift can't misfire) ----
    else if (verb == QStringLiteral("hide"))
    {
        if (themedDetailKey_.isEmpty()) return;
        const bool nowHidden = !ItemMarks::get(themedDetailKey_).hidden;
        ItemMarks::setHidden(themedDetailKey_, nowHidden);
        themedDetailMarksDirty_ = true; // -> browse model rebuilt on detail pop (row vanishes / returns)
        // Re-push detailData so the pill flips Hide<->Unhide in place (the favourite-branch idiom); the detail
        // stays open on the item, and the underlying list refreshes when the user backs out.
        QWidget* cur = stack_->currentWidget();
        if (QQuickItem* r = ThemeEngine::rootItem(cur))
        {
            QVariantMap d = r->property("detailData").toMap();
            d.insert(QStringLiteral("hidden"), nowHidden);
            r->setProperty("detailData", d);
        }
    }
    else if (verb == QStringLiteral("status")) themedDetailPickStatus();
    else if (verb == QStringLiteral("tags"))   themedDetailEditTags();
}

// The completion-status picker: a controller-navigable NavMenu over the five states, the current one marked
// with a check. Picking one writes it (ItemMarks, per profile) and re-pushes detailData so the status pill
// updates in place. Back leaves the mark untouched.
void MainWindow::themedDetailPickStatus()
{
    // Snapshot the key into a LOCAL: NavMenu::pick is a modal nested loop, so bind the target once (read before
    // AND written after the pick) rather than re-reading the member across the modality.
    const QString key = themedDetailKey_;
    if (key.isEmpty()) return;
    struct S { ItemMarks::Completion c; QString label; };
    const QVector<S> states = {
        { ItemMarks::Completion::None,       tr("None") },
        { ItemMarks::Completion::Planned,    tr("Planned") },
        { ItemMarks::Completion::InProgress, tr("In progress") },
        { ItemMarks::Completion::Finished,   tr("Finished") },
        { ItemMarks::Completion::Abandoned,  tr("Abandoned") },
    };
    const ItemMarks::Completion cur = ItemMarks::get(key).completion;
    QStringList rows;
    for (const S& s : states)
        rows << (s.c == cur ? QStringLiteral("✓  ") + s.label : QStringLiteral("     ") + s.label);
    const int pick = NavMenu::pick(tr("Set status"), rows, this);
    if (pick < 0 || pick >= states.size()) return;
    ItemMarks::setCompletion(key, states[pick].c);
    // Re-push the completion token so the status pill relabels (ActionRow maps token -> label).
    static const char* tok[] = { "none", "planned", "inProgress", "finished", "abandoned" };
    QWidget* w = stack_->currentWidget();
    if (QQuickItem* r = ThemeEngine::rootItem(w))
    {
        QVariantMap d = r->property("detailData").toMap();
        d.insert(QStringLiteral("completion"), QString::fromLatin1(tok[pick]));
        r->setProperty("detailData", d);
    }
}

// The tags picker LOOP: one NavMenu re-presented until Back. Rows are the profile's tag vocabulary — each shows
// a ✓ when the item carries it and a 📌 when it's a pinned shelf — followed by "New tag…" and, when the loop is
// re-entered after highlighting a real tag, a "Pin/Unpin shelf: <tag>" row for that tag. Picking a vocab tag
// toggles it on the item and re-presents; "New tag…" prompts (Osk) for a name, applies it, and re-presents;
// the pin row flips that tag's shelf-pin. Back exits the loop. All writes go through ItemMarks (per profile).
void MainWindow::themedDetailEditTags()
{
    // Snapshot the detail's marks key into a LOCAL up front: every step below re-enters a modal nested loop
    // (NavMenu::pick / Osk::getText / NavConfirm::ask), so binding the target once makes it explicit that the
    // whole flow acts on the item the detail was opened for, regardless of any member churn during modality.
    const QString key = themedDetailKey_;
    if (key.isEmpty()) return;
    QString selTag; // the tag a pin row (from the previous pass) targets — the last vocab row we acted on
    while (true)
    {
        const QStringList vocab = ItemMarks::tagVocab();
        const QStringList onItem = ItemMarks::get(key).tags;
        const QStringList pinned = ItemMarks::pinnedTags();

        QStringList rows;
        for (const QString& t : vocab)
        {
            QString row = onItem.contains(t) ? QStringLiteral("✓ ") : QStringLiteral("   ");
            row += t;
            if (pinned.contains(t)) row += QStringLiteral("   📌");
            rows << row;
        }
        const int newIdx = rows.size();            // "New tag…" row
        rows << tr("➕ New tag…");
        int pinIdx = -1, delIdx = -1;
        if (!selTag.isEmpty() && vocab.contains(selTag))
        {
            pinIdx = rows.size();                  // "Pin/Unpin shelf: <selected>" row
            rows << (pinned.contains(selTag) ? tr("📌 Unpin shelf: %1").arg(selTag)
                                             : tr("📌 Pin as shelf: %1").arg(selTag));
            delIdx = rows.size();                  // "Delete tag everywhere: <selected>" (vocab + all items + unpin)
            rows << tr("🗑 Delete tag everywhere: %1").arg(selTag);
        }

        const int pick = NavMenu::pick(tr("Tags"), rows, this);
        if (pick < 0) return;                       // Back exits the loop

        if (pick == newIdx)
        {
            const QString name = Osk::getText(tr("New tag:"), QString(), QLineEdit::Normal,
                                              this, currentThemedGraph()).trimmed();
            if (!name.isEmpty())
            {
                QStringList tags = ItemMarks::get(key).tags;
                if (!tags.contains(name)) tags << name;   // apply to the item (also unions into the vocab)
                ItemMarks::setTags(key, tags);
                selTag = name;
            }
            continue;
        }
        if (pick == pinIdx)
        {
            ItemMarks::setPinned(selTag, !ItemMarks::pinnedTags().contains(selTag));
            continue;
        }
        if (pick == delIdx)
        {
            // Destructive + profile-wide: confirm before it strips the tag from EVERY item and any shelf pin
            // (the confirmDeleteProfile card shape — Cancel focused, Back = Cancel). Decline → back to the loop.
            const int c = NavConfirm::ask(tr("Delete tag"),
                tr("Delete the tag “%1” from every item in this profile and unpin its shelf? "
                   "This can't be undone.").arg(selTag),
                { tr("Cancel"), tr("Delete") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
            if (c != 1) continue;                    // Cancel / Back -> re-present the tags loop unchanged
            ItemMarks::removeTagEverywhere(selTag);  // strips it from the vocab, every item, and any shelf pin
            selTag.clear();                          // it no longer exists -> no selected-tag rows next pass
            continue;
        }
        // A vocab tag row: toggle it on the item, remember it as the pin-row target, re-present.
        if (pick >= 0 && pick < vocab.size())
        {
            const QString t = vocab[pick];
            QStringList tags = ItemMarks::get(key).tags;
            if (tags.contains(t)) tags.removeAll(t); else tags << t;
            ItemMarks::setTags(key, tags);
            selTag = t;
        }
    }
}

// The browse Filter menu (triggered by "F" on the themed browse view): a NavMenu over All / Favorites / each
// completion status, plus a "By tag…" sub-pick when the profile has any tags. The chosen mode is a TRANSIENT,
// level-scoped presentation filter (HomeView::setBrowseFilter) — not persisted, cleared on the next level load.
// After a pick we re-read browseItems() into the live view so the narrowed set (and any orphaned group header)
// refreshes in place with no re-fetch.
void MainWindow::runThemedBrowseFilter()
{
    // Target the current themed browse surface — the flat browse view, or the XMB home column while drilled
    // into a catalog (mirrors the browseItemsChanged fan-out). Nothing to filter on any other surface.
    QWidget* tgt = nullptr;
    if (themedBrowse_ && stack_->currentWidget() == themedBrowse_) tgt = themedBrowse_;
    else if (themedHome_ && themedHomeIsXmb_ && themedXmbInCatalog_ && stack_->currentWidget() == themedHome_)
        tgt = themedHome_;
    if (!tgt) return;
    struct Opt { int mode; int comp; QString label; };
    const QVector<Opt> opts = {
        { 0, 0, tr("All") },
        { 1, 0, tr("★ Favorites") },
        { 2, static_cast<int>(ItemMarks::Completion::Planned),    tr("Planned") },
        { 2, static_cast<int>(ItemMarks::Completion::InProgress), tr("In progress") },
        { 2, static_cast<int>(ItemMarks::Completion::Finished),   tr("Finished") },
        { 2, static_cast<int>(ItemMarks::Completion::Abandoned),  tr("Abandoned") },
    };
    const QStringList tags = ItemMarks::tagVocab();
    QStringList rows;
    for (const Opt& o : opts) rows << o.label;
    int byTagIdx = -1;
    if (!tags.isEmpty()) { byTagIdx = rows.size(); rows << tr("By tag…"); }

    const int pick = NavMenu::pick(tr("Filter"), rows, this);
    if (pick < 0) return; // Back leaves the current filter unchanged
    if (pick == byTagIdx)
    {
        const int t = NavMenu::pick(tr("Filter by tag"), tags, this);
        if (t < 0 || t >= tags.size()) return;
        home_->setBrowseFilter(3, 0, tags[t]);
    }
    else
    {
        home_->setBrowseFilter(opts[pick].mode, opts[pick].comp, QString());
    }
    if (QQuickItem* r = ThemeEngine::rootItem(tgt))
    {
        r->setProperty("items", home_->browseItems()); // narrow the presentation (rebuilds the row map)
        r->setProperty("currentIndex", home_->browseRestoreIndex());
    }
}

// ---- Themed AUDIO now-playing page (Task 5) ---------------------------------------------------------------
// The current themed surface (home/browse) that hosts the audio page, or null if we're not on one.
QWidget* MainWindow::themedAudioHost() const
{
    QWidget* cur = stack_->currentWidget();
    return (cur == themedHome_ || cur == themedBrowse_) ? cur : nullptr;
}

// Switch the current themed surface to the `nowplayingAudio` view (mpv keeps playing invisibly — the classic
// player page is never shown), pushing a "nowplaying" nav level whose Back leaves the page. Follows the detail
// mechanism: populate the page data FIRST (no empty flash), flip currentView, then push the level. A theme
// without the view (older/user themes) falls back to the classic player page so audio always has a surface.
void MainWindow::showThemedAudioPage()
{
    QWidget* cur = themedAudioHost();
    if (!cur)
    {
        // Playback started while not on a themed surface (a classic transient under themed mode): bring the
        // themed home forward so the page has a home to live on.
        if (!themedHome_) { themedAudioSession_ = false; stack_->setCurrentWidget(playerPage_); revealMediaControls(); return; }
        stack_->setCurrentWidget(themedHome_);
        cur = themedHome_;
    }
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r) return;
    if (!r->property("theme").toMap().value(QStringLiteral("views")).toMap().contains(QStringLiteral("nowplayingAudio")))
    {
        // The theme has no audio page — keep the classic player page for this session.
        // Deprecation signal (B2 Task 6, item 2): a themed-mode audio session that falls back to the classic
        // QSplitter player page. The Task 7 walk greps this; a themed theme WITH an audio view stays silent.
        mwLog(QStringLiteral("deprecated-classic: audio-nowplaying"));
        themedAudioSession_ = false;
        playlist_->clear();
        for (const QString& t : themedAudioQueue_) playlist_->addItem(t);
        playlist_->setCurrentRow(themedAudioCurrent_);
        playlist_->setVisible(true);
        stack_->setCurrentWidget(playerPage_);
        revealMediaControls();
        return;
    }
    // Push the live data before flipping the view (so the page never paints empty).
    r->setProperty("audioData", themedAudioData_);
    r->setProperty("audioPaused", themedAudioPaused_);
    r->setProperty("audioSpeed", player_ ? player_->speed() : 1.0);
    pushThemedAudioQueue();
    themedAudioPushSec_ = -1;
    updateThemedAudioProgress();
    cur->setFocus();
    if (r->property("currentView").toString() == QStringLiteral("nowplayingAudio")) return; // already open (re-fired)
    r->setProperty("audioTransportIndex", 0);
    r->setProperty("audioQueueIndex", 0);
    r->setProperty("audioZone", QStringLiteral("transport"));
    const QString ret = r->property("currentView").toString();
    r->setProperty("currentView", QStringLiteral("nowplayingAudio")); // -> ThemeBridge::syncAudioPageZone counts zones up
    if (NavGraph* g = ThemeEngine::navGraph(cur))
        g->pushLevel(QStringLiteral("nowplaying"), [this, cur, ret] { leaveThemedAudioPage(cur, ret); });
}

// Leave the audio page (the "nowplaying" level's onPop). PRESERVES today's classic behaviour EXACTLY: on the
// classic player page, a Back gesture runs goBack()'s content-page branch — player stop + clear the queue +
// return to the previous screen (verified: leaving audio STOPS playback, it does not keep playing). So here we
// stop mpv, clear the session queue, and restore the themed surface's previous view (home/browse).
void MainWindow::leaveThemedAudioPage(QWidget* surface, const QString& returnView)
{
    themedAudioSession_ = false;
    themedAudioPushSec_ = -1;
    player_->stop();
    session_->clearQueue();
    if (QQuickItem* rr = ThemeEngine::rootItem(surface))
        rr->setProperty("currentView", returnView); // -> audio zones hidden, home cursor restored
}

// Map a transport-strip verb to the live player/session call, reusing the SAME MpvWidget / PlaybackSession API
// the classic transport row uses (no new player API). Then reflect the play/pause + speed state on the page.
void MainWindow::runThemedAudioTransport(const QString& verb)
{
    if (!player_) return;
    if (verb == QStringLiteral("playPause"))        { player_->togglePause(); themedAudioPaused_ = !themedAudioPaused_; }
    else if (verb == QStringLiteral("seekBack"))    player_->seekRelative(-15.0);
    else if (verb == QStringLiteral("seekFwd"))     player_->seekRelative(15.0);
    else if (verb == QStringLiteral("prevChapter")) player_->prevChapter();
    else if (verb == QStringLiteral("nextChapter")) player_->nextChapter();
    else if (verb == QStringLiteral("prevTrack"))   { if (session_) session_->prev(); }
    else if (verb == QStringLiteral("nextTrack"))   { if (session_) session_->next(); }
    else if (verb == QStringLiteral("speed"))
    {
        // Cycle the playback rate (the Task-3/4 ThemedChoice cycle idiom, applied to the transport button).
        static const double steps[] = { 1.0, 1.25, 1.5, 1.75, 2.0 };
        const int n = int(sizeof(steps) / sizeof(steps[0]));
        const double cur = player_->speed();
        int i = 0; double best = 1e9;
        for (int k = 0; k < n; ++k) { const double d = qAbs(steps[k] - cur); if (d < best) { best = d; i = k; } }
        player_->setSpeed(steps[(i + 1) % n]);
    }
    if (QWidget* cur = themedAudioHost())
        if (QQuickItem* r = ThemeEngine::rootItem(cur))
        {
            r->setProperty("audioPaused", themedAudioPaused_);
            r->setProperty("audioSpeed", player_->speed());
        }
}

// Push the throttled position/duration (and the live paused/speed) into the page's QML props. Called from
// onPosition at ~1 Hz (whole-second changes only — not mpv's event rate), so the progress bar steps once a
// second instead of re-rendering continuously.
void MainWindow::updateThemedAudioProgress()
{
    QWidget* cur = themedAudioHost();
    if (!cur) return;
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r || r->property("currentView").toString() != QStringLiteral("nowplayingAudio")) return;
    r->setProperty("audioPosition", session_ ? session_->position() : 0.0);
    r->setProperty("audioDuration", duration_);
    r->setProperty("audioPaused", themedAudioPaused_);
    r->setProperty("audioSpeed", player_ ? player_->speed() : 1.0);
}

// Push the session queue titles + the current row into the page's QML props (the queue-list zone). Also
// reflects the current track's title as the now-playing title (a folder queue advances per track; a single
// audiobook keeps its one title), preserving the cover art already in audioData.
void MainWindow::pushThemedAudioQueue()
{
    QWidget* cur = themedAudioHost();
    if (!cur) return;
    QQuickItem* r = ThemeEngine::rootItem(cur);
    if (!r) return;
    QVariantList q;
    for (const QString& t : themedAudioQueue_) q << t;
    r->setProperty("audioQueue", q);
    r->setProperty("audioQueueCurrent", themedAudioCurrent_);
    if (themedAudioCurrent_ >= 0 && themedAudioCurrent_ < themedAudioQueue_.size())
    {
        themedAudioData_.insert(QStringLiteral("title"), themedAudioQueue_[themedAudioCurrent_]);
        r->setProperty("audioData", themedAudioData_);
    }
}

// Keep the current themed screen's NavGraph level stack in lockstep with the app's real navigation state, so a
// single nav.back()/goBack() pops exactly one real level and bottoms out (rootBack) at the screen root. The
// desired stack, bottom -> top, is: an optional "catalog" level (XMB, only inside a MULTI-catalog bucket, its
// onPop re-shows the catalog list) then one "browse" level per HomeView drill beyond the catalog root (onPop =
// home_->browseBack()). Reconciled WITHOUT running any onPop (the underlying navigation already moved) — pushes
// add, popLevelSilent drops. A no-op off a themed screen, while an overlay owns the stack, or mid-onPop (the
// back() in progress already did the one real pop; the later async browseItemsChanged re-syncs).
void MainWindow::syncThemedLevels()
{
    QWidget* cur = stack_->currentWidget();
    NavGraph* g = (cur == themedHome_ || cur == themedBrowse_) ? ThemeEngine::navGraph(cur) : nullptr;
    if (!g || g->isPopping()) return;
    if (NavOverlay::topmost()) return;   // an overlay owns the top of the stack — reconcile after it closes
    // The themed DETAIL view's level is NOT part of the catalog/browse mirror this reconciler manages, and
    // popLevelSilent drops the TOPMOST level regardless of name — so while a "detail" level is up (an async
    // browseItemsChanged can land mid-detail), stand off entirely: reconciling here could silently swallow
    // the detail level and orphan the view. The next navigation change after the detail pop re-syncs.
    if (g->countLevels(QStringLiteral("detail")) > 0) return;
    // The audio now-playing page's level is likewise not part of the catalog/browse mirror — stand off while
    // it is up so an async browseItemsChanged can't popLevelSilent it and orphan the page (mirrors "detail").
    if (g->countLevels(QStringLiteral("nowplaying")) > 0) return;

    bool wantCatalog = false;
    int  wantBrowse  = 0;
    if (cur == themedHome_ && themedHomeIsXmb_)
    {
        if (themedXmbInCatalog_)
        {
            wantBrowse  = qMax(0, home_->browseDepth() - 1);
            wantCatalog = !themedXmbAutoOpened_; // a single auto-opened catalog IS the root: no catalog level
        }
    }
    else if (cur == themedBrowse_)
        wantBrowse = qMax(0, home_->browseDepth() - 1);

    // Browse levels sit on TOP of the catalog level, so always reconcile them from the top. Catalog presence
    // only ever flips while the browse depth is 0 (entering / leaving a bucket), so it is safe to add/remove
    // the catalog level once the browse levels are cleared.
    const bool hasCatalog = g->countLevels(QStringLiteral("catalog")) > 0;
    if (hasCatalog && !wantCatalog)
    {
        while (g->countLevels(QStringLiteral("browse")) > 0) g->popLevelSilent();
        g->popLevelSilent(); // the catalog level (now on top)
    }
    else if (!hasCatalog && wantCatalog)
    {
        while (g->countLevels(QStringLiteral("browse")) > 0) g->popLevelSilent();
        g->pushLevel(QStringLiteral("catalog"), themedCatalogPop_);
    }
    int curBrowse = g->countLevels(QStringLiteral("browse"));
    while (curBrowse > wantBrowse) { g->popLevelSilent(); --curBrowse; }
    while (curBrowse < wantBrowse) { g->pushLevel(QStringLiteral("browse"), [this] { home_->browseBack(); }); ++curBrowse; }
}

void MainWindow::showThemedXmb()
{
    themedHomeIsXmb_ = true;
    themedXmbInCatalog_ = false;

    // Debounce the live-metadata addon fetch: while you scroll the column the panel tracks instantly off a
    // skeleton, and only the row you settle on triggers the (networked) synopsis/facts fetch.
    if (!themedMetaTimer_)
    {
        themedMetaTimer_ = new QTimer(this);
        themedMetaTimer_->setSingleShot(true);
        themedMetaTimer_->setInterval(160);
        connect(themedMetaTimer_, &QTimer::timeout, this, [this] {
            if (themedHomeIsXmb_ && themedXmbInCatalog_ && themedMetaWant_ >= 0)
                home_->enrichThemedMeta(); // the networked half, for the row we settled on
        });
    }

    QVariantList cats = home_->categoryItems(); // the buckets that have catalogs (each {title,key,glyph,accent})
    themedXmbCatKeys_.clear();
    for (const QVariant& v : cats) themedXmbCatKeys_ << v.toMap().value(QStringLiteral("key")).toString();
    cats << QVariantMap{ { QStringLiteral("title"), tr("Profiles") }, { QStringLiteral("glyph"), QStringLiteral("profiles") },
                         { QStringLiteral("accent"), QStringLiteral("#3E6FB8") } };
    themedXmbCatKeys_ << QStringLiteral("profiles");
    const int profilesIdx = int(cats.size()) - 1;
    cats << QVariantMap{ { QStringLiteral("title"), tr("Settings") }, { QStringLiteral("glyph"), QStringLiteral("settings") },
                         { QStringLiteral("accent"), QStringLiteral("#5B6470") } };
    themedXmbCatKeys_ << QStringLiteral("settings");
    const int settingsIdx = int(cats.size()) - 1;

    // The Profiles column: every profile (the current one ticked) to scroll + select, plus an add/edit entry.
    auto profilesColumn = [] {
        QVariantList out;
        const QString cur = ProfileStore::currentId();
        for (const Profile& p : ProfileStore::list())
            out << QVariantMap{
                { QStringLiteral("title"), (p.icon.isEmpty() ? QString() : p.icon + QStringLiteral("  ")) + p.name
                                           + (p.id == cur ? QStringLiteral("   ✓") : QString()) },
                { QStringLiteral("profileId"), p.id }, { QStringLiteral("accent"), QStringLiteral("#3E6FB8") } };
        out << QVariantMap{ { QStringLiteral("title"), QObject::tr("＋  Add / edit profiles…") },
                            { QStringLiteral("profileAction"), QStringLiteral("manage") },
                            { QStringLiteral("accent"), QStringLiteral("#5B6470") } };
        return out;
    };

    const QStringList themes = ThemeEngine::availableThemes();
    QString themeName = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
    if (!themes.contains(themeName)) themeName = themes.value(0, QStringLiteral("Default"));
    const QString themeDir = ThemeEngine::themesRoot() + QStringLiteral("/") + themeName;
    const int startCat = qBound(0, themedHomeIndex_, settingsIdx);

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("My Media Vault"));

    // Show a bucket's catalog list as the column. If the bucket has a single catalog (e.g. Games -> one
    // catalog whose top level IS the console list), open it directly so the column shows its contents (the
    // consoles), skipping a pointless one-item folder.
    auto showCatalogs = [this, profilesColumn](int cat, int selectIdx) {
        const QString key = (cat >= 0 && cat < themedXmbCatKeys_.size()) ? themedXmbCatKeys_[cat] : QString();
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        // Leaving the items level (to a catalog list / profiles / settings): no leaf is selected, so drop the
        // metadata panel and any open action chooser. (When a catalog loads, browseItemsChanged repopulates it.)
        if (r) { r->setProperty("selectedMeta", QVariantMap()); r->setProperty("actionsOpen", false);
                 r->setProperty("catLoading", false); } // default: no spinner (the async branches below turn it on)
        if (key == QStringLiteral("profiles")) // the Profiles column: scroll to a profile to select it
        {
            themedXmbInCatalog_ = false; themedXmbAutoOpened_ = false;
            themedXmbCatalogs_ = profilesColumn();
            int sel = selectIdx; const QString cur = ProfileStore::currentId();
            for (int i = 0; i < themedXmbCatalogs_.size(); ++i)
                if (themedXmbCatalogs_[i].toMap().value(QStringLiteral("profileId")).toString() == cur) { sel = i; break; }
            if (r) { r->setProperty("items", themedXmbCatalogs_); r->setProperty("currentIndex", qMax(0, sel)); }
            return;
        }
        themedXmbCatalogs_ = (key == QStringLiteral("settings")) ? QVariantList() : home_->categoryCatalogs(key);
        // Count real catalogs (navKey-bearing): the trailing Playlists folder has none, so a lone-catalog bucket
        // still auto-opens its single catalog rather than stranding on a two-row [catalog, Playlists] column.
        int realCatalogs = 0; QString onlyNavKey;
        for (const QVariant& v : themedXmbCatalogs_)
        {
            const QString nk = v.toMap().value(QStringLiteral("navKey")).toString();
            if (!nk.isEmpty()) { ++realCatalogs; onlyNavKey = nk; }
        }
        if (realCatalogs == 1) // single catalog -> open straight into its contents
        {
            themedXmbInCatalog_ = true;
            themedXmbAutoOpened_ = true;
            if (r) { r->setProperty("items", QVariantList()); r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // clear + spinner while it loads
            home_->activateNav(onlyNavKey);
            return;
        }
        themedXmbInCatalog_ = false;
        themedXmbAutoOpened_ = false;
        const int sel = qBound(0, selectIdx, qMax(0, int(themedXmbCatalogs_.size()) - 1));
        if (r) { r->setProperty("items", themedXmbCatalogs_); r->setProperty("currentIndex", sel); }
    };

    auto onActivated = [this, settingsIdx, profilesIdx](int itemIdx) {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int cat = r ? r->property("catIndex").toInt() : 0;
        if (cat == settingsIdx) { openSettingsHub(); return; } // the full settings hub: Add-ons, Cloud Sync, Appearance, …
        if (cat == profilesIdx) // the Profiles column: pick a profile to switch, or open the add/edit dialog
        {
            if (itemIdx < 0 || itemIdx >= themedXmbCatalogs_.size()) return;
            const QVariantMap m = themedXmbCatalogs_[itemIdx].toMap();
            const QString id = m.value(QStringLiteral("profileId")).toString();
            if (!id.isEmpty())
            {
                if (id != ProfileStore::currentId()) { ProfileStore::setCurrent(id); home_->refresh(); }
                showHomeScreen(); // rebuild for the chosen profile (stays on the Profiles category)
            }
            else if (m.value(QStringLiteral("profileAction")).toString() == QStringLiteral("manage"))
            {
                themedHomeIndex_ = profilesIdx; // return here after the dialog
                onSwitchProfile();
            }
            return;
        }
        if (!themedXmbInCatalog_) // the column is the catalog list -> open the chosen catalog into its items
        {
            if (itemIdx < 0 || itemIdx >= themedXmbCatalogs_.size()) return;
            const QVariantMap sel = themedXmbCatalogs_[itemIdx].toMap();
            const QString plCategory = sel.value(QStringLiteral("playlistsCategory")).toString();
            if (!plCategory.isEmpty()) // the category-level Playlists folder -> drill its lists as a new root
            {
                themedXmbCatalogIndex_ = itemIdx;  // Back re-selects the Playlists row in the bucket column
                themedXmbInCatalog_ = true;
                themedXmbAutoOpened_ = false;       // a "catalog" nav level backs out to the bucket column
                if (r) r->setProperty("catLoading", true); // spinner until the deferred open lands
                // Defer the actual open one event-loop tick. openPlaylistsLevel fills the column SYNCHRONOUSLY
                // (unlike a real catalog's async fetch); firing it inside this activation handler would race the
                // QML nav model's own activate() transition and strand focus on the bucket column. A queued call
                // lets the QML settle first, exactly mirroring how an async catalog's items arrive next tick.
                QTimer::singleShot(0, this, [this, plCategory] {
                    home_->openPlaylistsLevel(plCategory, /*asRoot*/ true);
                    syncThemedLevels();
                });
                return;
            }
            const QString navKey = sel.value(QStringLiteral("navKey")).toString();
            if (navKey.isEmpty()) return;
            themedXmbCatalogIndex_ = itemIdx;       // remember the catalog, so Back re-selects it in the list
            themedXmbInCatalog_ = true;
            if (r) { r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // spinner while it loads
            home_->activateNav(navKey); // its items land via browseItemsChanged (which now targets this column)
            syncThemedLevels(); // entered a catalog: push its "catalog" level now (before the async items land)
        }
        else if (home_->atRecentsLevel() || home_->atDownloadsLevel())
            home_->browseActivate(itemIdx); // a Recent/Downloaded row -> re-open the local file at its spot
        else // inside a catalog: containers drill in-column; a leaf opens the inline action chooser
        {
            const QVariantList col = r ? r->property("items").toList() : QVariantList();
            const QVariantMap m = (itemIdx >= 0 && itemIdx < col.size()) ? col[itemIdx].toMap() : QVariantMap();
            const bool expandable = m.value(QStringLiteral("expandable")).toBool();
            const bool synthetic = m.value(QStringLiteral("type")).toString().startsWith(QLatin1Char('_'));
            // A container (series/console/volume) or a synthetic row (Playlists folder, a playlist, New) drills
            // / acts via the normal path; a real leaf opens the inline Play / Favorite / Add-to-playlist chooser.
            if (expandable || synthetic) { home_->browseActivate(itemIdx); syncThemedLevels(); } // drilled -> a browse level
            else if (r)
            {
                r->setProperty("actionItem", itemIdx);
                r->setProperty("actionFav", home_->isThemedLeafFavorite(itemIdx));
                r->setProperty("actionIndex", 0);
                r->setProperty("actionsOpen", true);
            }
        }
    };
    // The themed XMB back is now the NavGraph's level stack (kept in lockstep by syncThemedLevels): a deeper
    // browse drill is a "browse" level (onPop = home_->browseBack()); being inside a MULTI-catalog bucket is a
    // "catalog" level (onPop = themedCatalogPop_ below, re-showing the catalog list). When those are all
    // unwound, nav.back()'s rootBack lands HERE and brings up the app pause menu. A single-catalog bucket whose
    // contents ARE the root has no "catalog" level, so it too bottoms out straight to the pause menu.
    auto onBack = [this] { showEscMenu(); };
    // Pop of the "catalog" level: back out of the opened catalog, re-selecting it in the bucket's list (reads
    // the live category cursor, exactly as the old closure branch did).
    themedCatalogPop_ = [this, showCatalogs] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int cat = r ? r->property("catIndex").toInt() : 0;
        showCatalogs(cat, themedXmbCatalogIndex_);
    };
    auto onCycle = [this, themes, themeName] {
        if (themes.isEmpty()) return;
        const QString next = themes[(qMax(0, int(themes.indexOf(themeName))) + 1) % themes.size()];
        store().setValue(QStringLiteral("themedHome/theme"), next); store().sync();
        showThemedHome();
    };
    auto onSearch = [this] {
        // Inside a catalog: scope the search to it. At the XMB root: search every add-on at once (cross-addon).
        if (themedXmbInCatalog_)
        {
            const QString q = promptThemedSearch(home_->browseTitle());
            // A non-console search resets HomeView's drill stack to the base level (doSearch): sync the graph
            // levels NOW, not just on the async browseItemsChanged, so a fast Back can't pop a stale level.
            if (!q.isNull()) { home_->searchInBrowse(q); syncThemedLevels(); }
        }
        else
        {
            const QString q = promptThemedSearch(tr("everything"));
            if (!q.isNull() && !q.trimmed().isEmpty()) { home_->searchEverything(q); showThemedBrowse(); }
        }
    };
    auto onNearEnd = [this] { if (themedXmbInCatalog_ && home_->browseHasMore()) home_->browseLoadMore(); };
    auto onCategory = [this, showCatalogs] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (!r) return;
        themedHomeIndex_ = r->property("catIndex").toInt();
        themedXmbCatalogIndex_ = 0;                  // a different bucket starts at the top of its catalog list
        showCatalogs(themedHomeIndex_, 0); // switching bucket resets the column to its catalog list
        syncThemedLevels(); // bucket switch clears the old catalog/browse levels (or adds the new auto-opened one)
    };
    // The column selection moved: refresh the live metadata panel for the new row (only while in a catalog).
    auto onSelect = [this](int idx) { if (themedXmbInCatalog_) refreshThemedMeta(idx); };
    // The inline chooser fired: 0 = Play the leaf (and close), 1 = toggle Favorite (and stay, so the heart
    // updates in place; the metadata panel's "★ Favorited" follows too), 2 = Add to a playlist (and close),
    // 3 = Download the leaf for keeps (and close).
    auto onAction = [this](int which) {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (!r) return;
        const int idx = r->property("actionItem").toInt();
        if (which == 0)      { r->setProperty("actionsOpen", false); home_->playThemedLeaf(idx); }
        else if (which == 2) { r->setProperty("actionsOpen", false); home_->addBrowseItemToPlaylist(idx); }
        else if (which == 3) { r->setProperty("actionsOpen", false); home_->downloadThemedLeaf(idx); }
        else
        {
            home_->favoriteThemedLeaf(idx);
            r->setProperty("actionFav", home_->isThemedLeafFavorite(idx));
        }
    };
    // "P" on the highlighted item: add it to a playlist (only while inside a catalogue, on a real media row).
    auto onPlaylistAdd = [this] {
        if (!themedXmbInCatalog_) return;
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (r) home_->addBrowseItemToPlaylist(r->property("currentIndex").toInt());
    };

    // "F" inside an XMB catalog opens the transient browse Filter menu (a no-op at the XMB category root).
    auto onButton = [this](const QString& v) {
        if (v == QStringLiteral("filter") && themedXmbInCatalog_) runThemedBrowseFilter();
    };
    QWidget* w = ThemeEngine::buildView(themeDir, QVariantList(), system, this,
                                        onActivated, onBack, onCycle, onSearch, onNearEnd, onCategory,
                                        onSelect, onAction, onPlaylistAdd, onButton,
                                        [this] { openThemedDetail(-1); },
                                        [this](const QString& v) { runThemedDetailAction(v); },
                                        [this](const QString& v) { runThemedAudioTransport(v); },
                                        [this](int row) { if (session_) session_->playIndex(row); });
    if (QQuickItem* r = ThemeEngine::rootItem(w))
    {
        r->setProperty("categories", cats);
        r->setProperty("catIndex", startCat);
        r->setProperty("uiMotionMs", kUiFadeMs); // J05: XMB motion duration — FeedbackPolicy.h owns the value
    }
    QWidget* old = themedHome_;
    themedHome_ = w;
    applyThemeMusic(themeDir); // ship this theme's default menu music (used when the user has no music folder)
    updateThemedNowPlaying(); // seed the Triple theme's now-playing readout
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    w->setFocus();
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
    nudgeThemedHome(); // repaint the rebuilt themed home

    showCatalogs(startCat, themedXmbCatalogIndex_); // populate the starting bucket's catalog list (restore on rebuild)
    syncThemedLevels(); // establish the initial level stack (an auto-opened single catalog restores its drill depth)

    if (!themeWatcher_)
    {
        themeWatcher_ = new QFileSystemWatcher(this);
        connect(themeWatcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
            if (themedHomeEnabled() && stack_->currentWidget() == themedHome_)
                QTimer::singleShot(150, this, [this] {
                    if (themedHomeEnabled() && stack_->currentWidget() == themedHome_) showThemedHome();
                });
        });
    }
    themeWatcher_->removePaths(themeWatcher_->files());
    const QString themeFile = themeDir + QStringLiteral("/theme.json");
    if (QFile::exists(themeFile)) themeWatcher_->addPath(themeFile);
}

// Themed "gamelist": the current catalog level's items, rendered through the theme's `browse` view. The data
// + actions are driven by HomeView (which already loads/drills/opens); we just mirror its items and route
// activate/back back into it. browseItemsChanged() (connected in the ctor) refreshes us after a drill.
void MainWindow::showThemedBrowse()
{
    const QStringList themes = ThemeEngine::availableThemes();
    QString themeName = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
    if (!themes.contains(themeName)) themeName = themes.value(0, QStringLiteral("Default"));

    QVariantMap system; system.insert(QStringLiteral("name"), home_->browseTitle());
    // An info-page leaf (movie/series/book/comic/…) opens the themed DETAIL view (the retired classic page's
    // replacement); a container drills and a direct-open leaf (game/track) opens — both via browseActivate.
    auto onActivated = [this](int idx) {
        if (openThemedDetailForInfoLeaf(idx)) return;
        home_->browseActivate(idx); syncThemedLevels();
    };
    // rootBack for the browse view: every deeper drill is a "browse" level (onPop = home_->browseBack()); when
    // they are all unwound nav.back()'s rootBack lands here and returns to the themed (system) home.
    auto onBack = [this] { showThemedHome(); };
    auto onCycle = [this, themes, themeName] {
        if (themes.isEmpty()) return;
        const QString next = themes[(qMax(0, int(themes.indexOf(themeName))) + 1) % themes.size()];
        store().setValue(QStringLiteral("themedHome/theme"), next); store().sync();
        showThemedBrowse();
    };
    // "/" searches within the current catalog/console; the result refreshes via browseItemsChanged.
    auto onSearch = [this] {
        const QString q = promptThemedSearch(home_->browseTitle());
        // Sync the graph levels NOW: a non-console search collapses the drill stack (doSearch), and a Back in
        // the async window before browseItemsChanged must not pop a stale browse level.
        if (!q.isNull()) { home_->searchInBrowse(q); syncThemedLevels(); }
    };
    // Selection neared the end -> pull the next page (if any). browseItemsChanged appends + keeps selection.
    auto onNearEnd = [this] { if (home_->browseHasMore()) home_->browseLoadMore(); };

    // "F" on the browse view emits actionRequested("filter") -> open the transient browse Filter menu.
    auto onButton = [this](const QString& v) { if (v == QStringLiteral("filter")) runThemedBrowseFilter(); };
    QWidget* w = ThemeEngine::buildView(ThemeEngine::themesRoot() + QStringLiteral("/") + themeName,
                                        home_->browseItems(), system, this, onActivated, onBack, onCycle,
                                        onSearch, onNearEnd, {}, {}, {}, {}, onButton,
                                        [this] { openThemedDetail(-1); },
                                        [this](const QString& v) { runThemedDetailAction(v); },
                                        [this](const QString& v) { runThemedAudioTransport(v); },
                                        [this](int row) { if (session_) session_->playIndex(row); });
    if (QQuickItem* r = ThemeEngine::rootItem(w)) r->setProperty("currentView", QStringLiteral("browse"));
    QWidget* old = themedBrowse_;
    themedBrowse_ = w;
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    w->setFocus();
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
    syncThemedLevels(); // seed this view's browse-drill levels from HomeView's current stack depth
}

// The home theme picker, as a full-screen panel page in the main window (like the other settings screens).
// Changes save as you make them and preview live; backing out (-> the settings hub -> home) applies them.
void MainWindow::openAppearance()
{
    // Themed mode: render Appearance as a flat PanelRow list on the Nav Contract (ThemedPanelHost), instead of
    // the classic QWidget builder — the last classic surface reachable in themed mode (B2 Task 6.75). Same setters
    // verbatim: the themed-home Toggle writes themedHome/enabled; the theme Choice writes themedHome/theme (+sync).
    //
    // PREVIEW: the classic panel embedded a live QQuickWidget preview of the picked theme. In themed mode the app
    // ITSELF is theme-rendered, so apply-on-select IS the preview — picking a theme restyles the LIVE panel host
    // to that theme's settingsPanel block (setStyle(settingsPanelStyle()), which reads the just-saved key), so the
    // surface you are looking at re-renders in the new theme immediately. We deliberately do NOT rebuild the themed
    // HOME underlay here: showThemedHome() ends in stack_->setCurrentWidget(themedHome_), which would yank the
    // current page away from this panel — exactly the wedge follow-up #5 recorded against the classic panel
    // (Back could no longer leave the host). The FULL theme (home/browse/detail layout) applies on exit: the hub
    // root's onBack already calls showHomeScreen(), which rebuilds the themed home from the saved key. So:
    // apply-on-select restyles the panel live; the full theme lands on the way out — no underlay rebuild, no wedge.
    //
    // TOGGLE: turning themedHome/enabled OFF while inside the themed panel just SAVES the key (classic semantics —
    // value persists, takes effect on next navigation). We do NOT hot-swap the whole UI mid-panel; the panel stays
    // up until the user leaves, at which point normal navigation honours the new value (Back to the hub root ->
    // showHomeScreen renders the classic home if it was turned off). Least-surprising, and matches the classic setter.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        // Theme folder <-> display-name mapping (the Choice cycles display names; the handler maps the pick back
        // to its folder for the store, mirroring General's subtitle-language display<->code round-trip).
        const QStringList themeFolders = ThemeEngine::availableThemes();
        QList<QPair<QString, QString>> themePairs;   // (display, folder), captured by the handler
        QStringList themeOpts;
        for (const QString& folder : themeFolders)
        {
            const QString disp = ThemeEngine::themeDisplayName(folder);
            themePairs << qMakePair(disp, folder);
            themeOpts << disp;
        }
        QString curFolder = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
        if (!themeFolders.contains(curFolder)) curFolder = themeFolders.value(0, QStringLiteral("Default"));
        QString curDisp;
        for (const auto& p : themePairs) if (p.second == curFolder) { curDisp = p.first; break; }
        if (curDisp.isEmpty()) curDisp = themeOpts.value(0);

        themedPanelHost_->setStyle(settingsPanelStyle());   // the active theme's settingsPanel block (hard fallbacks)

        QVector<PanelRow> rows;
        auto sep    = [&rows](const QString& t) { PanelRow r; r.kind = PanelRow::Separator; r.label = t; rows << r; };
        auto info   = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto toggle = [&rows](const QString& id, const QString& label, bool on) {
            PanelRow r; r.kind = PanelRow::Toggle; r.id = id; r.label = label; r.checked = on; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        auto choice = [&rows](const QString& id, const QString& label, const QStringList& opts, const QString& cur) {
            PanelRow r; r.kind = PanelRow::Choice; r.id = id; r.label = label; r.options = opts; r.value = cur; rows << r; };

        // Display-mode <-> stored-value mapping (Choice cycles display names; the handler maps back to the stored
        // key). Order matches the stored values below one-for-one.
        const QStringList dispOpts   = { tr("Auto"), tr("Desktop"), tr("TV"), tr("Mobile") };
        const QStringList dispValues = { QStringLiteral("auto"), QStringLiteral("desktop"),
                                         QStringLiteral("tv"), QStringLiteral("mobile") };
        const int dispIdx = qMax(0, dispValues.indexOf(Settings::displayMode()));
        const QString dispCur = dispOpts.value(dispIdx);

        toggle(QStringLiteral("appr.themed"), tr("Use the themed home screen (beta)"), themedHomeEnabled());
        sep(tr("Theme"));
        choice(QStringLiteral("appr.theme"), tr("Theme"), themeOpts, curDisp);
        info(QStringLiteral("appr.applies"),
             tr("Applies live as you pick — the full theme takes effect when you leave Appearance."), QString());
        sep(tr("Display mode"));
        choice(QStringLiteral("appr.dispmode"), tr("Display mode"), dispOpts, dispCur);
        info(QStringLiteral("appr.dispmodehint"),
             tr("Auto fits this device; TV enlarges text and controls for the couch, Mobile for touch."), QString());
        sep(tr("Get more themes"));
        info(QStringLiteral("appr.customise"),
             tr("Edit a theme's theme.json to customise it (colours, layout, artwork)."), QString());
        info(QStringLiteral("appr.root"), tr("Themes folder"), ThemeEngine::themesRoot());
        info(QStringLiteral("appr.community"),
             tr("Browse and share community themes at github.com/cubman3134/mymediavault-themes."), QString());
        action(QStringLiteral("appr.gallery"), tr("Open the theme gallery (GitHub)…"));

        themedPanelHost_->present(tr("Appearance"), rows,
            [this, themePairs, dispOpts, dispValues](const QString& id, const QString& val) {
                if (id == QStringLiteral("appr.themed")) {
                    // Save only (classic semantics); do NOT hot-swap the UI mid-panel — see the note above.
                    store().setValue(QStringLiteral("themedHome/enabled"), val == QStringLiteral("1"));
                    store().sync();
                }
                else if (id == QStringLiteral("appr.dispmode")) {
                    // Map the picked display name back to its stored value, save, then re-resolve the form factor.
                    // FormFactor::changed then live-restyles the widget surfaces (applyFormFactorWidgets) and the
                    // QML form.* bindings update automatically; restyle THIS panel on the same setStyle mechanism
                    // as the theme row so the surface you are looking at re-renders immediately.
                    const int i = dispOpts.indexOf(val);
                    const QString mode = dispValues.value(i < 0 ? 0 : i, QStringLiteral("auto"));
                    Settings::setDisplayMode(mode);
                    FormFactor::instance().refresh();
                    themedPanelHost_->setStyle(settingsPanelStyle());
                }
                else if (id == QStringLiteral("appr.theme")) {
                    QString folder = val;   // map the picked display name back to its folder
                    for (const auto& p : themePairs) if (p.first == val) { folder = p.second; break; }
                    store().setValue(QStringLiteral("themedHome/theme"), folder); store().sync();   // save on selection
                    // Apply-on-select IS the preview: restyle THIS panel to the newly-picked theme's settingsPanel
                    // block (settingsPanelStyle() reads the key we just saved). No underlay rebuild -> no wedge; the
                    // full theme lands when the hub root's onBack runs showHomeScreen() on the way out.
                    themedPanelHost_->setStyle(settingsPanelStyle());
                }
                else if (id == QStringLiteral("appr.gallery")) {
                    // Outward navigation to the browser — parity with the classic panel's openExternalLinks GitHub link.
                    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/cubman3134/mymediavault-themes")));
                }
            },
            [this] { openSettingsHub(); });   // defensive root onBack: Appearance is nested, so a pop re-renders the hub

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }

    if (stack_->currentWidget() != panelPage_) panelReturnTo_ = stack_->currentWidget();

    // A representative stand-in for the preview: the four inherent categories (so XMB themes show their cross).
    QVariantList previewItems = home_->categoryItems();
    if (previewItems.isEmpty())
        for (const char* n : { "Video", "Games", "Audio", "Reading" })
            previewItems << QVariantMap{ { QStringLiteral("title"), QString::fromLatin1(n) },
                                         { QStringLiteral("accent"), QStringLiteral("#3E8E7E") } };
    QVariantMap previewSystem; previewSystem.insert(QStringLiteral("name"), QStringLiteral("My Media Vault"));

    showPanel(tr("Appearance"), [this, previewItems, previewSystem](QVBoxLayout* v) {
        auto* enable = new QCheckBox(tr("Use the themed home screen (beta)"));
        enable->setChecked(themedHomeEnabled());
        connect(enable, &QCheckBox::toggled, this, [this](bool on) {
            store().setValue(QStringLiteral("themedHome/enabled"), on); store().sync();
        });
        v->addWidget(enable);

        auto* row = new QHBoxLayout();
        auto* leftCol = new QVBoxLayout();
        leftCol->addWidget(new QLabel(tr("Theme")));
        auto* list = new QListWidget();
        list->setMinimumWidth(240);
        list->setSpacing(4); // breathing room so rows don't crowd/overlap, especially the selected one
        list->setStyleSheet(QStringLiteral(
            "QListWidget{outline:none;border:none;}"
            "QListWidget::item{padding:10px 12px;border-radius:6px;}"
            "QListWidget::item:selected{background:#2D6CDF;color:white;}"));
        const QStringList themes = ThemeEngine::availableThemes();
        const QString current = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
        for (const QString& folder : themes)
        {
            auto* it = new QListWidgetItem(ThemeEngine::themeDisplayName(folder), list);
            it->setData(Qt::UserRole, folder);
            if (folder == current) list->setCurrentItem(it);
        }
        if (!list->currentItem() && list->count() > 0) list->setCurrentRow(0);
        leftCol->addWidget(list, 1);
        auto* hint = new QLabel(tr("Edit a theme's theme.json to customise — it previews here live."));
        hint->setWordWrap(true);
        QFont hf = hint->font(); hf.setPointSizeF(hf.pointSizeF() * 0.85); hint->setFont(hf);
        leftCol->addWidget(hint);
        row->addLayout(leftCol, 0);

        auto* previewBox = new QFrame();
        previewBox->setFrameShape(QFrame::StyledPanel);
        previewBox->setMinimumSize(480, 300);
        auto* pv = new QVBoxLayout(previewBox);
        pv->setContentsMargins(1, 1, 1, 1);
        row->addWidget(previewBox, 1);
        v->addLayout(row, 1);

        // How to get more themes and share your own (the community theme registry on GitHub).
        auto* share = new QLabel(tr(
            "<b>Get more themes &amp; share yours.</b> "
            "Themes live in <code>%1</code> — each is a folder with a <code>theme.json</code>. "
            "Browse and download community themes, or upload your own, at "
            "<a href=\"https://github.com/cubman3134/mymediavault-themes\">github.com/cubman3134/mymediavault-themes</a>. "
            "To <b>add</b> a theme, drop its folder into the directory above and pick it here. "
            "To <b>share</b> yours, add the folder under <code>themes2/</code> in that repo with an "
            "<code>index.json</code> entry and open a pull request (see <code>THEME_FORMAT.md</code> for the format).")
            .arg(ThemeEngine::themesRoot()));
        share->setTextFormat(Qt::RichText);
        share->setWordWrap(true);
        share->setOpenExternalLinks(true); // the GitHub link opens in the browser
        share->setStyleSheet(QStringLiteral("margin-top:12px;"));
        v->addWidget(share);

        auto rebuildPreview = [this, pv, previewBox, previewItems, previewSystem](const QString& folder) {
            while (QLayoutItem* old = pv->takeAt(0)) { if (old->widget()) old->widget()->deleteLater(); delete old; }
            QWidget* p = ThemeEngine::buildView(ThemeEngine::themesRoot() + QStringLiteral("/") + folder,
                                                previewItems, previewSystem, previewBox);
            p->setMinimumSize(480, 270);
            if (QQuickItem* r = ThemeEngine::rootItem(p)) // feed categories too, so an XMB theme shows its cross
                { r->setProperty("categories", previewItems); r->setProperty("catIndex", 0); }
            pv->addWidget(p);
        };
        connect(list, &QListWidget::currentItemChanged, this, [this, rebuildPreview](QListWidgetItem* it, QListWidgetItem*) {
            if (!it) return;
            const QString folder = it->data(Qt::UserRole).toString();
            store().setValue(QStringLiteral("themedHome/theme"), folder); store().sync(); // save on selection
            rebuildPreview(folder);
        });
        if (list->currentItem()) rebuildPreview(list->currentItem()->data(Qt::UserRole).toString());
    }, [this] { openSettingsHub(); });
}
#else
void MainWindow::showThemedHome() {}
void MainWindow::openAppearance() {}
void MainWindow::syncThemedLevels() {}
#endif

void MainWindow::enterSplitScreen()
{
    if (!splitView_) // first use: build the split view + its per-pane engines and wire it up
    {
        splitView_ = new SplitView(this);
        stack_->addWidget(splitView_);
        connect(splitView_, &SplitView::exitRequested, this, &MainWindow::exitSplitScreen);
        connect(splitView_, &SplitView::openHereRequested, this, [this](MediaPane* pane) {
            splitTarget_ = pane;                 // the next opened item loads into this pane
            // Deprecation signal (B2 Task 6 fix round): the split-screen pane-pick drops to the classic
            // HomeView even in themed mode (the themed home has no split-target picking flow yet).
            if (themedHomeEnabled()) mwLog(QStringLiteral("deprecated-classic: split-pane-pick"));
            stack_->setCurrentWidget(home_);     // pick it from Home; the other pane keeps playing in the background
            home_->focusContent();
        });
        applyFormFactorWidgets(); // size the just-built pane bars to the current form-factor tokens
    }
    // Park the playing views (don't leave a movie playing behind the split) and show the empty split.
    // The window state is left exactly as it is — no screen ever changes it on the user's behalf.
    player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); session_->clearQueue();
    splitMode_ = true;
    splitTarget_ = nullptr;
    stack_->setCurrentWidget(splitView_);
}

void MainWindow::exitSplitScreen()
{
    splitMode_ = false;
    splitTarget_ = nullptr;
    if (splitView_) splitView_->clearAll(); // stop both panes' engines
    openHome();
}

void MainWindow::finishSplitOpen()
{
    // The item just opened into splitTarget_; return to the split view and focus that pane.
    if (splitTarget_) splitView_->focusPane(splitTarget_);
    splitTarget_ = nullptr;
    stack_->setCurrentWidget(splitView_);
}

void MainWindow::onRequestOpenFile(const QString& kind)
{
    // The Home view's "open a file" item routes here (the toolbar open buttons were removed).
    if (kind == QStringLiteral("video"))         openFile();
    else if (kind == QStringLiteral("audio"))    openAudio();
    else if (kind == QStringLiteral("game"))     openGame();
    else if (kind == QStringLiteral("document")) openDocument();
    else if (kind == QStringLiteral("stream"))   openStreamPrompt();
}

void MainWindow::openRecent(const QString& path, const QString& kind,
                            const QString& resumeKey, const QString& title, const QString& thumb)
{
    currentNextSourceCapable_ = false; // a Recent re-open has no live Allarr context to swap sources
    // Game kinds dispatch through RecentStore::relaunchFor (the shared pure dispatch table; probe_importers pins it).
    switch (RecentStore::relaunchFor(kind))
    {
        case RecentStore::Relaunch::SteamGame:
        {
            // Re-launch through the Steam client (fire-and-forget). Key is "steam:<appid>"; the recorded path is
            // the steam://rungameid URL. Re-record so the re-open moves to the front of Recents.
            const QString appid = resumeKey.startsWith(QStringLiteral("steam:"))
                                      ? resumeKey.mid(QStringLiteral("steam:").size())
                                      : path.section(QLatin1Char('/'), -1);
            const QString url = SteamLibrary::launchUrl(appid);
            QDesktopServices::openUrl(QUrl(url));
            RecentStore::add({ url, title, QStringLiteral("steamgame"), thumb, QStringLiteral("steam:") + appid });
            statusBar()->showMessage(tr("Launching “%1” via Steam…").arg(title), 5000);
            return;
        }
        case RecentStore::Relaunch::EpicGame:
        {
            // Re-launch through the Epic launcher (fire-and-forget). Key is "epic:<AppName>"; the recorded path
            // is the launcher URI. Re-record so the re-open moves to the front of Recents.
            const QString appName = resumeKey.startsWith(QStringLiteral("epic:"))
                                        ? resumeKey.mid(QStringLiteral("epic:").size())
                                        : path.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
            const QString url = EpicLibrary::launchUrl(appName);
            QDesktopServices::openUrl(QUrl(url));
            RecentStore::add({ url, title, QStringLiteral("epicgame"), thumb, QStringLiteral("epic:") + appName });
            statusBar()->showMessage(tr("Launching “%1” via Epic…").arg(title), 5000);
            return;
        }
        // A GOG game re-opens through the monitored exe path (re-resolving from the registry, falling back to the
        // recorded exe). launchPcExe records the "goggame" Recent itself.
        case RecentStore::Relaunch::GogGame:
            relaunchGogGame(resumeKey, title, thumb, path);
            return;
        // A PC game re-opens through its remembered install (exe from PcGameStore) - even when the exact path this
        // Recent entry recorded (e.g. its one-time installer) is stale or gone.
        case RecentStore::Relaunch::PcGame:
            relaunchPcGame(resumeKey, title, thumb, path);
            return;
        default:
            break; // media kinds fall through to the file/URL handling below
    }
    // A streamed link has no local file to check; route it straight to libmpv.
    const bool isUrl = path.contains(QStringLiteral("://"));
    if (!isUrl && !QFileInfo::exists(path))
    {
        statusBar()->showMessage(tr("That file can no longer be found: %1").arg(path), kFeedbackLong);
        return;
    }
    if (isUrl && kind == QStringLiteral("audio")) openAudioStream(path, resumeKey, title);
    else if (isUrl)                              openStreamUrl(path, resumeKey, title);
    else if (kind == QStringLiteral("video"))    openVideoPath(path);
    else if (kind == QStringLiteral("audio"))    openAudioPath(path);
    else if (kind == QStringLiteral("game"))
    {
        // Re-open with the console the game was launched with, so a shared extension (.iso/.cue/.chd/.bin)
        // isn't mis-resolved by extension alone. Prefer the Downloaded record (authoritative for downloads),
        // else the Recent entry; match by stable key, else path.
        QString sysId;
        for (const DownloadedItem& d : DownloadsStore::list())
            if ((!resumeKey.isEmpty() && d.key == resumeKey) || d.path == path) { sysId = d.system; break; }
        if (sysId.isEmpty())
            for (const RecentItem& r : RecentStore::list())
                if ((!resumeKey.isEmpty() && r.key == resumeKey) || r.path == path) { sysId = r.system; break; }
        openGamePath(path, title, thumb, resumeKey, sysId); // keep its name/cover + console
    }
    else if (kind == QStringLiteral("document")) openDocumentPath(path);
}

// When a restricted (kids) profile is active and a PIN is set, require it before an "escape" action. Returns
// true when the action may proceed (no restriction, no PIN, or the correct PIN was entered).
bool MainWindow::parentalUnlock(const QString& reason)
{
    if (!Settings::hasParentalPin() || !ProfileStore::current().restricted) return true;
    const QString pin = Osk::getText(reason, QString(), QLineEdit::Password, this); // in-window PIN pad
    if (pin.isNull()) return false; // backed out
    if (Settings::checkParentalPin(pin)) return true;
    notify(tr("Incorrect PIN."), kFeedbackLong);
    return false;
}

void MainWindow::onSwitchProfile()
{
    if (!parentalUnlock(tr("Enter the parental PIN to switch profiles."))) return;
#ifdef MMV_HAVE_QML
    // Themed mode: the switcher on the Nav Contract. Back keeps the current profile (openHome), matching the
    // classic Cancel button (which mustChoose hides). The classic ProfileDialog path below stays for classic mode.
    if (themedHomeEnabled() && themedPanelHost_) { presentProfilePicker(/*mustChoose*/ false); return; }
#endif
    auto* dlg = new ProfileDialog(/*mustChoose*/ false, this);
    showDialogPanel(tr("Profiles"), dlg, [this, dlg](int result) {
        if (result == QDialog::Accepted && !dlg->selectedId().isEmpty())
            ProfileStore::setCurrent(dlg->selectedId());
        // This entry point lives on Home; openHome() switches back and refreshes for the active profile
        // (covers a switched user, a deletion repointing "current", or an edited name/icon).
        openHome();
    }, [this] { openHome(); });
}

// NOTE (non-QML link safety): onboardingChoiceTitle / presentOnboardingChoice / onboardingToFresh below are
// deliberately OUTSIDE the MMV_HAVE_QML guard (which resumes before onboardingChoiceIsTop) — showEvent() calls
// presentOnboardingChoice() UNCONDITIONALLY, so a Qt-without-qtdeclarative build must still link them. Each one's
// own inner #ifdef MMV_HAVE_QML supplies the classic (no panel host) fallback, so both worlds build.

// ---- Themed Profiles picker (B2 Task 5) --------------------------------------------------------------------
// The classic ProfileDialog (a QStackedWidget: list page + a name/icon picker page) becomes, in themed mode, a
// ThemedPanelHost presentation: the list is the root panel (Action row per profile + "Create New Profile"); the
// picker's second page is a nested panel level (name TextField via the OSK + icon Choice). Activating a profile
// row opens a NavMenu chooser (Switch / Edit / Delete) — the themed analogue of the classic row's three buttons.
// All data ops go through ProfileStore verbatim.

// First-run onboarding choice screen (onboarding/drive-restore T1): a themed two-action panel presented pre-home
// on the SAME ThemedPanelHost the startup picker uses, so it wears the picker's chrome and reads as the first
// screen. It reuses presentProfileList's Action-row idiom (rows -> host->present) rather than a NavConfirm dialog
// because a two-CHOICE landing reads cleanest as a rooted list, and it matches the picker that follows it exactly.
//   • "Set up a new library" -> mark onboarding done, then the EXISTING promptStartupProfile()/presentProfilePicker
//     fresh path, BYTE-UNCHANGED (a pure prepend — the create-a-profile flow is untouched).
//   • "Restore from Google Drive" -> beginOnboardingRestore() (T2): the signInAvailable gate -> async signIn ->
//     the pull chain -> the pure onboardingRoute (Picker on restored profiles, Fresh on an empty cloud). Every
//     failure routes back HERE with a one-line notice — never a dead end — and onboarding/done stays FALSE so a
//     mistap or a failed attempt can be retried from this same screen.
// Back on the first screen has no escape (same as the picker) -> quit-confirm. A classic (non-themed) build has no
// panel host for the choice screen, so it falls through to the fresh path directly (the choice screen is a
// themed-home feature) — keeping the classic startup path unchanged.
//
// This screen is ALSO the landing spot the restore flow re-presents on failure, so its title doubles as the
// "onboarding is still the active surface" gate (onboardingChoiceIsTop) that drops a late OAuth completion after
// the user navigated away — hence the one title source, onboardingChoiceTitle().
QString MainWindow::onboardingChoiceTitle() { return tr("Welcome to My Media Vault"); }

void MainWindow::presentOnboardingChoice()
{
#ifdef MMV_HAVE_QML
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();            // settings-area BOUNDARY: no stale async listener may outlive this present
        themedPanelHost_->reset();        // fresh ROOT presentation (also drops any stale panel levels)
        themedPanelHost_->setStyle(settingsPanelStyle());
        NavOverlay::setThemeColors(settingsPanelStyle());  // this screen's own menus/confirms match the theme

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("restore");
          r.label = tr("☁   Restore from Google Drive"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("fresh");
          r.label = tr("＋   Set up a new library"); rows << r; }

        auto onAct = [this](const QString& id, const QString&) {
            if (id == QStringLiteral("fresh"))
                onboardingToFresh();          // mark done + the EXISTING fresh path — byte-unchanged (a pure prepend)
            else if (id == QStringLiteral("restore"))
                beginOnboardingRestore();     // T2: signInAvailable gate -> signIn -> pull -> onboardingRoute
        };
        auto onBack = [this] { quitConfirmFromStartup(); };  // first screen: no escape — confirm quit / re-present

        themedPanelHost_->present(onboardingChoiceTitle(), rows, onAct, onBack);
        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        updateBackgroundMusic();
        return;
    }
#endif
    // Classic (non-themed) build: no panel host for a choice screen -> go straight to the unchanged fresh path.
    onboardingToFresh();
}

// The onboarding "fresh library" exit (onboarding/drive-restore T2): mark onboarding done, then the EXISTING
// startup profile picker — the create-a-profile flow, BYTE-UNCHANGED. Shared by the choice screen's "Set up a new
// library" action, the classic-build fallback, the Android/decline path, and the empty-cloud restore outcome.
void MainWindow::onboardingToFresh()
{
    Settings::setOnboardingDone(true);
    promptStartupProfile();
}

#ifdef MMV_HAVE_QML
// "The onboarding choice/flow is still the active surface" — the restore flow's equivalent of themedPanelIsTop's
// late-async gate (openCloudSync). While Restore is signing in / pulling, the choice screen panel stays presented
// (only a notify overlays it), so its title being the live top panel means the user hasn't quit or navigated away.
// Every async restore callback checks this FIRST and DROPS otherwise: a browser OAuth that completes minutes after
// the user backed out (quit-confirm raises a NavConfirm overlay -> overlayAbove() -> gate false) must never present
// a picker or re-present the choice screen over an unrelated surface. Classic builds never reach the flow (false).
bool MainWindow::onboardingChoiceIsTop() const
{
    return themedPanelIsTop(onboardingChoiceTitle());
}

// "Restore from Google Drive" (onboarding/drive-restore T2). The signInAvailable gate, then the async sign-in whose
// completion drives the shipped pull chain and the pure onboardingRoute. NO merge/transport changes — this is a UI
// spine over CloudSync + the existing cloudPullAtStartup chain. Every failure has a non-dead-end route:
//   • !signInAvailable (Android, until the OAuth follow-up) -> Decline -> the fresh path, with a notice.
//   • signIn fails / is cancelled                            -> back to the choice screen (token/refresh KEPT; retry).
//   • signed in, then a pull/network failure                 -> back to the choice screen ("Couldn't reach Drive").
//   • signed in, empty cloud                                 -> the fresh path (this device seeds the cloud).
//   • signed in, restored profiles                           -> the themed picker (restored profiles as rows).
// onboarding/done is set ONLY on a terminal fresh/picker outcome — a failed attempt leaves it FALSE so the user
// lands back on Restore-vs-new-library and can retry (the same signIn also re-mints an expired refresh token).
void MainWindow::beginOnboardingRestore()
{
    // The pre-sign-in decision goes through the pure router too: Decline (sign-in unavailable) -> the fresh path.
    if (mmv::onboardingRoute(/*hasLocal*/ false, /*restorePicked*/ true, /*signInOk*/ false,
                             /*remoteHasProfiles*/ false, CloudSync::signInAvailable())
        == mmv::OnboardingRoute::Decline)
    {
        notify(tr("Drive sign-in isn't available on this device yet."));
        onboardingToFresh();
        return;
    }

    // cloud_ is eager (ctor) for push-on-exit, so it always exists here. Arm one-shot, top-gated completion
    // handlers in the panelPageConns_ pool (the openCloudSync lifetime model): they are replaced wholesale the
    // moment the flow re-presents ANY panel (the choice screen on failure, or the picker on success both clear
    // the pool), so a stale handler can never outlive the surface it belongs to.
    clearPanelPageConns();
    panelPageConns_ << connect(cloud_.get(), &CloudSync::signedIn, this, [this](const QString&) {
        if (!onboardingChoiceIsTop()) return;                 // navigated away / quit before OAuth returned — drop
        onboardingRestorePull();
    });
    panelPageConns_ << connect(cloud_.get(), &CloudSync::signInFailed, this, [this](const QString& e) {
        if (!onboardingChoiceIsTop()) return;                 // late failure after navigating away — drop
        qInfo("[onboarding] Drive sign-in failed/cancelled: %s", qUtf8Printable(e));
        presentOnboardingChoice();                            // token/refresh KEPT; onboarding/done stays FALSE
        notify(tr("Couldn't sign in to Google Drive — try again, or set up a new library."));
    });

    notify(tr("Opening your browser to sign in…"));
    cloud_->signIn();                                         // async: emits signedIn(email) / signInFailed(error)
}

// Signed in — run the shipped pull chain (the cloudPullAtStartup checkStatus+applyRemote chain, but async on the
// GUI thread rather than a startup QEventLoop), then route. Distinguishes the PROVEN-empty cloud (reached AND the
// bundle-query succeeded with no file -> this device seeds it: Fresh) from a network/pull failure after auth
// (folder unreachable, the file-query itself errored, or applyRemote failed -> couldn't reach Drive -> ChoiceScreen,
// never a fresh seed — a query failure must not be read as "empty" and clobber a real backup on exit-push). The
// bundle carries the synced profiles/list, so ProfileStore::list() reflects the restored profiles once applyRemote
// has applied it; the small per-item progress doc (marks/recents/resume) merges alongside via pullAndMergeProgress.
void MainWindow::onboardingRestorePull()
{
    notify(tr("Restoring your library from Google Drive…"));
    cloud_->checkStatus([this](const CloudSync::Status& st) {
        if (!onboardingChoiceIsTop()) return;                 // navigated away mid-pull — drop
        switch (mmv::restorePullStage(st.reached, st.listReached, st.hasRemote))
        {
        case mmv::RestorePullStage::Retry:                    // unreachable OR the file-query failed: UNPROVEN empty.
            // Route to the choice screen (token kept, done stays FALSE) — never seed fresh over a backup we merely
            // couldn't read. This is the data-safety fix: a query failure must not be mistaken for an empty cloud.
            finishOnboardingRestore(/*restoreOk*/ false, /*remoteHasProfiles*/ false);
            return;
        case mmv::RestorePullStage::Seed:                     // reached AND query succeeded, no bundle -> proven-empty
            pullAndMergeProgress();                           // (no bundle, but any progress doc merges; no-ops if none)
            finishOnboardingRestore(/*restoreOk*/ true, /*remoteHasProfiles*/ !ProfileStore::list().isEmpty());
            return;
        case mmv::RestorePullStage::HasBundle:                // reached AND query succeeded AND a bundle exists -> apply
            cloud_->applyRemote(st.fileId, st.modifiedIso, st.remoteHash, [this](bool ok) {
                if (!onboardingChoiceIsTop()) return;         // navigated away mid-apply — drop
                if (!ok) { finishOnboardingRestore(/*restoreOk*/ false, /*remoteHasProfiles*/ false); return; }
                pullAndMergeProgress();                       // merge the per-item stores the bundle doesn't carry
                finishOnboardingRestore(/*restoreOk*/ true, /*remoteHasProfiles*/ !ProfileStore::list().isEmpty());
            });
            return;
        }
    });
}

// The single post-sign-in dispatch, driven entirely by the pure onboardingRoute so the decision can never drift
// from probe_onboarding. restoreOk folds "auth AND the pull both completed" into the router's signInOk leg: a
// pull/network failure after auth reuses the SAME ChoiceScreen row as a sign-in failure (token kept, retry), so no
// new enum value is introduced. Picker/Fresh is the router's post-sign-in Picker-vs-Fresh branch (remote populated
// vs. empty), already pinned in the probe. onboarding/done is set only on these two terminal outcomes.
void MainWindow::finishOnboardingRestore(bool restoreOk, bool remoteHasProfiles)
{
    switch (mmv::onboardingRoute(/*hasLocal*/ false, /*restorePicked*/ true, /*signInOk*/ restoreOk,
                                 remoteHasProfiles, /*signInAvailable*/ true))
    {
    case mmv::OnboardingRoute::Picker:                        // restored profiles pulled down -> pick one (as rows)
        Settings::setOnboardingDone(true);
        presentProfilePicker(/*mustChoose*/ true);
        break;
    case mmv::OnboardingRoute::Fresh:                         // signed in, empty cloud -> this device seeds it
        notify(tr("Nothing to restore yet — let's set up your library."));
        onboardingToFresh();
        break;
    case mmv::OnboardingRoute::ChoiceScreen:                  // pull/network failure after auth -> retry (token kept)
    default:
        presentOnboardingChoice();                            // onboarding/done stays FALSE
        notify(tr("Couldn't reach Drive — check your connection and try again."));
        break;
    }
}

void MainWindow::presentProfilePicker(bool mustChoose)
{
    clearPanelPageConns();            // settings-area BOUNDARY: no stale async listener may outlive this present
    themedPanelHost_->reset();        // fresh ROOT presentation (also drops any stale panel levels)
    themedPanelHost_->setStyle(settingsPanelStyle());
    NavOverlay::setThemeColors(settingsPanelStyle());  // the picker's own menus/confirms (pre-home) match the theme
    presentProfileList(mustChoose, /*replace*/ false);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}

void MainWindow::presentProfileList(bool mustChoose, bool replace)
{
    const QString title = mustChoose ? tr("Who's using My Media Vault?") : tr("Profiles");

    QVector<PanelRow> rows;
    for (const Profile& p : ProfileStore::list())
    {
        PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("profile:") + p.id;
        r.label = (p.icon.isEmpty() ? QStringLiteral("🙂") : p.icon) + QStringLiteral("   ") + p.name;
        rows << r;
    }
    { PanelRow s; s.kind = PanelRow::Separator; s.id = QStringLiteral("sep"); rows << s; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("new");
      r.label = tr("＋  Create New Profile"); rows << r; }

    auto onAct = [this, mustChoose](const QString& id, const QString&) {
        if (id == QStringLiteral("new"))            editProfilePanel(QString(), mustChoose); // nested picker
        else if (id.startsWith(QStringLiteral("profile:"))) profileRowMenu(id.mid(8), mustChoose);
    };
    auto onBack = [this, mustChoose] {
        if (mustChoose) quitConfirmFromStartup();  // startup: no escape — confirm quit (or re-present the list)
        else            openHome();                // switcher: Back keeps the current profile
    };

    if (replace && themedPanelIsTop(title))
        themedPanelHost_->replaceTop(title, rows, onAct, onBack);
    else
        themedPanelHost_->present(title, rows, onAct, onBack);
}

// The name/icon picker: New (id empty) or Edit an existing profile. A nested panel level (present) so Back pops
// back to the list. name is a TextField (host runs the OSK); icon is a Choice cycling the shared glyph set.
// Values held pending (shared_ptr) and committed only on Create/Save (classic parity — Back discards).
void MainWindow::editProfilePanel(const QString& id, bool mustChoose)
{
    const bool isNew = id.isEmpty();
    Profile target;
    if (!isNew)
        for (const Profile& p : ProfileStore::list())
            if (p.id == id) { target = p; break; }

    const QStringList icons = ProfileDialog::iconChoices();
    auto name = std::make_shared<QString>(target.name);
    auto icon = std::make_shared<QString>(target.icon.isEmpty() ? icons.value(0) : target.icon);

    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("name"); r.label = tr("Name");
      r.value = *name; rows << r; }
    { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("icon"); r.label = tr("Icon");
      r.value = *icon; r.options = icons; rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("save");
      r.label = isNew ? tr("Create Profile") : tr("Save"); rows << r; }

    themedPanelHost_->present(isNew ? tr("New Profile") : tr("Edit Profile"), rows,
        [this, id, isNew, name, icon, mustChoose](const QString& rid, const QString& val) {
            if (rid == QStringLiteral("name"))      *name = val;
            else if (rid == QStringLiteral("icon")) *icon = val;
            else if (rid == QStringLiteral("save"))
            {
                const QString entered = name->trimmed();
                if (entered.isEmpty()) { notify(tr("Please enter a name.")); return; }
                if (isNew)
                {
                    const Profile p = ProfileStore::add(entered, *icon);   // add + auto-select (classic parity)
                    chooseProfile(p.id);                                   // finish: setCurrent + openHome
                }
                else
                {
                    ProfileStore::update(id, entered, *icon);
                    // Pop this picker level then refresh the list in place (deferred: we're inside the picker's
                    // own onActivate, so let it unwind before mutating the panel stack).
                    QTimer::singleShot(0, this, [this, mustChoose] {
                        themedPanelHost_->handleBack();
                        presentProfileList(mustChoose, /*replace*/ true);
                    });
                }
            }
        },
        [] { /* nested: Back is a graph-level pop -> the host restores the list; nothing to run here */ });
}

void MainWindow::profileRowMenu(const QString& profileId, bool mustChoose)
{
    Profile target;
    for (const Profile& p : ProfileStore::list())
        if (p.id == profileId) { target = p; break; }
    if (target.id.isEmpty()) return;

    QStringList labels; QVector<int> acts;
    enum { A_Switch, A_Edit, A_Delete };
    labels << tr("Switch to %1").arg(target.name); acts << A_Switch;
    labels << tr("Edit");                          acts << A_Edit;
    if (ProfileStore::list().size() > 1) { labels << tr("Delete"); acts << A_Delete; } // never delete the last one

    auto* menu = new NavMenu(target.name, labels, [this, profileId, acts, mustChoose](int row) {
        if (row < 0 || row >= acts.size()) return;  // backed out
        switch (acts[row])
        {
        case A_Switch: chooseProfile(profileId);                break;  // setCurrent + openHome
        case A_Edit:   editProfilePanel(profileId, mustChoose); break;  // nested picker
        case A_Delete: confirmDeleteProfile(profileId, mustChoose); break;
        }
    }, this);
    menu->setNavGraph(themedPanelHost_ ? themedPanelHost_->navGraph() : nullptr);
}

void MainWindow::confirmDeleteProfile(const QString& profileId, bool mustChoose)
{
    Profile target;
    for (const Profile& p : ProfileStore::list())
        if (p.id == profileId) { target = p; break; }
    if (target.id.isEmpty()) return;

    const int choice = NavConfirm::ask(tr("Delete profile"),
        tr("Delete “%1”? Their recent list will be removed. This can't be undone.").arg(target.name),
        { tr("Cancel"), tr("Delete") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (choice == 1)
    {
        ProfileStore::remove(profileId);                 // if it was current, ProfileStore repoints current
        presentProfileList(mustChoose, /*replace*/ true); // refresh the list in place
    }
}

void MainWindow::chooseProfile(const QString& id)
{
    ProfileStore::setCurrent(id);
    ItemMarks::invalidate(); // drop the previous profile's marks cache NOW (no signal fires) so the fresh home
                             // filters/labels against the new profile's hidden/completion/tags, not the old one's
    ConsumptionStats::invalidate(); // likewise drop the previous profile's stats cache so accrual/display key off
                                    // the new profile from here on (stats are separately per-profile)
    openHome();   // render for the chosen profile (also the pre-home startup finish: builds the themed home now)
    // When this resolves the pre-home startup picker, showEvent already returned before its own
    // maybeOfferTvMode singleShot — offer TV mode now. Idempotent: it bails once prompted / outside its guards,
    // so scheduling it here on the runtime profile switcher too is harmless.
    QTimer::singleShot(0, this, [this] { maybeOfferTvMode(); });
}

// mustChoose Back: the classic must-choose dialog exited the app on close. Themed rendering: confirm the quit
// (the "you must pick a profile" contract), or re-present the list if the user chooses to keep choosing.
void MainWindow::quitConfirmFromStartup()
{
    // The onboarding choice screen borrows this same no-escape quit-confirm, but its Back must return to the CHOICE
    // screen (not the profile list) and its prompt is Restore-vs-new, not "choose a profile". onboardingDone() is the
    // discriminator MUST match showEvent's router guard (onboardingRoute): the choice screen is first-run ONLY, i.e.
    // no local profiles yet AND onboarding not marked done. An UPGRADE user (existing profiles, onboarding/done never
    // written -> false) must NEVER see the choice screen — profiles-empty is what makes it genuinely first-run, so a
    // Back on THEIR startup picker keeps today's create-picker/quit behavior, not the Restore-vs-new prompt.
    const bool onboarding = !Settings::onboardingDone() && ProfileStore::list().isEmpty();
    const int choice = NavConfirm::ask(tr("Quit My Media Vault?"),
        onboarding ? tr("Choose Restore or a new library to continue.")
                   : tr("You need to choose a profile to continue."),
        { onboarding ? tr("Go back") : tr("Choose a profile"), tr("Quit") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (choice == 1) { mwLog(QStringLiteral("quit: startup-picker quit-confirm")); QApplication::quit(); return; }
    if (onboarding) { presentOnboardingChoice(); return; }       // re-present the choice screen (Back popped its level)
    presentProfileList(/*mustChoose*/ true, /*replace*/ false);  // the level was popped by Back — present afresh
}
#endif // MMV_HAVE_QML

// A game whose platform is desktop Windows isn't an emulator ROM — we run it on the PC itself. The addon
// tags these with the platform name it was opened from (e.g. "PC (Windows)").
static bool isPcPlatform(const QString& hint)
{
    const QString h = hint.trimmed().toLower();
    return h == QStringLiteral("pc (windows)") || h == QStringLiteral("pc (microsoft windows)")
        || h == QStringLiteral("pc windows") || h == QStringLiteral("windows") || h == QStringLiteral("pc");
}

// Pull the real download filename (which carries the extension we need to run/open it) out of a
// Content-Disposition header; a debrid link's URL is just an opaque id, so this is where the name comes from.
static QString filenameFromContentDisposition(const QString& cd)
{
    // Prefer the RFC 5987 "filename*=UTF-8''…" form, then a plain filename="…" / filename=… .
    for (const QString& pat : { QStringLiteral("filename\\*=(?:UTF-8'')?([^;]+)"),
                                QStringLiteral("filename=\"?([^\";]+)") })
    {
        const QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(cd);
        if (m.hasMatch())
        {
            QString name = m.captured(1).trimmed();
            if (name.endsWith(QLatin1Char('"'))) name.chop(1);
            return QUrl::fromPercentEncoding(name.toUtf8()).trimmed(); // undo any RFC 5987 %-encoding
        }
    }
    return QString();
}

// An .exe that isn't the game or its main installer: an uninstaller, a bundled redistributable (VC++, DirectX,
// .NET/NDP, OpenAL), or a crash handler. Skipped when hunting for the installer or the game exe. base is the
// lowercased file name.
static bool isCruftExe(const QString& base)
{
    static const char* pats[] = { "unins", "redist", "vcredist", "vc_redist", "directx", "dxsetup",
        "dxwebsetup", "dotnet", "dotnetfx", "netfx", "ndp", "oalinst", "crashreport", "crashpad",
        "notification_helper", "uninstall" };
    for (const char* p : pats) if (base.contains(QLatin1String(p))) return true;
    return false;
}

// A downloaded/extracted .exe that's actually an installer (GOG "setup_<game>_….exe", Inno "setup.exe",
// *installer*, …) rather than the game itself - run it as setup, then locate the installed game.
static bool looksLikeInstaller(const QString& fileName)
{
    const QString b = QFileInfo(fileName).fileName().toLower();
    return b.startsWith(QStringLiteral("setup")) || b.startsWith(QStringLiteral("install"))
        || b.startsWith(QStringLiteral("gog_")) || b.contains(QStringLiteral("installer"))
        || b.contains(QStringLiteral("_setup"));
}

// Find the installer to launch inside an extracted repack: prefer setup.exe, else the largest .exe that
// isn't obvious cruft (an uninstaller or a bundled redistributable). Empty if there's no runnable .exe.
static QString findInstaller(const QString& dir)
{
    QString best;
    qint64 bestSize = -1;
    QDirIterator it(dir, QStringList{ QStringLiteral("*.exe") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString p = it.next();
        const QString base = QFileInfo(p).fileName().toLower();
        if (base == QStringLiteral("setup.exe")) return p; // the repack installer
        if (isCruftExe(base)) continue;
        const qint64 sz = QFileInfo(p).size();
        if (sz > bestSize) { bestSize = sz; best = p; }
    }
    return best;
}

// Find the actual *game* executable inside an installed/extracted PC game (as opposed to its installer).
// Skips setup.exe and the usual cruft (uninstallers, bundled redistributables, crash handlers); prefers an
// .exe whose name matches the game title, else the largest remaining one. Returns empty when there's no game
// exe to run yet - which distinguishes a portable/installed game (has one) from a repack that's been unpacked
// but whose setup.exe hasn't been run (only setup.exe present -> empty -> the caller runs the installer).
// titleMatchOnly restricts it to the title-named exe - used when scanning the shared games/pc root, where
// "largest exe anywhere" could belong to a different game.
static QString findGameExe(const QString& dir, const QString& title, bool titleMatchOnly = false)
{
    auto normAlnum = [](const QString& s) { QString o; for (const QChar c : s) if (c.isLetterOrNumber()) o += c.toLower(); return o; };
    const QString want = normAlnum(title);
    QString titleHit;  qint64 titleHitSize = -1;
    QString largest;   qint64 largestSize = -1;
    bool hasSetup = false;
    QDirIterator it(dir, QStringList{ QStringLiteral("*.exe") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString p = it.next();
        const QString base = QFileInfo(p).fileName().toLower();
        if (base == QStringLiteral("setup.exe")) { hasSetup = true; continue; }
        if (looksLikeInstaller(base)) { hasSetup = true; continue; } // e.g. setup_<game>_….exe (GOG) is setup
        if (isCruftExe(base)) continue;
        const qint64 sz = QFileInfo(p).size();
        const QString stem = normAlnum(QFileInfo(p).completeBaseName());
        if (!want.isEmpty() && !stem.isEmpty() && (stem == want || stem.contains(want) || want.contains(stem)))
        { if (sz > titleHitSize) { titleHitSize = sz; titleHit = p; } }
        if (sz > largestSize) { largestSize = sz; largest = p; }
    }
    if (!titleHit.isEmpty()) return titleHit; // the game exe is named after the game - strongest signal
    if (titleMatchOnly)      return QString();
    if (!hasSetup)           return largest;  // no installer in the tree => it's portable/installed already
    return QString();                         // an un-run installer repack: no game exe yet
}

// Extract a PC-game archive robustly. The bundled miniz/LZMA path handles typical .zip/.7z, but large repacks
// (ZIP64, RAR, or unusual methods - e.g. FitGirl) defeat it, so fall back to the system's bsdtar (Windows
// ships it as tar.exe) / 7z, which read virtually any archive. Synchronous: a repack extraction blocks, like
// the built-in path already does.
static bool extractPcArchive(const QString& archive, const QString& destDir, QString* err)
{
    if (ArchiveRom::extractAll(archive, destDir, err)) return true;
#ifdef Q_OS_IOS
    // No QProcess on iOS — the built-in ArchiveRom extractors are the only option.
    if (err && err->isEmpty()) *err = QObject::tr("no available extractor could read it");
    return false;
#else
    QDir().mkpath(destDir);
    struct Cmd { QString prog; QStringList args; };
    QList<Cmd> cmds;
#ifdef Q_OS_WIN
    cmds << Cmd{ QStringLiteral("C:/Windows/System32/tar.exe"), { QStringLiteral("-xf"), archive, QStringLiteral("-C"), destDir } };
    cmds << Cmd{ QStringLiteral("tar"),  { QStringLiteral("-xf"), archive, QStringLiteral("-C"), destDir } };
    cmds << Cmd{ QStringLiteral("7z"),   { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + destDir, archive } };
    cmds << Cmd{ QStringLiteral("7za"),  { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + destDir, archive } };
#else
    cmds << Cmd{ QStringLiteral("bsdtar"), { QStringLiteral("-xf"), archive, QStringLiteral("-C"), destDir } };
    cmds << Cmd{ QStringLiteral("7z"), { QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-o") + destDir, archive } };
#endif
    for (const Cmd& c : cmds)
    {
        QProcess p;
        p.start(c.prog, c.args);
        if (!p.waitForStarted(5000)) continue; // tool not installed
        p.waitForFinished(-1);                 // let a multi-GB repack finish
        if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        { mwLog(QStringLiteral("pcgame: extracted via %1").arg(QFileInfo(c.prog).fileName())); return true; }
    }
    if (err) *err = QObject::tr("no available extractor could read it");
    return false;
#endif
}

// Fuzzy match a title against a folder / DisplayName (lowercase, alnum-only, containment either way). Guards
// against tiny strings matching everything.
static QString pcNorm(const QString& s) { QString o; for (const QChar c : s) if (c.isLetterOrNumber()) o += c.toLower(); return o; }
static bool pcTitleMatch(const QString& a, const QString& b)
{
    const QString x = pcNorm(a), y = pcNorm(b);
    if (x.size() < 4 || y.size() < 4) return false;
    return x.contains(y) || y.contains(x);
}

// Does this exe plausibly belong to `title`? An exe outside our managed games/pc folder came from a matched
// external install (GOG/Program Files) and is trusted. One INSIDE games/pc must have the game's name in the
// exe or one of its parent folders - otherwise it's a stale cross-match (an old bug once stored another
// game's exe here), and we must not launch it for this title.
static bool pcExeBelongsTo(const QString& exe, const QString& title, const QString& pcRoot)
{
    const QString a = QFileInfo(exe).absoluteFilePath();
    if (!a.startsWith(pcRoot + QLatin1Char('/'), Qt::CaseInsensitive)) return true; // external install - trust it
    if (pcTitleMatch(QFileInfo(exe).completeBaseName(), title)) return true;
    QString dir = QFileInfo(exe).absolutePath();
    for (int i = 0; i < 5 && dir.length() > pcRoot.length(); ++i) // walk the extracted-repack nesting up to root
    {
        if (pcTitleMatch(QFileInfo(dir).fileName(), title)) return true;
        dir = QFileInfo(dir).absolutePath();
    }
    return false;
}

#ifdef Q_OS_WIN
// The Windows "Uninstall" registry roots where installers register their DisplayName + InstallLocation.
static QStringList uninstallRoots()
{
    return {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
}
// Snapshot every uninstall entry's full key path, so a before/after diff around an install reveals the
// entry the setup just created - and thus where it installed the game.
static QSet<QString> uninstallKeysSnapshot()
{
    QSet<QString> out;
    for (const QString& root : uninstallRoots())
    {
        QSettings s(root, QSettings::NativeFormat);
        for (const QString& g : s.childGroups()) out.insert(root + QLatin1Char('\\') + g);
    }
    return out;
}
// Where a specific uninstall entry says the app installed (InstallLocation, or the folder of DisplayIcon).
static QString installLocationOf(const QString& fullKey)
{
    QSettings s(fullKey, QSettings::NativeFormat);
    QString loc = s.value(QStringLiteral("InstallLocation")).toString().trimmed();
    if (loc.isEmpty())
    {
        const QString icon = s.value(QStringLiteral("DisplayIcon")).toString().trimmed();
        if (icon.contains(QStringLiteral(".exe"), Qt::CaseInsensitive))
            loc = QFileInfo(icon.split(QLatin1Char(',')).first()).absolutePath();
    }
    return loc;
}
#endif

namespace { QString safeFileName(const QString& title); } // defined in the anonymous namespace further below

// Download a PC (Windows) game and hand it to the OS to run/install. The file (a repack installer, a portable
// .exe, or an archive) is streamed to <data>/games/pc so multi-GB downloads don't sit in memory; its real
// name — and the extension we need to launch it — comes from Content-Disposition since the debrid URL is an
// opaque id. On completion we open it with the OS default (installer/exe runs; archive opens); if we couldn't
// determine the type, we just reveal it in its folder.
void MainWindow::openPcGame(const MediaItem& item)
{
    // Already have it on disk? Launch the installed game directly (or re-run a pending installer / ask where
    // it went) instead of downloading again.
    if (tryLaunchInstalledPcGame(item.id, item.title, item.thumbnailUrl)) return;

    const QString dir = AppPaths::dataDir() + QStringLiteral("/games/pc");
    QDir().mkpath(dir);
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);

    const QString partPath = dir + QStringLiteral("/") + safeFileName(item.title) + QStringLiteral(".download");
    auto part = std::make_shared<QFile>(partPath);
    if (!part->open(QIODevice::WriteOnly))
    {
        notify(tr("Couldn't start downloading “%1”.").arg(item.title), kFeedbackLong);
        return;
    }

    // Downloading for keeps: save the catalog metadata + poster locally so the game's shelf entry and
    // info render offline (MetaCache; the detail card is added by the Home info page when it was shown).
    MetaCache::saveItem(item);
    MetaCache::cacheImage(MetaCache::keyFor(item), QStringLiteral("thumb"), item.thumbnailUrl);

    mwLog(QStringLiteral("pcgame: download \"%1\" from %2").arg(item.title, logSafeUrl(item.url)));
    QNetworkRequest rq{ QUrl(item.url) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = docNam_->get(rq);

    auto fileName = std::make_shared<QString>(); // real download name, from Content-Disposition
    statusBar()->showMessage(tr("Downloading “%1”…").arg(item.title));
    notify(tr("Downloading “%1”…").arg(item.title), 0); // sticky window-level notice, kept up through the whole
                                                        // download → extract → launch so there's never a blank gap

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, fileName] {
        if (fileName->isEmpty())
            if (const QByteArray cd = reply->rawHeader("Content-Disposition"); !cd.isEmpty())
                *fileName = filenameFromContentDisposition(QString::fromUtf8(cd));
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, part] { part->write(reply->readAll()); });

    auto humanMB = [](qint64 b) { return QString::number(b / 1048576.0, 'f', 1); };
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, title = item.title, humanMB, lastPct = -1](qint64 rec, qint64 total) mutable {
        if (total <= 0) return;
        const int pct = static_cast<int>(rec * 100 / total);
        if (pct == lastPct) return;
        lastPct = pct;
        const QString msg = tr("Downloading “%1”… %2%  (%3 / %4 MB)")
                                .arg(title).arg(pct).arg(humanMB(rec), humanMB(total));
        statusBar()->showMessage(msg);
        notify(msg, 0);
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, part, partPath, fileName, dir, item] {
        reply->deleteLater();
        part->write(reply->readAll());
        part->close();
        if (reply->error() != QNetworkReply::NoError)
        {
            mwLog(QStringLiteral("pcgame: download failed \"%1\": %2").arg(item.title, reply->errorString()));
            const QString e = tr("Couldn't download “%1”: %2").arg(item.title, reply->errorString());
            statusBar()->showMessage(e, kFeedbackLong);
            notify(e, kFeedbackLong);
            part->remove();
            return;
        }

        const QString name = fileName->isEmpty() ? safeFileName(item.title) : safeFileName(*fileName);
        const QString dest = dir + QStringLiteral("/") + name;
        QFile::remove(dest);
        if (!QFile::rename(partPath, dest))
        {
            statusBar()->showMessage(tr("Couldn't save “%1”.").arg(item.title), kFeedbackLong);
            notify(tr("Couldn't save “%1”.").arg(item.title), kFeedbackLong);
            QFile::remove(partPath);
            return;
        }
        mwLog(QStringLiteral("pcgame: saved \"%1\" -> %2").arg(item.title, QFileInfo(dest).fileName()));

        // An archive (the whole-torrent .zip, or a .7z) is a repack: extract it and launch its installer.
        if (ArchiveRom::isArchive(dest))
        {
            const QString gameDir = dir + QStringLiteral("/") + QFileInfo(dest).completeBaseName();
            statusBar()->showMessage(tr("Extracting “%1”…").arg(item.title));
            notify(tr("Extracting “%1”… this can take a while for a large game.").arg(item.title), 0);
            QString aerr;
            mwLog(QStringLiteral("pcgame: extracting %1").arg(QFileInfo(dest).fileName()));
            if (!extractPcArchive(dest, gameDir, &aerr))
            {
                mwLog(QStringLiteral("pcgame: extract failed: %1").arg(aerr));
                const QString e = tr("Couldn't extract “%1”: %2").arg(item.title, aerr);
                statusBar()->showMessage(e, kFeedbackLong);
                notify(e, kFeedbackLong);
                RecentStore::add({ dest, item.title, QStringLiteral("game"), item.thumbnailUrl, item.id });
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir)); // reveal the archive so the user can act
                return;
            }
            QFile::remove(dest); // unpacked now; drop the archive
            PcGameStore::setDir(item.id, gameDir); // remember where it lives so we never re-download it

            // A portable game runs directly: its exe is named after the game, or ships its runtime DLLs right
            // beside it. Anything else in the extracted repack - a setup.exe, or an installer .exe sitting in a
            // folder - is run as the installer, after which we locate the game it installs.
            const QString game = findGameExe(gameDir, item.title);
            const bool portable = !game.isEmpty()
                && (pcTitleMatch(QFileInfo(game).completeBaseName(), item.title)
                    || !QDir(QFileInfo(game).absolutePath()).entryList(QStringList{ QStringLiteral("*.dll") }, QDir::Files).isEmpty());
            if (portable)
            {
                PcGameStore::setExe(item.id, game);
                launchPcExe(game, item.id, item.title, item.thumbnailUrl);
                return;
            }
            QString installer = findInstaller(gameDir);
            if (installer.isEmpty()) installer = game; // the only runnable exe we found - treat it as the installer
            if (!installer.isEmpty())
            {
                runPcInstaller(installer, item.id, item.title, item.thumbnailUrl, gameDir);
            }
            else
            {
                RecentStore::add({ gameDir, item.title, QStringLiteral("pcgame"), item.thumbnailUrl, item.id });
                notify(tr("Extracted “%1” — opening its folder.").arg(item.title), 6000);
                QDesktopServices::openUrl(QUrl::fromLocalFile(gameDir));
            }
            return;
        }

        // A direct download. An installer .exe (GOG "setup_<game>_….exe", Inno "setup.exe", …) is run as
        // setup, then we locate the installed game. A portable/standalone game .exe IS the game - remember it
        // so re-opening runs it straight away. Anything else opens with the OS default.
        if (QFileInfo(dest).suffix().toLower() == QStringLiteral("exe"))
        {
            if (looksLikeInstaller(dest)) { runPcInstaller(dest, item.id, item.title, item.thumbnailUrl, dir); return; }
            PcGameStore::setExe(item.id, dest);
            launchPcExe(dest, item.id, item.title, item.thumbnailUrl);
            return;
        }
        RecentStore::add({ dest, item.title, QStringLiteral("pcgame"), item.thumbnailUrl, item.id });
        notify(tr("Opening “%1”…").arg(item.title), 5000);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
    });
}

// Find where a PC game landed after its installer ran - wherever the user pointed it.
QString MainWindow::locateInstalledGameExe(const QString& title, const QString& gameDir,
                                           const QStringList& extraLocations)
{
    QStringList locations = extraLocations; // e.g. InstallLocations captured by the install-time registry diff
#ifdef Q_OS_WIN
    // 1) Any uninstall entry whose name or install folder matches the game -> its InstallLocation. Catches
    //    installers (GOG/Inno) that register after our process-watch already fired.
    for (const QString& root : uninstallRoots())
    {
        QSettings s(root, QSettings::NativeFormat);
        for (const QString& g : s.childGroups())
        {
            const QString key = root + QLatin1Char('\\') + g;
            QSettings e(key, QSettings::NativeFormat);
            const QString loc = installLocationOf(key);
            if (loc.isEmpty()) continue;
            if (pcTitleMatch(e.value(QStringLiteral("DisplayName")).toString(), title)
                || pcTitleMatch(QFileInfo(loc).fileName(), title))
                locations << loc;
        }
    }
    // 2) A subfolder named like the game under a common install root (GOG's default is C:\GOG Games\<Title>).
    QStringList roots{ QStringLiteral("C:/GOG Games"), QStringLiteral("C:/Games"), QStringLiteral("C:/GOG") };
    for (const char* ev : { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" })
        if (const QString p = qEnvironmentVariable(ev); !p.isEmpty()) roots << p;
    for (const QString& root : roots)
    {
        QDir d(root);
        if (!d.exists()) continue;
        for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (pcTitleMatch(sub, title)) locations << d.absoluteFilePath(sub);
    }
#endif
    // 3) Our own extracted folder (this game's own dir).
    if (!gameDir.isEmpty()) locations << gameDir;

    // These candidates are all THIS game's own folders (a matched install location / its extracted dir), so a
    // "largest game exe" fallback is safe. But never the shared games/pc root - it holds OTHER games, and the
    // largest exe there would be some unrelated game (that's how "Warhammer" once launched Rift Wizard).
    const QString pcRoot = QDir(AppPaths::dataDir() + QStringLiteral("/games/pc")).absolutePath();
    QStringList specific;
    for (const QString& loc : locations)
        if (QDir(loc).absolutePath() != pcRoot) specific << loc;

    // Title-named exe first (skips bundled redistributables), then the largest game exe - within this game's
    // own folders only.
    for (const QString& loc : specific) { const QString e = findGameExe(loc, title, /*titleMatchOnly*/ true); if (!e.isEmpty()) return e; }
    for (const QString& loc : specific) { const QString e = findGameExe(loc, title, false); if (!e.isEmpty()) return e; }
    // Shared games/pc root: only ever a title-named match (portable repacks / pre-store installs), never the
    // arbitrary largest exe.
    return findGameExe(pcRoot, title, /*titleMatchOnly*/ true);
}

// Run a resolved local-game exe and record it in Recent so re-opening from Home relaunches this exact
// executable. `kind` ("pcgame" default, or "goggame") is the Recent routing kind — the launch MECHANICS below
// are identical for both (GOG games are DRM-free processes, just like a downloaded PC repack). The kind-vs-
// mechanics split: a GOG game records the "goggame" kind (so it groups on the GOG console and re-opens via the
// GogGame dispatch) but rides this pcgame monitor path; and it is NOT added to the PC-console Downloads (it's
// an installed registry game, not a downloaded repack), so we skip that store write for non-pcgame kinds.
void MainWindow::launchPcExe(const QString& exe, const QString& id, const QString& title, const QString& thumb,
                            const QString& kind)
{
    mwLog(QStringLiteral("%1: launch \"%2\"").arg(kind, QFileInfo(exe).fileName()));
    notify(tr("Launching “%1”…").arg(title), 5000);
    RecentStore::add({ exe, title, kind, thumb, id });
    if (kind == QStringLiteral("pcgame"))
        DownloadsStore::add({ exe, title, kind, thumb, id, QStringLiteral("pc") });
    const QString playId = PlayStats::identity(id, exe);
    PlayStats::markPlayed(playId); // last-played now; total time is banked when the process exits (Windows path)
    // Run with the working directory set to the game's own folder - most games load their DLLs, Content and
    // config relative to the CWD and silently fail to start if it's ours.
    const QString workDir = QFileInfo(exe).absolutePath();

#ifdef Q_OS_WIN
    // Launch via the shell (elevates if the game asks) with a process handle, and watch it briefly: a game
    // that closes within a few seconds didn't really open (missing redistributables, or the wrong exe).
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = nullptr;
    const std::wstring wFile = exe.toStdWString();
    const std::wstring wDir  = workDir.toStdWString();
    sei.lpFile = wFile.c_str();
    sei.lpDirectory = wDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess)
    {
        HANDLE h = sei.hProcess;
        auto done    = std::make_shared<bool>(false); // the notifier has fired and been handled
        auto graceOk = std::make_shared<bool>(false); // survived the 9s "did it actually open" grace window
        const qint64 startSecs = QDateTime::currentSecsSinceEpoch();
        auto* watch = new QWinEventNotifier(h, this);
        auto* timer = new QTimer(this); timer->setSingleShot(true);
        auto cleanup = [watch, timer, h] { watch->setEnabled(false); watch->deleteLater(); timer->stop(); timer->deleteLater(); CloseHandle(h); };
        connect(watch, &QWinEventNotifier::activated, this,
                [this, done, graceOk, cleanup, id, title, thumb, exe, playId, startSecs] {
            if (*done) return; *done = true; cleanup();
            if (!*graceOk) // exited before the grace period -> it didn't really open
            {
                mwLog(QStringLiteral("pcgame: \"%1\" closed right after launching").arg(title));
                onPcGameFailedToOpen(id, title, thumb, exe);
                return;
            }
            const qint64 secs = QDateTime::currentSecsSinceEpoch() - startSecs; // a real play session ended
            PlayStats::addSession(playId, secs);
            mwLog(QStringLiteral("pcgame: \"%1\" exited after %2s of play").arg(title).arg(secs));
        });
        // Keep watching after the grace window (don't clean up) so we can time the whole session, not just
        // confirm it opened. A game whose launcher forks and exits early undercounts; that's inherent here.
        connect(timer, &QTimer::timeout, this, [done, graceOk] { if (*done) return; *graceOk = true; });
        timer->start(9000);
        return;
    }
    mwLog(QStringLiteral("pcgame: ShellExecuteEx couldn't launch \"%1\" — falling back").arg(QFileInfo(exe).fileName()));
#endif
#ifdef Q_OS_IOS
    // No QProcess on iOS; PC games can't launch here anyway (desktop-only feature).
    Q_UNUSED(workDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(exe));
#else
    if (!QProcess::startDetached(exe, QStringList(), workDir))
    {
        mwLog(QStringLiteral("pcgame: startDetached failed for \"%1\" — falling back to shell open").arg(QFileInfo(exe).fileName()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(exe));
    }
#endif
}

// The launched game closed almost immediately: tell the user (it usually means missing redistributables or the
// wrong exe) and offer to open its folder or point us at a different exe.
void MainWindow::onPcGameFailedToOpen(const QString& id, const QString& title, const QString& thumb, const QString& exe)
{
    const QString folder = QFileInfo(exe).absolutePath();
    const int choice = NavConfirm::ask(
        tr("“%1” didn't stay open").arg(title),
        tr("“%1” closed right after it launched.\n\nThis usually means it's missing components — install the "
           "redistributables in its folder (often a _Redist / _CommonRedist folder) — or the launched file "
           "was the wrong one.").arg(title),
        { tr("Open game folder"), tr("Choose a different .exe"), tr("Close") },
        /*focusIndex=*/0, /*cancelIndex=*/2, this);
    if (choice == 0)
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    else if (choice == 1)
    {
        const QString picked = QFileDialog::getOpenFileName(this,
            tr("Pick the .exe that launches “%1”").arg(title), folder, tr("Programs (*.exe)"));
        if (!picked.isEmpty()) { PcGameStore::setExe(id, picked); launchPcExe(picked, id, title, thumb); }
    }
}

// Run a PC game's setup, watch the installer process, and when it closes find + launch the installed game.
void MainWindow::runPcInstaller(const QString& installer, const QString& id, const QString& title,
                                const QString& thumb, const QString& gameDir)
{
    PcGameStore::setInstallerRan(id, true);
    RecentStore::add({ installer, title, QStringLiteral("pcgame"), thumb, id });
    mwLog(QStringLiteral("pcgame: running installer %1").arg(QFileInfo(installer).fileName()));

#ifdef Q_OS_WIN
    const QSet<QString> before = uninstallKeysSnapshot();
    // ShellExecuteEx runs setup through the shell so a manifest asking for elevation gets its UAC prompt
    // (QProcess can't elevate), and SEE_MASK_NOCLOSEPROCESS hands us a handle we can watch to completion.
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = nullptr;                       // default verb; the exe's manifest decides whether to elevate
    const std::wstring wFile = installer.toStdWString();
    const std::wstring wDir  = QFileInfo(installer).absolutePath().toStdWString();
    sei.lpFile = wFile.c_str();
    sei.lpDirectory = wDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess)
    {
        HANDLE h = sei.hProcess;
        // A modal "installing" dialog blocks interaction with the app until setup closes (the separate setup
        // window is a different app, so it stays usable). App-modal, not exec()-blocking, so the event loop
        // keeps running and the process watcher can fire.
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(tr("Installing “%1”").arg(title));
        dlg->setModal(true);
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->setWindowFlags((dlg->windowFlags() & ~Qt::WindowCloseButtonHint) | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        auto* lay = new QVBoxLayout(dlg);
        auto* msg = new QLabel(tr("Installing “%1”…\n\nComplete the setup window that just opened. The app will "
                                  "continue automatically when setup closes.").arg(title), dlg);
        msg->setWordWrap(true);
        auto* bar = new QProgressBar(dlg); bar->setRange(0, 0); // indeterminate "busy" bar
        auto* done = new QPushButton(tr("I've finished setup"), dlg); // safety valve if the watcher can't tell
        lay->addWidget(msg); lay->addWidget(bar); lay->addWidget(done, 0, Qt::AlignRight);
        dlg->setMinimumWidth(440);

        auto* watch = new QWinEventNotifier(h, this);
        auto guard = std::make_shared<bool>(false);
        auto complete = [this, dlg, watch, h, id, title, thumb, gameDir, installer, before, guard]() {
            if (*guard) return; // process-exit and the button can both fire; run the tail exactly once
            *guard = true;
            watch->setEnabled(false); watch->deleteLater();
            CloseHandle(h);
            // Uninstall entries that appeared during setup point at where the game landed.
            QStringList locs;
            const QSet<QString> after = uninstallKeysSnapshot();
            for (const QString& k : after)
                if (!before.contains(k)) { const QString l = installLocationOf(k); if (!l.isEmpty()) locs << l; }
            mwLog(QStringLiteral("pcgame: installer for \"%1\" done; %2 new install location(s)").arg(title).arg(locs.size()));
            dlg->accept(); dlg->deleteLater();
            onPcInstallerFinished(id, title, thumb, gameDir, installer, locs);
        };
        connect(watch, &QWinEventNotifier::activated, this, [complete]() { complete(); });
        connect(done, &QPushButton::clicked, this, [complete]() { complete(); });
        dlg->show(); // non-blocking; app-modal locks the main window while the watcher stays live
        return;
    }
    mwLog(QStringLiteral("pcgame: ShellExecuteEx couldn't monitor the installer — opening without monitoring"));
#endif
    // Fallback (no handle / non-Windows): open it; we can't detect completion, so the next open resolves the
    // installed exe.
    notify(tr("Installing “%1”… run its setup, then open it again to play.").arg(title), 9000);
    QDesktopServices::openUrl(QUrl::fromLocalFile(installer));
}

// Delete a PC game's install media (the downloaded installer .exe and/or the extracted repack folder) once
// the game is installed elsewhere. Strictly bounded: only touches paths inside <data>/games/pc, never the
// games/pc root itself, and never the folder the game actually installed into (a portable game).
void MainWindow::cleanupPcInstallMedia(const QString& installer, const QString& gameDir, const QString& installedExe)
{
    const QString pcRoot = QDir(AppPaths::dataDir() + QStringLiteral("/games/pc")).absolutePath();
    const QString exePath = QFileInfo(installedExe).absoluteFilePath();
    auto insidePcRoot = [&](const QString& p) {
        const QString a = QFileInfo(p).absoluteFilePath();
        return a == pcRoot || a.startsWith(pcRoot + QLatin1Char('/'), Qt::CaseInsensitive);
    };
    auto exeUnder = [&](const QString& folder) {
        const QString f = QDir(folder).absolutePath();
        return exePath == f || exePath.startsWith(f + QLatin1Char('/'), Qt::CaseInsensitive);
    };

    // The standalone installer file (e.g. games/pc/setup_<game>_….exe) - not needed once installed.
    if (!installer.isEmpty() && insidePcRoot(installer)
        && QFileInfo(installer).absoluteFilePath() != exePath && QFileInfo::exists(installer))
    {
        mwLog(QStringLiteral("pcgame: removing installer %1").arg(QFileInfo(installer).fileName()));
        QFile::remove(installer);
    }
    // The extracted repack folder - only if it's a real subfolder of games/pc (not the root) and the game
    // didn't install into it (portable). removeRecursively frees the whole unpacked repack.
    const QString gd = QDir(gameDir).absolutePath();
    if (!gameDir.isEmpty() && gd != pcRoot && insidePcRoot(gd) && QDir(gd).exists() && !exeUnder(gd))
    {
        mwLog(QStringLiteral("pcgame: removing extracted repack %1").arg(QFileInfo(gd).fileName()));
        QDir(gd).removeRecursively();
    }
}

// The installer closed: find the game it installed (its registered location, or our folder) and launch it.
void MainWindow::onPcInstallerFinished(const QString& id, const QString& title, const QString& thumb,
                                       const QString& gameDir, const QString& installer,
                                       const QStringList& installLocations)
{
    notify(tr("Setup finished — locating “%1”…").arg(title), 0);
    const QString exe = locateInstalledGameExe(title, gameDir, installLocations);
    if (!exe.isEmpty())
    {
        PcGameStore::setDir(id, QFileInfo(exe).absolutePath());
        PcGameStore::setExe(id, exe);
        mwLog(QStringLiteral("pcgame: post-install exe for \"%1\" -> %2").arg(title, exe));
        cleanupPcInstallMedia(installer, gameDir, exe); // reclaim space: the installer/repack is spent now
        launchPcExe(exe, id, title, thumb);
        return;
    }
    mwLog(QStringLiteral("pcgame: post-install couldn't locate \"%1\" — asking the user").arg(title));
    const QString start = !installLocations.isEmpty() ? installLocations.first()
                          : (gameDir.isEmpty() ? AppPaths::dataDir() + QStringLiteral("/games/pc") : gameDir);
    promptLocatePcExe(id, title, thumb, start);
}

// Try to open an already-downloaded PC game without fetching it again. Returns true when it handled the open.
bool MainWindow::tryLaunchInstalledPcGame(const QString& id, const QString& title, const QString& thumb)
{
    PcGameStore::Entry e = PcGameStore::get(id);
    const QString pcRoot = QDir(AppPaths::dataDir() + QStringLiteral("/games/pc")).absolutePath();
    // A stored exe/dir that points at ANOTHER game (a stale cross-match from an old locate bug - e.g. Warhammer
    // once mapped to Rift Wizard's exe): forget it so we re-resolve/download instead of launching the wrong game.
    if (!e.exe.isEmpty() && QFileInfo::exists(e.exe) && !looksLikeInstaller(e.exe) && !pcExeBelongsTo(e.exe, title, pcRoot))
    {
        mwLog(QStringLiteral("pcgame: stored exe %1 doesn't match \"%2\" — clearing stale mapping")
                  .arg(QFileInfo(e.exe).fileName(), title));
        PcGameStore::clear(id);
        e = PcGameStore::Entry{}; // continue as if never downloaded
    }

    // 1) We already know the game exe - just run it. (Ignore a stored path that's actually an installer, from
    //    before installer-named .exes were detected: fall through to re-locate the real game below.)
    if (!e.exe.isEmpty() && QFileInfo::exists(e.exe) && !looksLikeInstaller(e.exe))
    { launchPcExe(e.exe, id, title, thumb); return true; }

    // 2) Find it wherever it installed - the installer's registered location, C:\GOG Games\<Title>, Program
    //    Files, our extracted folder, etc. Handles installs outside our folder and pre-store installs.
    const QString found = locateInstalledGameExe(title, e.dir);
    if (!found.isEmpty())
    {
        PcGameStore::setDir(id, QFileInfo(found).absolutePath());
        PcGameStore::setExe(id, found);
        mwLog(QStringLiteral("pcgame: located \"%1\" -> %2").arg(title, found));
        launchPcExe(found, id, title, thumb);
        return true;
    }

    // 3) Not installed anywhere yet. If we downloaded it but haven't run setup, do that now (the next open
    //    finds the installed exe). Otherwise, if we have a folder, ask the user to point us at the exe once;
    //    if we have nothing local at all, let the caller download it fresh.
    if (!e.dir.isEmpty() && QFileInfo::exists(e.dir))
    {
        if (!e.installerRan)
        {
            const QString inst = findInstaller(e.dir);
            if (!inst.isEmpty()) { runPcInstaller(inst, id, title, thumb, e.dir); return true; }
        }
        promptLocatePcExe(id, title, thumb, e.dir);
        return true;
    }
    return false; // never downloaded and not found anywhere -> caller downloads it
}

// Forget a PC game entirely and reclaim its space, so re-opening starts a fresh download/install.
void MainWindow::forgetPcGame(const QString& id, const QString& title)
{
    const QString pcRoot = QDir(AppPaths::dataDir() + QStringLiteral("/games/pc")).absolutePath();
    const PcGameStore::Entry e = PcGameStore::get(id);
    auto underPcRoot = [&](const QString& p) {
        return QFileInfo(p).absoluteFilePath().startsWith(pcRoot + QLatin1Char('/'), Qt::CaseInsensitive);
    };
    // Delete the leftover installer file(s) this game recorded (only inside games/pc).
    for (const RecentItem& r : RecentStore::list())
        if (r.key == id && underPcRoot(r.path) && QFileInfo(r.path).isFile())
            QFile::remove(r.path);
    // Delete the extracted repack folder (a subfolder of games/pc, never the root itself).
    const QString gd = QDir(e.dir).absolutePath();
    if (!e.dir.isEmpty() && gd != pcRoot && underPcRoot(e.dir) && QDir(gd).exists())
        QDir(gd).removeRecursively();
    // Forget it everywhere.
    PcGameStore::clear(id);
    RecentStore::remove(id);
    DownloadsStore::remove(id);
    MetaCache::remove(id); // drop its offline metadata/artwork bundle too
    mwLog(QStringLiteral("pcgame: forgot \"%1\" (no exe chosen) — cleared store/recent/downloads + media").arg(title));
}

// Ask the user which .exe launches this game (for installers that put the game outside our folder). Remembers
// the choice; if they cancel, the game is forgotten (removed from Recent/Downloads) so re-opening reinstalls.
void MainWindow::promptLocatePcExe(const QString& id, const QString& title, const QString& thumb, const QString& startDir)
{
    const QString start = QFileInfo::exists(startDir) ? startDir
                          : (AppPaths::dataDir() + QStringLiteral("/games/pc"));
    const QString exe = QFileDialog::getOpenFileName(this,
        tr("Where did “%1” install? Pick its game .exe").arg(title), start, tr("Programs (*.exe)"));
    if (exe.isEmpty())
    {
        forgetPcGame(id, title);
        notify(tr("Cancelled — “%1” was removed. Open it again to download and install it.").arg(title), kFeedbackLong);
        return;
    }
    PcGameStore::setExe(id, exe);
    launchPcExe(exe, id, title, thumb);
}

// Re-open a PC game from a Recent entry (kind "pcgame"): use the store's resolved exe / install folder, and
// only fall back to the exact path Recent recorded if the store was lost.
void MainWindow::relaunchPcGame(const QString& id, const QString& title, const QString& thumb, const QString& recordedPath)
{
    if (tryLaunchInstalledPcGame(id, title, thumb)) return;
    if (!recordedPath.isEmpty() && QFileInfo::exists(recordedPath)) { launchPcExe(recordedPath, id, title, thumb); return; }
    notify(tr("Couldn't find “%1”. Open it from the library to download it again.").arg(title), kFeedbackLong);
}

// Re-open a GOG game from a Recent entry (kind "goggame", key "gog:<id>"): prefer the registry's current exe
// (survives a game update/move), then the exact exe the Recent recorded, then give up. Uses the "goggame" kind
// so re-recording keeps it on the GOG console (not the PC console).
void MainWindow::relaunchGogGame(const QString& id, const QString& title, const QString& thumb, const QString& recordedPath)
{
    const QString gogId = id.startsWith(QStringLiteral("gog:")) ? id.mid(QStringLiteral("gog:").size()) : id;
    for (const GogGame& g : GogLibrary::installedGames())
        if (g.id == gogId && !g.exe.isEmpty() && QFileInfo::exists(g.exe))
        { launchPcExe(g.exe, id, title, thumb, QStringLiteral("goggame")); return; }
    if (!recordedPath.isEmpty() && QFileInfo::exists(recordedPath))
    { launchPcExe(recordedPath, id, title, thumb, QStringLiteral("goggame")); return; }
    notify(tr("Couldn't find “%1”. It may have been uninstalled from GOG.").arg(title), kFeedbackLong);
}

void MainWindow::startScrobble(const QString& imdbStreamId)
{
    stopScrobble();  // close out any previous video first
    if (imdbStreamId.isEmpty() || !TraktClient::connected()) return;
    scrobbleImdb_ = imdbStreamId;
    trakt_->scrobbleStart(imdbStreamId, 0.0);
}

void MainWindow::stopScrobble()
{
    if (scrobbleImdb_.isEmpty()) return;
    const double pct = duration_ > 0.0 ? qBound(0.0, session_->position() / duration_ * 100.0, 100.0) : 0.0;
    trakt_->scrobbleStop(scrobbleImdb_, pct); // Trakt marks it watched when the stop is past ~80%
    scrobbleImdb_.clear();
}

// Decide whether the video about to play should get an auto-downloaded subtitle, and stash the match hints
// for the MpvWidget::fileLoaded handler. Only movies/episodes qualify, and only when the feature is enabled
// and configured. Always resets subCtx_ first, so a prior video's context can't leak into an ineligible open.
void MainWindow::armSubtitleFetch(const MediaItem& item)
{
    subCtx_ = {};
    const QString t = item.type.toLower();
    const bool eligible = t.isEmpty() || t == QStringLiteral("movie") || t == QStringLiteral("series")
                          || t == QStringLiteral("episode") || t == QStringLiteral("video");
    if (!eligible) return;
    // Keep the match hints for the current video so the subtitle menu's manual "download" works too; only arm
    // the automatic-on-load fetch when the feature is enabled, configured, and we have something to match on.
    subCtx_.imdbStreamId = item.imdbStreamId;
    subCtx_.title = item.title;
    subCtx_.active = Settings::subtitlesOnByDefault() && SubtitleFetcher::configured()
                     && !(item.imdbStreamId.isEmpty() && item.title.isEmpty());
}

// The cast button's popup: kick off discovery, then list found Chromecast / DLNA devices. Selecting one hands
// it the current stream URL and stops local playback; a "Stop casting" row appears while a cast is active.
void MainWindow::showCastMenu(QWidget* anchor)
{
    revealMediaControls();
    castMgr_->startDiscovery(); // refresh the device list each time the menu opens

    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background:#1c1c22; color:#e8e8e8; border:1px solid rgba(255,255,255,0.14); padding:6px; }"
        "QMenu::item { padding:7px 26px; border-radius:6px; } QMenu::item:selected { background:rgba(90,140,255,0.55); }"
        "QMenu::item:disabled { color:#888; }"
        "QMenu::separator { height:1px; background:rgba(255,255,255,0.12); margin:6px 8px; }"));

    if (castMgr_->isCasting())
    {
        QAction* s = menu.addAction(tr("■  Stop casting to %1").arg(castMgr_->currentDeviceName()));
        connect(s, &QAction::triggered, this, [this] { castMgr_->stopCasting(); });
        menu.addSeparator();
    }

    if (castUrl_.isEmpty() || !(castUrl_.startsWith(QStringLiteral("http"))))
    {
        QAction* n = menu.addAction(tr("Nothing castable is playing"));
        n->setEnabled(false);
    }

    const QList<CastDevice> devs = castMgr_->devices();
    if (devs.isEmpty())
    {
        QAction* searching = menu.addAction(tr("Searching for devices…"));
        searching->setEnabled(false);
    }
    for (const CastDevice& d : devs)
    {
        const QString icon = d.type == CastDevice::Chromecast ? QStringLiteral("📺  ") : QStringLiteral("📡  ");
        QAction* a = menu.addAction(icon + d.name);
        const bool ready = !castUrl_.isEmpty() && castUrl_.startsWith(QStringLiteral("http"));
        a->setEnabled(ready);
        const CastDevice dev = d;
        connect(a, &QAction::triggered, this, [this, dev] {
            player_->stop();                         // hand playback to the device; free the local decoder
            castMgr_->cast(dev, castUrl_, castTitle_, castMime_);
        });
    }

    const QSize sh = menu.sizeHint();
    menu.exec(anchor->mapToGlobal(QPoint(0, -sh.height() - 6)));
}

// The preset playback rates the speed button / [ ] keys step through.
static const double kSpeedPresets[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };

void MainWindow::setPlaybackSpeed(double s)
{
    player_->setSpeed(s);
    if (speedBtn_) speedBtn_->setText(QString::number(s, 'g', 3) + QStringLiteral("×")); // 1×, 1.25×, 0.75×
}

void MainWindow::cyclePlaybackSpeed(int dir)
{
    const int n = int(sizeof(kSpeedPresets) / sizeof(double));
    const double cur = player_->speed();
    // Find the nearest preset to the current speed, then step from there.
    int idx = 0; double best = 1e9;
    for (int i = 0; i < n; ++i) { const double d = qAbs(kSpeedPresets[i] - cur); if (d < best) { best = d; idx = i; } }
    idx = qBound(0, idx + (dir >= 0 ? 1 : -1), n - 1);
    setPlaybackSpeed(kSpeedPresets[idx]);
}

void MainWindow::captureVideoScreenshot()
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/screenshots");
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/video-%2.png")
        .arg(dir, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    player_->takeScreenshot(path);                 // async in mpv; the file lands a moment later
    notify(tr("Screenshot saved to the screenshots folder."), 3000);
}

void MainWindow::hideSubtitleMenu()
{
    if (!subOverlay_) return;
    subOverlay_->hide();
    subOverlay_->deleteLater();
    subOverlay_ = nullptr;
    subLeftCol_.clear();
    subRightCol_.clear();
    revealMediaControls();
}

// The subtitle button opens a full-player overlay panel (Stremio-style) rather than a dropdown: a dimmed
// scrim over the whole video with a centred card holding the track picker, sync/size adjusters, and the
// load/download sources. Clicking the scrim, the ✕, or pressing Esc/Back closes it; it's arrow-navigable.
// A signed millisecond readout for a sync offset (seconds): "+150 ms", "0 ms", "−100 ms" (U+2212 minus, so it
// matches the − glyph on the step buttons). The label shows this re-read from mpv, never our own arithmetic.
static QString formatMs(double secs)
{
    const long ms = qRound(secs * 1000.0);
    if (ms == 0) return QStringLiteral("0 ms");
    const QChar sign = ms > 0 ? QLatin1Char('+') : QChar(0x2212);
    return QStringLiteral("%1%2 ms").arg(sign).arg(qAbs(ms));
}

void MainWindow::showSubtitleMenu()
{
    if (subOverlay_) { hideSubtitleMenu(); return; } // second press on the button toggles it closed
    revealMediaControls();

    subOverlay_ = new QWidget(player_);
    subOverlay_->setObjectName(QStringLiteral("subScrim"));
    subOverlay_->setStyleSheet(QStringLiteral("#subScrim { background: rgba(8,8,12,0.60); }"));
    // Give the overlay its own arrow cursor so it never inherits the player's blanked cursor (which the
    // inactivity timer sets in full screen) over the scrim/card background.
    subOverlay_->setCursor(Qt::ArrowCursor);
    subOverlay_->setGeometry(player_->rect());
    subOverlay_->installEventFilter(this); // a click on the scrim (outside the card) dismisses

    // Centre the card within the scrim.
    auto* centre = new QVBoxLayout(subOverlay_);
    centre->setContentsMargins(0, 0, 0, 0);
    centre->addStretch(1);
    auto* midRow = new QHBoxLayout();
    midRow->addStretch(1);

    auto* card = new QFrame(subOverlay_);
    card->setObjectName(QStringLiteral("subCard"));
    card->setStyleSheet(QStringLiteral(
        "#subCard { background:#16161c; border:1px solid rgba(255,255,255,0.14); border-radius:16px; }"
        "#subCard QLabel { color:#e8e8e8; }"));
    const int cardW = qBound(360, player_->width() - 60, 760);
    card->setFixedWidth(cardW);
    card->setMaximumHeight(qMax(300, player_->height() - 48));
    midRow->addWidget(card);
    midRow->addStretch(1);
    centre->addLayout(midRow);
    centre->addStretch(1);

    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(24, 20, 24, 20);
    cv->setSpacing(14);

    // Header: title + close.
    auto* headRow = new QHBoxLayout();
    auto* title = new QLabel(tr("Audio & Subtitles"), card);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:bold;"));
    headRow->addWidget(title, 1);
    auto* closeBtn = new QPushButton(QString(QChar(0x00D7)), card); // × (U+00D7) renders in every font
    closeBtn->setFixedSize(32, 32);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background:rgba(255,255,255,0.08); color:#e8e8e8; border:none; border-radius:8px;"
        " font-size:22px; font-weight:bold; padding-bottom:3px; }"
        "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, [this] { hideSubtitleMenu(); });
    headRow->addWidget(closeBtn);
    cv->addLayout(headRow);

    // Two columns side by side: the track list (left) and the sync/size/source controls (right), so you can
    // reach the settings directly instead of walking the whole track list.
    auto* body = new QHBoxLayout();
    body->setSpacing(22);
    auto* leftCol = new QVBoxLayout();
    leftCol->setSpacing(8);
    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);

    auto sectionLabel = [card](const QString& t) {
        auto* l = new QLabel(t, card);
        l->setStyleSheet(QStringLiteral("font-size:13px;font-weight:bold;color:#9aa0aa;"));
        return l;
    };

    // Flat full-width row button used for tracks + source actions; `on` gives it the accent selected look.
    auto rowButton = [card](const QString& text, bool on) {
        auto* b = new QPushButton(text, card);
        b->setMinimumHeight(38);
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setStyleSheet(QString(QStringLiteral(
            "QPushButton { text-align:left; padding:6px 14px; border-radius:8px; color:#e8e8e8; font-size:15px;"
            " background:%1; border:1px solid %2; }"
            "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.55); border:1px solid rgba(255,255,255,0.5); }"))
            .arg(on ? QStringLiteral("rgba(90,140,255,0.30)") : QStringLiteral("rgba(255,255,255,0.05)"),
                 on ? QStringLiteral("rgba(90,140,255,0.9)")  : QStringLiteral("transparent")));
        return b;
    };

    // --- LEFT: audio-track picker + subtitle-track picker, scrollable so long lists stay contained. ---
    auto* trackScroll = new QScrollArea(card);
    trackScroll->setWidgetResizable(true);
    trackScroll->setFrameShape(QFrame::NoFrame);
    trackScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    trackScroll->setStyleSheet(QStringLiteral("background:transparent;"));
    trackScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* trackHost = new QWidget(trackScroll);
    auto* tv = new QVBoxLayout(trackHost);
    tv->setContentsMargins(0, 0, 6, 0);
    tv->setSpacing(6);
    subLeftCol_ = {};
    QPushButton* initial = nullptr;

    // A track's display label: "LANG · Title", or a numbered fallback.
    auto trackLabel = [this](const MpvWidget::Track& t) {
        const QString lang = t.lang.isEmpty() ? QString() : t.lang.toUpper();
        QString label = lang;
        if (!t.title.isEmpty()) label = label.isEmpty() ? t.title : lang + QStringLiteral("  ·  ") + t.title;
        return label.isEmpty() ? tr("Track %1").arg(t.id) : label;
    };

    // Audio tracks (only when the file actually has some to choose between / label).
    const auto audio = player_->audioTracks();
    if (!audio.isEmpty())
    {
        tv->addWidget(sectionLabel(tr("AUDIO")));
        for (const MpvWidget::Track& t : audio)
        {
            auto* b = rowButton((t.selected ? QStringLiteral("✓  ") : QStringLiteral("     ")) + trackLabel(t), t.selected);
            const int id = t.id;
            connect(b, &QPushButton::clicked, this, [this, id] { player_->setAudioTrack(id); hideSubtitleMenu(); });
            tv->addWidget(b);
            subLeftCol_ << b;
            if (t.selected && !initial) initial = b;
        }
        tv->addSpacing(8);
    }

    // Subtitle tracks (Off + each).
    tv->addWidget(sectionLabel(tr("SUBTITLE")));
    const auto tracks = player_->subtitleTracks();
    bool anySelected = false;
    for (const MpvWidget::Track& t : tracks) if (t.selected) anySelected = true;

    auto* offBtn = rowButton(tr("Off"), !anySelected);
    connect(offBtn, &QPushButton::clicked, this, [this] { player_->setSubtitleTrack(-1); hideSubtitleMenu(); });
    tv->addWidget(offBtn);
    subLeftCol_ << offBtn;
    if (!initial) initial = offBtn;
    for (const MpvWidget::Track& t : tracks)
    {
        auto* b = rowButton((t.selected ? QStringLiteral("✓  ") : QStringLiteral("     ")) + trackLabel(t), t.selected);
        const int id = t.id;
        connect(b, &QPushButton::clicked, this, [this, id] { player_->setSubtitleTrack(id); hideSubtitleMenu(); });
        tv->addWidget(b);
        subLeftCol_ << b;
        if (t.selected) initial = b;
    }
    if (tracks.isEmpty())
    {
        auto* none = new QLabel(tr("No subtitle tracks — load or download one on the right."), card);
        none->setStyleSheet(QStringLiteral("color:#999;font-size:13px;padding:2px 4px;"));
        none->setWordWrap(true);
        tv->addWidget(none);
    }
    tv->addStretch(1);
    trackScroll->setWidget(trackHost);
    leftCol->addWidget(trackScroll, 1);

    // --- RIGHT: sync + size adjusters (−/+ update live), then the source actions. ---
    // Close is the first right-column focus target (it sits top-right), so Up from the settings reaches it.
    subRightCol_ = { closeBtn };
    rightCol->addWidget(sectionLabel(tr("SUBTITLE SIZE")));
    auto addAdjustRow = [this, card, rightCol](const QString& name, std::function<QString()> value,
                                               std::function<void()> minus, std::function<void()> plus) {
        auto* w = new QWidget(card);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        auto* lbl = new QLabel(name + QStringLiteral(":  ") + value(), w);
        lbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        h->addWidget(lbl, 1);
        auto mkBtn = [w](const QString& t) {
            auto* b = new QPushButton(t, w);
            b->setFixedSize(40, 34);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QPushButton { background:rgba(255,255,255,0.10); color:#e8e8e8; border:none; border-radius:8px;"
                " font-size:19px;font-weight:bold; padding:0; }"
                "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
            return b;
        };
        auto* minusBtn = mkBtn(QString(QChar(0x2212))); // − minus sign
        auto* plusBtn = mkBtn(QStringLiteral("+"));
        h->addWidget(minusBtn);
        h->addWidget(plusBtn);
        connect(minusBtn, &QPushButton::clicked, w, [=] { minus(); lbl->setText(name + QStringLiteral(":  ") + value()); });
        connect(plusBtn,  &QPushButton::clicked, w, [=] { plus();  lbl->setText(name + QStringLiteral(":  ") + value()); });
        rightCol->addWidget(w);
        subRightCol_ << minusBtn << plusBtn;
    };
    addAdjustRow(tr("Size"),
                 [this] { return QStringLiteral("%1%").arg(qRound(player_->subtitleScale() * 100)); },
                 [this] { player_->setSubtitleScale(qMax(0.2, player_->subtitleScale() - 0.1)); },
                 [this] { player_->setSubtitleScale(qMin(4.0, player_->subtitleScale() + 0.1)); });

    // AUDIO SYNC + SUBTITLE SYNC: one lambda builds both sections (DRY), parameterized on the axis + its live
    // getter/setter. Compact so the whole card fits at TV scale without a (focus-trapping) scroll area: a readout
    // + inline ∓50 ms steppers on one row (like the Size row above), then a Reset / Save-as-default row. The
    // readout re-reads mpv after every change (shows mpv truth, never our arithmetic). Per-file steps persist via
    // SyncOffsets keyed by syncKey_; Reset drops the per-file entry (revert to the global default); Save-as-default
    // promotes the current value to the global. Every focusable button joins subRightCol_ + is hitClamp-sized.
    auto addSyncSection = [this, card, rightCol, rowButton, sectionLabel](const QString& heading, SyncOffsets::Which w,
                              std::function<double()> getter, std::function<void(double)> setter) {
        FormFactor& ff = FormFactor::instance();
        rightCol->addSpacing(6);
        rightCol->addWidget(sectionLabel(heading));

        // Row 1: live readout (mpv truth) + the ∓50 ms steppers, on one line.
        auto* r1 = new QWidget(card);
        auto* h1 = new QHBoxLayout(r1);
        h1->setContentsMargins(0, 0, 0, 0);
        h1->setSpacing(10);
        auto* readout = new QLabel(formatMs(getter()), r1);
        readout->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        h1->addWidget(readout, 1);
        auto mkStep = [r1, &ff](const QString& t) {
            auto* b = new QPushButton(t, r1);
            b->setMinimumSize(ff.hitClamp(58), ff.hitClamp(34));
            b->setAutoRepeat(true);                 // hold to keep nudging
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QPushButton { background:rgba(255,255,255,0.10); color:#e8e8e8; border:none; border-radius:8px;"
                " font-size:14px;font-weight:bold; padding:0 6px; }"
                "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
            return b;
        };
        auto* minusBtn = mkStep(QString(QChar(0x2212)) + QStringLiteral("50"));
        auto* plusBtn  = mkStep(QStringLiteral("+50"));
        h1->addWidget(minusBtn);
        h1->addWidget(plusBtn);
        rightCol->addWidget(r1);

        auto step = [this, w, getter, setter, readout](double delta) {
            const double v = qBound(-10.0, getter() + delta, 10.0);
            setter(v);
            SyncOffsets::savePerFile(syncKey_, w, v);
            readout->setText(formatMs(getter())); // re-read from mpv
        };
        connect(minusBtn, &QPushButton::clicked, this, [step] { step(-0.05); });
        connect(plusBtn,  &QPushButton::clicked, this, [step] { step(+0.05); });
        subRightCol_ << minusBtn << plusBtn;

        // Row 2: Reset (revert to global) + Save as default (promote current to global), side by side.
        auto* resetBtn = rowButton(tr("Reset"), false);
        resetBtn->setMinimumHeight(ff.hitClamp(34));
        connect(resetBtn, &QPushButton::clicked, this, [this, w, setter, readout] {
            SyncOffsets::clearPerFile(syncKey_, w);
            setter(SyncOffsets::globalDefault(w));
            readout->setText(formatMs(w == SyncOffsets::Which::Audio ? player_->audioDelay()
                                                                     : player_->subtitleDelay()));
        });
        auto* saveBtn = rowButton(tr("Save as default"), false);
        saveBtn->setMinimumHeight(ff.hitClamp(34));
        connect(saveBtn, &QPushButton::clicked, this, [this, w, getter] {
            SyncOffsets::setGlobalDefault(w, getter());
            notify(tr("Saved as the default sync offset."), 2500);
        });
        auto* r2 = new QWidget(card);
        auto* h2 = new QHBoxLayout(r2);
        h2->setContentsMargins(0, 0, 0, 0);
        h2->setSpacing(10);
        h2->addWidget(resetBtn);
        h2->addWidget(saveBtn);
        rightCol->addWidget(r2);
        subRightCol_ << resetBtn << saveBtn;
    };
    addSyncSection(tr("AUDIO SYNC"), SyncOffsets::Which::Audio,
                   [this] { return player_->audioDelay(); },
                   [this](double v) { player_->setAudioDelay(v); });
    addSyncSection(tr("SUBTITLE SYNC"), SyncOffsets::Which::Sub,
                   [this] { return player_->subtitleDelay(); },
                   [this](double v) { player_->setSubtitleDelay(v); });

    rightCol->addSpacing(6);
    rightCol->addWidget(sectionLabel(tr("SOURCE")));
    auto* loadBtn = rowButton(tr("📂  Load from file…"), false);
    connect(loadBtn, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Load subtitle"), QString(),
            tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx);;All files (*)"));
        if (!f.isEmpty()) player_->addSubtitle(f);
        hideSubtitleMenu();
    });
    rightCol->addWidget(loadBtn);
    subRightCol_ << loadBtn;
    if (SubtitleFetcher::configured() && !(subCtx_.imdbStreamId.isEmpty() && subCtx_.title.isEmpty()))
    {
        auto* dlBtn = rowButton(tr("🔍  Download from OpenSubtitles"), false);
        connect(dlBtn, &QPushButton::clicked, this, [this] {
            hideSubtitleMenu();
            notify(tr("Searching OpenSubtitles for a subtitle…"), 0);
            subFetcher_->fetch(subCtx_.imdbStreamId, subCtx_.title, Settings::subtitleLanguage(),
                               [this](const QString& srt) {
                if (!srt.isEmpty()) { player_->addSubtitle(srt); notify(tr("Subtitle added."), 3000); }
                else notify(tr("No matching subtitle found on OpenSubtitles."), kFeedbackLong);
            });
        });
        rightCol->addWidget(dlBtn);
        subRightCol_ << dlBtn;
    }
    rightCol->addStretch(1);

    body->addLayout(leftCol, 3);
    body->addLayout(rightCol, 2);
    cv->addLayout(body, 1);

    subOverlay_->show();
    subOverlay_->raise();
    initial->setFocus(Qt::TabFocusReason); // land on the current track (or Off)
}

void MainWindow::openLibraryItem(const MediaItem& item)
{
    // Whether the player/reader should offer "Issue with Streaming" for this item (an Allarr-resolved file
    // whose source can be swapped). Preserved across the remote-document download round-trip (item is copied).
    currentNextSourceCapable_ = item.nextSourceCapable;
    mwLog(QStringLiteral("open: \"%1\" type=%2 mime=%3 src=%4%5")
              .arg(item.title, item.type.isEmpty() ? QStringLiteral("?") : item.type,
                   item.mime.isEmpty() ? QStringLiteral("-") : item.mime, logSafeUrl(item.url),
                   splitTarget_ ? QStringLiteral(" [->split pane]") : QString()));
    if (item.url.isEmpty())
    {
        // Catalog metadata with no file associated yet (movies/games/episodes/tracks).
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(item.title), kFeedbackLong);
        return;
    }
    // Local library prefer-local: if we own this item on disk, play the local file instead of resolving
    // a stream. Guard on mime so the re-entry (with a filesystem url) doesn't recurse. Movies key on id;
    // episodes key on imdbStreamId ("ttShow:season:episode", matches the OwnedIndex episode key).
    if (item.mime != QStringLiteral("local:video"))
    {
        QString localPath = LocalLibrary::index().localPathFor(item.id);
        if (localPath.isEmpty() && !item.imdbStreamId.isEmpty())
            localPath = LocalLibrary::index().localPathFor(item.imdbStreamId);
        if (!localPath.isEmpty() && QFileInfo::exists(localPath))
        {
            MediaItem local = item;
            local.url = localPath;
            local.mime = QStringLiteral("local:video");
            openLibraryItem(local);   // re-enter: filesystem url + local:video mime -> mpv branch
            return;
        }
    }
    // A GOG game: a DRM-free exe launched through the MONITORED path (launchPcExe records the "goggame" Recent
    // itself — do NOT re-record here). Its exe rides on item.url; id/title/thumb come from the tile.
    if (item.mime == QStringLiteral("goggame"))
    {
        launchPcExe(item.url, item.id, item.title, item.thumbnailUrl, QStringLiteral("goggame"));
        return;
    }
    // An Epic game: hand the launcher URI to the OS (fire-and-forget, exactly like steam://). Record a Recent
    // (kind "epicgame", key "epic:<AppName>") so it resumes from the Recent tab and re-launches via the URI.
    if (item.url.startsWith(QStringLiteral("com.epicgames.launcher://")))
    {
        QDesktopServices::openUrl(QUrl(item.url));
        const QString appName = item.url.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
        const QString key = item.id.startsWith(QStringLiteral("epic:"))
                                ? item.id : QStringLiteral("epic:") + appName;
        RecentStore::add({ item.url, item.title, QStringLiteral("epicgame"), item.thumbnailUrl, key });
        statusBar()->showMessage(tr("Launching “%1” via Epic…").arg(item.title), 5000);
        return;
    }
    // A Steam game: hand it to the Steam client to launch (it handles install/run). Fire-and-forget — the Steam
    // client owns the process (no play-time tracking on this path).
    if (item.url.startsWith(QStringLiteral("steam://")))
    {
        QDesktopServices::openUrl(QUrl(item.url));
        const bool installing = item.url.startsWith(QStringLiteral("steam://install/"));
        // A RUN records a Recent (kind "steamgame", key "steam:<appid>", capsule thumb) so it resumes from the
        // Recent tab and re-launches via SteamLibrary::launchUrl. An install handoff is not a play -> no Recent.
        if (!installing)
        {
            const QString appid = item.url.section(QLatin1Char('/'), -1);
            const QString key = item.id.startsWith(QStringLiteral("steam:"))
                                    ? item.id : QStringLiteral("steam:") + appid;
            RecentStore::add({ item.url, item.title, QStringLiteral("steamgame"), item.thumbnailUrl, key });
        }
        statusBar()->showMessage(installing ? tr("Installing “%1” via Steam…").arg(item.title)
                                            : tr("Launching “%1” via Steam…").arg(item.title), 5000);
        return;
    }
    const QString url = item.url;
    const QString type = item.type.toLower();
    const QString lower = url.toLower();
    QString err;

    // PC (Windows) games aren't emulator ROMs — we're on a PC, so download the file and hand it to the OS to
    // run/install (an installer or portable .exe runs; an archive opens). Must come before the ROM handling
    // below, which would otherwise try to find an emulator core for it and fail ("no system").
    if (type == QStringLiteral("game") && isPcPlatform(item.systemHint))
    {
        openPcGame(item);
        return;
    }

    // Playlists (IPTV channel lists / HLS streams) must be handled before the ROM check below: ".m3u" is
    // also the PlayStation multi-disc extension, so it would otherwise be fetched as a game. streams_->resolve()
    // re-checks the contents and still routes a genuine disc list to the emulator.
    if (StreamResolver::isM3uRef(lower))
    {
        if (splitTarget_) { splitTarget_->openVideo(url, item.title); finishSplitOpen(); return; }
        streams_->resolve(url, item.title);
        return;
    }

    // Documents (CBZ/EPUB/PDF) open through file-based readers (miniz / epub / PDFium), which need a
    // local path. When an addon hands us a remote http(s) document, fetch it to a cache file first,
    // then re-enter with the local path. Pick the reader extension from the url, else the mime (debrid
    // links often have no extension), else the media type. Audio mimes/types are left for libmpv.
    if (lower.startsWith(QStringLiteral("http://")) || lower.startsWith(QStringLiteral("https://")))
    {
        const QString mime = item.mime.toLower();
        QString ext;
        if      (lower.endsWith(QStringLiteral(".cbz")))  ext = QStringLiteral(".cbz");
        else if (lower.endsWith(QStringLiteral(".epub"))) ext = QStringLiteral(".epub");
        else if (lower.endsWith(QStringLiteral(".pdf")))  ext = QStringLiteral(".pdf");
        else if (mime.contains(QStringLiteral("epub")))   ext = QStringLiteral(".epub");
        else if (mime.contains(QStringLiteral("pdf")))    ext = QStringLiteral(".pdf");
        else if (mime.contains(QStringLiteral("comicbook")) || mime.contains(QStringLiteral("cbz"))) ext = QStringLiteral(".cbz");
        else if (type == QStringLiteral("comic") || type == QStringLiteral("manga")) ext = QStringLiteral(".cbz");
        else if (type == QStringLiteral("ebook") || type == QStringLiteral("book")) ext = QStringLiteral(".epub");
        else if (type == QStringLiteral("pdf"))  ext = QStringLiteral(".pdf");
        if (!ext.isEmpty()) { fetchRemoteDocumentThenOpen(item, ext); return; }

        // Game ROMs run in the emulator (RetroView / external), which loads a local file. Fetch the
        // ROM to a cache file first, keeping its extension so the right core is picked. Take the extension
        // from the url, else the mime — a debrid link (e.g. a Switch game) has no filename in its path, so
        // the addon passes the rom type as an "application/x-<ext>" mime (mirrors the document fallback above).
        QString romExt = QFileInfo(QUrl(url).path()).suffix().toLower();
        if (romExt.isEmpty() && type == QStringLiteral("game"))
        {
            QString cand = mime.section(QLatin1Char('/'), -1).section(QLatin1Char(';'), 0, 0);
            if (cand.startsWith(QStringLiteral("x-"))) cand = cand.mid(2);
            // A recognized ROM extension, OR an archive the ROM is packed in (a debrid link is a bare id with
            // no filename, so the mime is our only clue — console ROMs are usually a .zip/.7z of the ROM, which
            // openGamePath extracts). Without this the raw URL is handed to the emulator, which can't open it.
            if (SystemCatalog::forExtension(cand) != nullptr
                || cand == QStringLiteral("zip") || cand == QStringLiteral("7z") || cand == QStringLiteral("rar"))
                romExt = cand;
        }
        if (!romExt.isEmpty() && (type == QStringLiteral("game") || SystemCatalog::forExtension(romExt) != nullptr))
        { fetchRemoteDocumentThenOpen(item, QStringLiteral(".") + romExt); return; }
    }

    // Split screen: the item is now a local file (remote docs/ROMs were fetched above) or a streamable URL -
    // load it into the focused pane instead of the full-screen views. Games fall through to openGamePath,
    // which is split-aware (it needs to resolve the core first).
    if (splitTarget_)
    {
        if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
        { splitTarget_->openBook(url); finishSplitOpen(); return; }
        if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
        { splitTarget_->openPdf(url); finishSplitOpen(); return; }
        if (lower.endsWith(QStringLiteral(".cbz")))
        { splitTarget_->openComic(url); finishSplitOpen(); return; }
        const bool isGame = (type == QStringLiteral("game")
                             || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr);
        if (!isGame) // video / audio / audiobook all play through the pane's own libmpv
        { splitTarget_->openVideo(url, item.title); finishSplitOpen(); return; }
    }

    // Recent entry for a document: carry the catalog title/cover (the local file is often a hashed cache
    // name) and key on the stable item id so re-opening de-dups instead of stacking a second entry.
    auto recordDocument = [&] {
        const QString t = item.title.isEmpty() ? QFileInfo(url).completeBaseName() : item.title;
        RecentStore::add({ url, t, QStringLiteral("document"), item.thumbnailUrl, item.id });
    };

    if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
    {
        if (!book_->openBook(url, &err)) { notify(tr("Can't open book: %1").arg(err), kFeedbackLong); return; }
        player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); session_->clearQueue();
        book_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
        presentBook();
        recordDocument();
    }
    else if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
    {
        // Prefer the reflowable reader (font sizing / pagination like EPUB) for text PDFs - this is mainly a
        // book app. Fall back to the fixed page-image view for scanned PDFs that have no text layer.
        if (book_->openBook(url, &err))
        {
            player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); session_->clearQueue();
            book_->setStreamIssueVisible(currentNextSourceCapable_);
            presentBook();
            recordDocument();
        }
        else if (pdf_->openPdf(url, &err))
        {
            player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); session_->clearQueue();
            pdf_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
            presentPdf();
            recordDocument();
        }
        else { notify(tr("Can't open PDF: %1").arg(err), kFeedbackLong); }
    }
    else if (lower.endsWith(QStringLiteral(".cbz"))) // a downloaded/associated comic archive
    {
        if (!comic_->openComic(url, &err)) { notify(tr("Can't open comic: %1").arg(err), kFeedbackLong); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); session_->clearQueue();
        presentComic();
        recordDocument();
    }
    else if (type == QStringLiteral("audiobook") || item.mime.toLower().startsWith(QStringLiteral("audio/")))
    {
        // An audiobook (or any audio-mime stream, e.g. from Allarr): play in the now-playing audio view with
        // resume keyed by the stable item id (a re-resolved debrid URL changes, so it can't be the key).
        openAudioStream(url, item.id, item.title, item.thumbnailUrl);
    }
    else if (type == QStringLiteral("audio"))
    {
        // Delegate to the SAME entry point the audiobook branch uses: openAudioStream owns the themed-audio
        // routing (themedAudioSession_ + the page's cover/title data) as well as the J18 stable-id resume key.
        // The old inline setQueue bypassed that routing, leaving a STALE themedAudioSession_ to decide the
        // surface — a classic page in themed mode (or a themed page with the previous item's art).
        openAudioStream(url, item.id, item.title, item.thumbnailUrl);
    }
    else if (type == QStringLiteral("game") || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr)
    {
        // Carry the catalog title/cover/id into Recent (the ROM file itself is a hashed cache name), and the
        // console/platform hint so the right emulator is picked even when the extension is shared.
        openGamePath(url, item.title, item.thumbnailUrl, item.id, item.systemHint);
    }
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        // Resume + Recent are keyed by the item's stable id when it has one (a debrid/stream URL changes every
        // time it's resolved, so keying on the URL would lose your place and duplicate the Recent entry).
        const QString rkey = item.id.isEmpty() ? url : item.id;
        // External-player handoff: an external player takes the resolved URL (Recent on both routes). A one-off
        // armed on this leaf rode here as item.playRouteHint (leak-free — a failed resolve never reaches here).
        if (routePlay(url, routeFromHint(item.playRouteHint))) {
            const QString rt = !item.title.isEmpty() ? item.title : QUrl(url).fileName();
            RecentStore::add({ url, rt, QStringLiteral("video"), item.thumbnailUrl, rkey });
            return;
        }
        notePlaybackStart();     // channel guard (built-in catalog play): keep the channel iff this is its pick
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); session_->clearQueue();
        session_->setMediaVideo(true); // consumption-stats: a catalog movie/episode stream accrues "watch" seconds
        session_->beginResume(rkey);
        syncKey_ = rkey;         // catalog stream: key sync offsets by the stable id, not the volatile URL
        armSubtitleFetch(item); // auto-download a subtitle if this movie/episode has none in the preferred language
        castUrl_ = url; castTitle_ = item.title; castMime_ = item.mime; // castable stream for the cast button
        castMgr_->startDiscovery();     // prime device discovery so the cast menu is populated when opened
        startScrobble(item.imdbStreamId); // Trakt: begin tracking this movie/episode
        stack_->setCurrentWidget(playerPage_);
        player_->play(url);
        revealMediaControls();
        const QString title = !item.title.isEmpty() ? item.title : QUrl(url).fileName();
        RecentStore::add({ url, title, QStringLiteral("video"), item.thumbnailUrl, rkey });
    }
}

void MainWindow::fetchRemoteDocumentThenOpen(const MediaItem& item, const QString& ext)
{
    // Cache by url hash so re-opening the same document doesn't re-download it.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + QStringLiteral("/remote-docs");
    QDir().mkpath(dir);
    const QString hash = QString::fromUtf8(
        QCryptographicHash::hash(item.url.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString localPath = dir + QStringLiteral("/") + hash + ext;

    auto openLocal = [this, item, localPath] {
        hideNotice(); // resolve+download feedback done; the content view takes over
        MediaItem local = item;
        local.url = localPath; // a local path now -> openLibraryItem dispatches to the file-based reader
        openLibraryItem(local);
    };

    if (QFileInfo::exists(localPath) && QFileInfo(localPath).size() > 0)
    {
        mwLog(QStringLiteral("download: cache hit for \"%1\" (%2 bytes %3) — opening")
                  .arg(item.title).arg(QFileInfo(localPath).size()).arg(ext));
        openLocal();
        return;
    }

#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    if (item.cfCurl)
    {
        // A Cloudflare-gated direct source (lolroms): the normal HTTP client's TLS fingerprint gets a 403, so
        // fetch it with a browser-UA curl (ships on Win10+/macOS/Linux) straight to the .part file. No addon
        // proxy, so no Cloudflare-tunnel size/time limit, and no in-memory buffering (curl writes to disk).
        const QString partPath = localPath + QStringLiteral(".part");
#ifdef Q_OS_WIN
        QString curlExe = QDir(QString::fromLocal8Bit(qgetenv("SystemRoot"))).filePath(QStringLiteral("System32/curl.exe"));
        if (!QFileInfo::exists(curlExe)) curlExe = QStringLiteral("curl");
#else
        const QString curlExe = QStringLiteral("curl");
#endif
        const QUrl u(item.url);
        const QString referer = u.scheme() + QStringLiteral("://") + u.host() + QStringLiteral("/");
        static const QString kUa = QStringLiteral(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");

        statusBar()->showMessage(tr("Downloading “%1”…").arg(item.title));
        notify(tr("Downloading “%1”…").arg(item.title), 0);
        mwLog(QStringLiteral("download(curl): %1 -> %2").arg(logSafeUrl(item.url), QFileInfo(partPath).fileName()));

        QProcess* p = new QProcess(this);
        // curl doesn't stream progress to us, so poll the growing .part file for a byte count.
        QTimer* progress = new QTimer(p);
        connect(progress, &QTimer::timeout, this, [this, partPath, title = item.title] {
            const QString msg = tr("Downloading “%1”… %2 MB").arg(title).arg(QFileInfo(partPath).size() / 1048576);
            statusBar()->showMessage(msg); notify(msg, 0);
        });
        progress->start(1000);

        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, p, progress, partPath, localPath, openLocal, title = item.title](int code, QProcess::ExitStatus) {
            progress->stop();
            p->deleteLater();
            if (code != 0)
            {
                QFile::remove(partPath);
                mwLog(QStringLiteral("download(curl): FAILED \"%1\" (curl exit %2)").arg(title).arg(code));
                const QString e = tr("Couldn't download “%1” (the source may be down).").arg(title);
                statusBar()->showMessage(e, kFeedbackLong); notify(e, kFeedbackLong);
                return;
            }
            if (QFileInfo(partPath).size() == 0)
            {
                QFile::remove(partPath);
                mwLog(QStringLiteral("download(curl): empty (0 bytes) for \"%1\"").arg(title));
                notify(tr("Couldn't get “%1” — the source returned no data (there may be no copy).").arg(title), kFeedbackLong);
                return;
            }
            QFile::remove(localPath);
            if (!QFile::rename(partPath, localPath))
            {
                mwLog(QStringLiteral("download(curl): finalise (rename) failed for \"%1\"").arg(title));
                notify(tr("Couldn't finalise the download for “%1”.").arg(title), kFeedbackLong);
                return;
            }
            mwLog(QStringLiteral("download(curl): complete \"%1\" (%2 bytes) — opening").arg(title).arg(QFileInfo(localPath).size()));
            statusBar()->clearMessage();
            openLocal();
        });
        connect(p, &QProcess::errorOccurred, this, [this, p, progress, title = item.title](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return; // other errors also fire `finished`, handled there
            progress->stop(); p->deleteLater();
            mwLog(QStringLiteral("download(curl): curl failed to start for \"%1\"").arg(title));
            const QString msg = tr("Couldn't download “%1”: curl isn't available.").arg(title);
            statusBar()->showMessage(msg, kFeedbackLong); notify(msg, kFeedbackLong);
        });
        p->start(curlExe, { QStringLiteral("-sL"), QStringLiteral("-A"), kUa, QStringLiteral("-e"), referer,
                            QStringLiteral("--fail"), QStringLiteral("-o"), partPath, item.url });
        return;
    }
#endif

    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    statusBar()->showMessage(tr("Downloading “%1”…").arg(item.title));
    // Continue the feedback in the same toast that showed "Finding/Looking…", so the file-pull progress
    // appears where the user is already looking (not just the status bar). Sticky until it opens/fails.
    notify(tr("Downloading “%1”…").arg(item.title), 0);
    mwLog(QStringLiteral("download: GET %1 -> %2").arg(logSafeUrl(item.url), QFileInfo(localPath).fileName()));

    // Stream the body straight to a .part file as it arrives instead of buffering the whole thing in memory
    // with readAll(): ROMs (Wii U / GameCube / PS2 disc images) can be several GB and would exhaust RAM.
    const QString partPath = localPath + QStringLiteral(".part");
    auto part = std::make_shared<QFile>(partPath);
    if (!part->open(QIODevice::WriteOnly))
    {
        mwLog(QStringLiteral("download: can't open cache file for \"%1\": %2").arg(item.title, part->errorString()));
        const QString e = tr("Couldn't save “%1” to cache.").arg(item.title);
        statusBar()->showMessage(e, kFeedbackLong); notify(e, kFeedbackLong);
        return;
    }

    QNetworkRequest rq{QUrl(item.url)};
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = docNam_->get(rq);

    // Write each chunk to disk as it arrives — memory stays ~one buffer, not the whole file.
    connect(reply, &QNetworkReply::readyRead, this, [reply, part] { part->write(reply->readAll()); });

    // Live download feedback: a percentage when the server sends a Content-Length, else
    // the running byte count. Throttled to whole-percent / changed-text updates so we
    // don't thrash the status bar on every packet.
    auto humanMB = [](qint64 bytes) { return QString::number(bytes / 1048576.0, 'f', 1); };
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, title = item.title, humanMB, lastPct = -1, lastLog = -1](qint64 received, qint64 total) mutable {
        QString msg;
        if (total > 0)
        {
            const int pct = static_cast<int>(received * 100 / total);
            if (pct == lastPct) return;            // same percent — skip the update
            lastPct = pct;
            msg = tr("Downloading “%1”… %2%  (%3 / %4 MB)")
                      .arg(title).arg(pct).arg(humanMB(received), humanMB(total));
            if (pct >= lastLog + 25 || pct == 100) // log every ~25% so the trace isn't spammed
            { lastLog = pct; mwLog(QStringLiteral("download: \"%1\" %2%% (%3/%4 MB)")
                                       .arg(title).arg(pct).arg(humanMB(received), humanMB(total))); }
        }
        else
        {
            msg = tr("Downloading “%1”… %2 MB").arg(title, humanMB(received));
        }
        statusBar()->showMessage(msg);
        notify(msg, 0); // mirror the live percentage into the toast (sticky)
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, part, partPath, localPath, openLocal, title = item.title] {
        reply->deleteLater();
        part->write(reply->readAll());               // any tail not yet drained by readyRead
        part->flush();                               // surface a buffered write error (e.g. disk full)
        const bool writeOk = part->error() == QFileDevice::NoError;
        part->close();

        if (reply->error() != QNetworkReply::NoError)
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: FAILED \"%1\": %2").arg(title, reply->errorString()));
            const QString e = tr("Couldn't download “%1”: %2").arg(title, reply->errorString());
            statusBar()->showMessage(e, kFeedbackLong);
            notify(e, kFeedbackLong);
            return;
        }
        // A transport-level success isn't the whole story: an HTTP 404/403/5xx arrives with NoError but the body
        // is an error page, not the ROM. Reject a >=400 status so we don't cache and open a bogus file.
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http >= 400)
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: HTTP %1 for \"%2\"").arg(http).arg(title));
            const QString e = tr("Couldn't get “%1” — the source returned HTTP %2 (there may be no copy).").arg(title).arg(http);
            statusBar()->showMessage(e, kFeedbackLong);
            notify(e, kFeedbackLong);
            return;
        }
        // A dropped connection can finish "cleanly" mid-file. If the server told us the length up front, reject a
        // body that came up short rather than caching a truncated ROM/movie that would fail to open.
        const qint64 expected = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (expected > 0 && QFileInfo(partPath).size() < expected)
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: truncated \"%1\" (%2/%3 bytes)").arg(title).arg(QFileInfo(partPath).size()).arg(expected));
            const QString e = tr("The download for “%1” stopped before it finished — please try again.").arg(title);
            statusBar()->showMessage(e, kFeedbackLong);
            notify(e, kFeedbackLong);
            return;
        }
        if (!writeOk)
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: save failed for \"%1\"").arg(title));
            statusBar()->showMessage(tr("Couldn't save “%1” to cache.").arg(title), kFeedbackLong);
            notify(tr("Couldn't save “%1” to cache.").arg(title), kFeedbackLong);
            return;
        }
        if (QFileInfo(partPath).size() == 0) // the source returned nothing (no copy / a dead link) - opening it would just fail
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: empty (0 bytes) for \"%1\"").arg(title));
            notify(tr("Couldn't get “%1” — the source returned no data (there may be no copy).").arg(title), kFeedbackLong);
            return;
        }
        QFile::remove(localPath);
        if (!QFile::rename(partPath, localPath))
        {
            mwLog(QStringLiteral("download: finalise (rename) failed for \"%1\"").arg(title));
            statusBar()->showMessage(tr("Couldn't finalise the download for “%1”.").arg(title), kFeedbackLong);
            notify(tr("Couldn't finalise the download for “%1”.").arg(title), kFeedbackLong);
            return;
        }
        mwLog(QStringLiteral("download: complete \"%1\" (%2 bytes) — opening").arg(title).arg(QFileInfo(localPath).size()));
        statusBar()->clearMessage();
        openLocal();
    });
}

// ---- library downloads (save for keeps to <app>/downloads, add to Recent) --------------------------------

namespace {
// A filesystem-safe file name from a media title.
QString safeFileName(const QString& title)
{
    QString s = title;
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\r\\n\\t]")), QStringLiteral("_"));
    s = s.simplified();
    return s.isEmpty() ? QStringLiteral("download") : s.left(150);
}
// The extension to save a resolved item under: url suffix, else mime, else media type.
QString downloadExt(const MediaItem& item)
{
    const QString lower = QUrl(item.url).path().toLower();
    const QString mime = item.mime.toLower();
    const QString type = item.type.toLower();
    for (const QString& e : { QStringLiteral(".mkv"), QStringLiteral(".mp4"), QStringLiteral(".avi"),
         QStringLiteral(".m4v"), QStringLiteral(".mov"), QStringLiteral(".webm"), QStringLiteral(".cbz"),
         QStringLiteral(".epub"), QStringLiteral(".pdf"), QStringLiteral(".mobi"), QStringLiteral(".m4b"),
         QStringLiteral(".mp3"), QStringLiteral(".m4a"), QStringLiteral(".flac"), QStringLiteral(".gba"),
         QStringLiteral(".gb"), QStringLiteral(".gbc"), QStringLiteral(".nes"), QStringLiteral(".sfc"),
         QStringLiteral(".smc"), QStringLiteral(".md"), QStringLiteral(".gen"), QStringLiteral(".smd"),
         QStringLiteral(".n64"), QStringLiteral(".z64"), QStringLiteral(".pce"), QStringLiteral(".ws"),
         QStringLiteral(".a26"), QStringLiteral(".cue"), QStringLiteral(".chd") })
        if (lower.endsWith(e)) return e;
    if (mime.contains(QStringLiteral("epub"))) return QStringLiteral(".epub");
    if (mime.contains(QStringLiteral("pdf")))  return QStringLiteral(".pdf");
    if (mime.contains(QStringLiteral("comicbook")) || mime.contains(QStringLiteral("cbz"))) return QStringLiteral(".cbz");
    if (mime.startsWith(QStringLiteral("audio/"))) return mime.contains(QStringLiteral("mpeg")) ? QStringLiteral(".mp3") : QStringLiteral(".m4a");
    if (mime.startsWith(QStringLiteral("video/"))) return QStringLiteral(".mp4");
    if (type == QStringLiteral("comic") || type == QStringLiteral("comic_issue") || type == QStringLiteral("manga")) return QStringLiteral(".cbz");
    if (type == QStringLiteral("book"))      return QStringLiteral(".epub");
    if (type == QStringLiteral("audiobook")) return QStringLiteral(".m4a");
    if (type == QStringLiteral("game"))
    { const QString e = QFileInfo(QUrl(item.url).path()).suffix(); return e.isEmpty() ? QStringLiteral(".bin") : QStringLiteral(".") + e; }
    return QStringLiteral(".mkv"); // a movie/episode default
}
// The Recent "kind" so re-opening routes to the right view.
QString downloadKind(const QString& type, const QString& ext)
{
    if (type == QStringLiteral("audiobook") || ext == QStringLiteral(".mp3") || ext == QStringLiteral(".m4a")
        || ext == QStringLiteral(".m4b") || ext == QStringLiteral(".flac")) return QStringLiteral("audio");
    if (ext == QStringLiteral(".cbz") || ext == QStringLiteral(".epub") || ext == QStringLiteral(".pdf")
        || ext == QStringLiteral(".mobi")) return QStringLiteral("document");
    if (type == QStringLiteral("game") || SystemCatalog::forExtension(ext.mid(1)) != nullptr) return QStringLiteral("game");
    return QStringLiteral("video");
}
// The canonical SystemCatalog id for a downloaded game, from its console hint (preferred) or file extension,
// so the Downloaded folder can group it under the right console. Empty if it isn't a game / can't be mapped.
QString downloadSystemId(const QString& systemHint, const QString& ext)
{
    const GameSystem* s = systemHint.isEmpty() ? nullptr : SystemCatalog::forConsoleName(systemHint);
    if (!s) s = SystemCatalog::forExtension(ext.startsWith(QLatin1Char('.')) ? ext.mid(1) : ext);
    return s ? s->id : QString();
}
} // namespace

void MainWindow::enqueueDownload(const MediaItem& item)
{
    if (item.url.isEmpty()) return;
    const QString ext = downloadExt(item);
    const QString kind = downloadKind(item.type, ext);
    DownloadJob j;
    j.title = item.title;
    j.url = item.url;
    j.dest = AppPaths::dataDir() + QStringLiteral("/downloads/") + safeFileName(item.title) + ext;
    j.kind = kind;
    j.sysId = (kind == QStringLiteral("game")) ? downloadSystemId(item.systemHint, ext) : QString();
    j.thumb = item.thumbnailUrl;
    j.key = item.id;
    dm_->enqueue(j);
    notify(tr("“%1” added to Downloads. See Settings ▸ Downloads for progress.").arg(item.title), 4000);
    mwLog(QStringLiteral("download(library): queued \"%1\" -> %2").arg(item.title, QFileInfo(j.dest).fileName()));
}



void MainWindow::openImagePages(const QString& title, const QString& key, const QStringList& pageUrls)
{
    mwLog(QStringLiteral("openImagePages: \"%1\" %2 page url(s)").arg(title).arg(pageUrls.size()));
    if (pageUrls.isEmpty()) { statusBar()->showMessage(tr("No pages to read for “%1”.").arg(title), kFeedbackLong); return; }

    // Cache the assembled chapter as a CBZ keyed by the chapter id, so re-opening it is instant and the
    // comic reader's path-based resume remembers your page.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/manga");
    QDir().mkpath(dir);
    const QString hash = QString::fromUtf8(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString cbzPath = dir + QStringLiteral("/") + hash + QStringLiteral(".cbz");

    auto openCbz = [this, cbzPath, title] {
        QString err;
        if (!comic_->openComic(cbzPath, &err))
        { mwLog(QStringLiteral("openImagePages: openComic failed: %1").arg(err)); notify(tr("Can't open “%1”: %2").arg(title, err), kFeedbackLong); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); session_->clearQueue();
        presentComic();
        mwLog(QStringLiteral("openImagePages: reader shown"));
    };

    if (QFileInfo::exists(cbzPath) && QFileInfo(cbzPath).size() > 0) { openCbz(); return; } // already cached

    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    statusBar()->showMessage(tr("Downloading “%1” (%2 pages)…").arg(title).arg(pageUrls.size()));

    // Fetch every page (in parallel), stored by index so the archive keeps the right order. When the last
    // one finishes, pack them into the CBZ and open it.
    auto pages = std::make_shared<QVector<QByteArray>>(pageUrls.size());
    auto remaining = std::make_shared<int>(pageUrls.size());
    for (int i = 0; i < pageUrls.size(); ++i)
    {
        QNetworkRequest rq{QUrl(pageUrls[i])};
        rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        rq.setTransferTimeout(20000);
        QNetworkReply* reply = docNam_->get(rq);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, i, pages, remaining, pageUrls, cbzPath, title, openCbz] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) (*pages)[i] = reply->readAll();
            if (--*remaining != 0) return; // wait for every page

            // Pack the downloaded pages into a CBZ (store, not deflate - the images are already compressed).
            const QString partPath = cbzPath + QStringLiteral(".part");
            QFile::remove(partPath);
            mz_zip_archive zip; std::memset(&zip, 0, sizeof(zip));
            if (!mz_zip_writer_init_file(&zip, partPath.toUtf8().constData(), 0))
            { notify(tr("Couldn't assemble “%1”.").arg(title), kFeedbackLong); return; }

            int added = 0;
            for (int p = 0; p < pages->size(); ++p)
            {
                const QByteArray& d = (*pages)[p];
                if (d.isEmpty()) continue; // a page that failed to download - skip it
                const QString lu = pageUrls[p].toLower();
                QString ext = QStringLiteral(".jpg");
                for (const QString& e : { QStringLiteral(".png"), QStringLiteral(".jpeg"),
                                          QStringLiteral(".webp"), QStringLiteral(".gif") })
                    if (lu.endsWith(e)) { ext = e; break; }
                const QString name = QStringLiteral("%1").arg(p + 1, 5, 10, QLatin1Char('0')) + ext; // natural order
                if (mz_zip_writer_add_mem(&zip, name.toUtf8().constData(), d.constData(), size_t(d.size()), 0)) ++added;
            }
            mz_zip_writer_finalize_archive(&zip);
            mz_zip_writer_end(&zip);

            mwLog(QStringLiteral("openImagePages: packed %1 page(s) into cbz").arg(added));
            if (added == 0)
            { QFile::remove(partPath); notify(tr("Couldn't download any pages for “%1”.").arg(title), kFeedbackLong); return; }
            QFile::remove(cbzPath);
            if (!QFile::rename(partPath, cbzPath))
            { mwLog(QStringLiteral("openImagePages: rename to cbz failed")); notify(tr("Couldn't save “%1”.").arg(title), kFeedbackLong); return; }
            statusBar()->clearMessage();
            mwLog(QStringLiteral("openImagePages: opening reader"));
            openCbz();
        });
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
    // Any panel we show replaces the previous content (deleting its widgets). The Downloads panel keeps live
    // pointers to its progress bars; clear the "is-open" flag here so a stray download tick can never touch the
    // freed widgets of a panel we've navigated away from. openDownloadManager re-sets it once it has rebuilt.
    dlPanelOpen_ = false;
    // Deprecation signal (B2 Task 6, item 2): showPanel is the single chokepoint for every classic QWidget
    // settings panel + inline dialog (showDialogPanel routes through here). Reaching one WHILE the themed home
    // is enabled means a classic surface leaked into themed mode — the Task 7 walk greps this line to prove
    // zero such sightings. Classic mode (themed disabled) is silent; that's the sanctioned classic path.
    if (themedHomeEnabled()) mwLog(QStringLiteral("deprecated-classic: panel:%1").arg(title));
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
    updateNavForPage();     // panel -> panel doesn't fire currentChanged; re-register the ring explicitly
    // Drop focus onto the first interactive row so arrow keys / a remote work without a click first.
    // Deferred a tick so the new content widget is laid out and its children are focusable.
    QTimer::singleShot(0, this, [this] {
        if (stack_->currentWidget() != panelPage_) return;
        if (QWidget* first = firstPanelRow()) first->setFocus();
    });
}

// The first focusable row in the current panel content (used for initial focus and Up→Back wrap-around).
QWidget* MainWindow::firstPanelRow() const
{
    QWidget* w = panelScroll_->widget();
    if (!w) return nullptr;
    QVector<QWidget*> rows;
    collectPanelRows(w->layout(), rows);      // visual order, so the first row is the topmost one
    return rows.isEmpty() ? nullptr : rows.first();
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
    updateNavForPage(); // panelDialog_ changed after showPanel: re-register the ring choice for it
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

// Play the menu background music on browse/settings screens; pause it on content (player, emulator, readers,
// split) so it never plays over a game or video.
void MainWindow::updateBackgroundMusic()
{
    if (!bgm_) return;
    QWidget* w = stack_->currentWidget();
    bool menu = (w == home_ || w == themedHome_ || w == themedBrowse_ || w == panelPage_ || w == library_);
#ifdef MMV_HAVE_QML
    menu = menu || (w == themedPanelHost_);
#endif
    bgm_->setActive(menu);
}

// A theme can ship default menu music via theme.json "music" (a path relative to the theme dir). It plays
// only when the user's own music folder is empty, so every theme has sound out of the box.
void MainWindow::applyThemeMusic(const QString& themeDir)
{
    if (!bgm_) return;
    QFile f(themeDir + QStringLiteral("/theme.json"));
    QString music;
    if (f.open(QIODevice::ReadOnly))
    {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        music = o.value(QStringLiteral("music")).toString();
    }
    bgm_->setThemeDefault(music.isEmpty() ? QString()
                                          : QDir(themeDir).absoluteFilePath(music));
}

// Push the current track name into the themed home so the Triple theme's "now playing" readout shows it.
void MainWindow::updateThemedNowPlaying()
{
    if (!themedHome_ || !bgm_) return;
#ifdef MMV_HAVE_QML // themedHome_ is only ever non-null in QML builds; guard the QtQuick types for the rest
    if (QQuickItem* r = ThemeEngine::rootItem(themedHome_))
        r->setProperty("nowPlaying", bgm_->currentTitle());
#endif
}

// The active theme's `settingsPanel` styling block (colors/accent) for the themed panels. Resolved from the same
// theme the themed home uses; missing keys fall back HARD in SettingsPanel.qml, so an empty map still renders.
QVariantMap MainWindow::settingsPanelStyle() const
{
    QVariantMap out;
#ifdef MMV_HAVE_QML
    const QStringList themes = ThemeEngine::availableThemes();
    QString themeName = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
    if (!themes.contains(themeName)) themeName = themes.value(0, QStringLiteral("Default"));
    const QString themeFile = ThemeEngine::themesRoot() + QStringLiteral("/") + themeName
                            + QStringLiteral("/theme.json");
    QFile f(themeFile);
    if (f.open(QIODevice::ReadOnly))
        out = QJsonDocument::fromJson(f.readAll()).object()
                  .value(QStringLiteral("settingsPanel")).toObject().toVariantMap();
#endif
    return out;
}

void MainWindow::openSettingsHub()
{
    if (!parentalUnlock(tr("Enter the parental PIN to open Settings."))) return;
    // Entering the settings area from a real page: remember it so the top-level Back returns there. (Coming back
    // from a child panel — classic panelPage_ or the themed host — must NOT clobber the remembered return page.)
    QWidget* cw = stack_->currentWidget();
    bool fromPanel = (cw == panelPage_);
#ifdef MMV_HAVE_QML
    fromPanel = fromPanel || (cw == themedPanelHost_);
#endif
    if (!fromPanel)
        panelReturnTo_ = cw;

#ifdef MMV_HAVE_QML
    // Themed mode: render the hub on the Nav Contract (ThemedPanelHost) instead of the classic widget panel. The
    // SAME rows, as Action descriptors, dispatch to the SAME open* methods (which route themed/classic per mode).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // settings-area BOUNDARY: every panel level is dropped below, so no stale
                                 // Cloud/RA listener may outlive its panel (the lifetime model at openCloudSync)
        themedPanelHost_->reset();                       // fresh root presentation — drop any stale panel levels
        themedPanelHost_->setStyle(settingsPanelStyle()); // the active theme's settingsPanel block (hard fallbacks)

        QVector<PanelRow> rows;
        auto act = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r;
        };
        act(QStringLiteral("general"),      tr("General"));
        act(QStringLiteral("stats"),        tr("Stats"));
        act(QStringLiteral("appearance"),   tr("Appearance"));
        act(QStringLiteral("addons"),       tr("Add-ons"));
        act(QStringLiteral("downloads"),    tr("Downloads"));
        act(QStringLiteral("cloud"),        tr("Cloud Sync"));
        act(QStringLiteral("split"),        tr("Split Screen"));
        act(QStringLiteral("retroach"),     tr("RetroAchievements"));
#if !defined(Q_OS_ANDROID)
        act(QStringLiteral("standalone"),   tr("Stand Alone Emulators Settings"));
#endif
        act(QStringLiteral("libretro"),     tr("Libretro Emulator Settings"));
        act(QStringLiteral("bios"),         tr("BIOS Check"));
        act(QStringLiteral("input"),        tr("Input Mapping…"));
        act(QStringLiteral("debug"),        tr("Debug"));
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("uninstall");
          r.label = tr("Uninstall My Media Vault…"); r.destructive = true; rows << r; }

        themedPanelHost_->present(tr("Settings"), rows,
            [this](const QString& id, const QString&) {
                // Dispatch to the SAME handlers the classic hub connects (each routes themed/classic per mode).
                if      (id == QStringLiteral("general"))    openGeneralSettings();
                else if (id == QStringLiteral("stats"))      openStats();
                else if (id == QStringLiteral("appearance")) openAppearance();
                else if (id == QStringLiteral("addons"))     openLibrary();
                else if (id == QStringLiteral("downloads"))  openDownloadManager();
                else if (id == QStringLiteral("cloud"))      openCloudSync();
                else if (id == QStringLiteral("split"))      enterSplitScreen();
                else if (id == QStringLiteral("retroach"))   openRetroAchievements();
                else if (id == QStringLiteral("standalone")) openEmulatorManager();
                else if (id == QStringLiteral("libretro"))   openEmulatorSettings();
                else if (id == QStringLiteral("bios"))       openBiosCheck();
                else if (id == QStringLiteral("input"))      openInputMapping();
                else if (id == QStringLiteral("debug"))      openDebug();
                else if (id == QStringLiteral("uninstall"))  confirmUninstall();
            },
            [this] {
                // The last panel dismissed (Back at the hub root): return to the home screen (rebuilt so an
                // Appearance/theme change applies), exactly like the classic hub's onBack. Leaving the settings
                // area is the other lifetime BOUNDARY: drop any armed panel listeners with it.
                clearPanelPageConns();
                if (panelReturnTo_ == home_ || panelReturnTo_ == themedHome_) showHomeScreen();
                else if (panelReturnTo_) stack_->setCurrentWidget(panelReturnTo_);
                else showHomeScreen();
            });
        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        updateBackgroundMusic();
        return;
    }
#endif

    showPanel(tr("Settings"), [this](QVBoxLayout* v) {
        auto add = [this, v](const QString& label, std::function<void()> fn) {
            auto* b = panelRow(label);
            connect(b, &QPushButton::clicked, this, fn);
            v->addWidget(b);
        };
        add(tr("General"),            [this] { openGeneralSettings(); });
        add(tr("Stats"),              [this] { openStats(); });             // per-profile consumption stats
        add(tr("Appearance"),         [this] { openAppearance(); });        // the home theme picker
        add(tr("Add-ons"),            [this] { openLibrary(); });
        add(tr("Downloads"),          [this] { openDownloadManager(); });   // resume / retry / cancel stalled downloads
        add(tr("Cloud Sync"),         [this] { openCloudSync(); });
        add(tr("Split Screen"),       [this] { enterSplitScreen(); });    // two media side by side (F8)
        add(tr("RetroAchievements"),  [this] { openRetroAchievements(); });
#if !defined(Q_OS_ANDROID)
        add(tr("Stand Alone Emulators Settings"), [this] { openEmulatorManager(); }); // standalone emulators (Dolphin…) - desktop only
#endif
        add(tr("Libretro Emulator Settings"), [this] { openEmulatorSettings(); }); // still a popup (phase 2)
        add(tr("BIOS Check"),         [this] { openBiosCheck(); });        // per-system BIOS presence (RetroBat-style)
        add(tr("Input Mapping…"),     [this] { openInputMapping(); });     // still a popup (phase 2)
        add(tr("Debug"),              [this] { openDebug(); });
        add(tr("Uninstall My Media Vault…"), [this] { confirmUninstall(); }); // remove the app + all its data
    }, [this] {
        // Returning to a home screen rebuilds it, so an Appearance/theme change applies on the way out.
        if (panelReturnTo_ == home_ || panelReturnTo_ == themedHome_) showHomeScreen();
        else stack_->setCurrentWidget(panelReturnTo_);
    });
}

// Per-profile consumption stats (Settings ▸ Stats). Reads the ConsumptionStats rollups (watched/listened seconds,
// pages reached) + PlayStats' games rollup (profileTotalSeconds — computed at display, PlayStats is NOT migrated),
// then top-5 titles per media category. Per-profile by construction (both stores scope by the active profile), so
// a profile switch shows different totals with no extra work. ONE row list feeds both surfaces: the themed
// ThemedPanelHost present() (a hub child -> Back pops to the hub) and, in classic mode, a showPanel rendering the
// same rows as labels. "Read" is furthest-page progress (the T1 high-water semantics), labelled as such.
void MainWindow::openStats()
{
    // --- Shared row list (Info + Separator descriptors) both surfaces render -------------------------------
    QVector<PanelRow> rows;
    auto info = [&rows](const QString& id, const QString& label, const QString& value) {
        PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r;
    };
    auto sep = [&rows](const QString& id, const QString& label) {
        PanelRow r; r.kind = PanelRow::Separator; r.id = id; r.label = label; rows << r;
    };
    auto durOrNone = [](qint64 s) { return s > 0 ? PlayStats::formatDuration(s) : tr("None yet"); };

    sep(QStringLiteral("stats.sep.totals"), tr("Totals"));
    info(QStringLiteral("stats.watched"),  tr("Watched"),
         durOrNone(ConsumptionStats::categorySeconds(QStringLiteral("video"))));
    info(QStringLiteral("stats.listened"), tr("Listened"),
         durOrNone(ConsumptionStats::categorySeconds(QStringLiteral("audio"))));
    const qint64 pages = ConsumptionStats::categoryPages();
    // "Read" is furthest-page progress (high-water pages reached), NOT pages-per-session — say so in the value.
    info(QStringLiteral("stats.read"), tr("Read"),
         pages > 0 ? tr("Pages reached: %1").arg(pages) : tr("None yet"));
    info(QStringLiteral("stats.played"), tr("Played"), durOrNone(PlayStats::profileTotalSeconds()));

    // Top-5 titles per media category (games live in PlayStats, no per-title join here — Playnite-parity totals).
    auto topSection = [&](const QString& cat, const QString& heading, bool reading) {
        const auto top = ConsumptionStats::topTitles(cat, 5);
        if (top.isEmpty()) return;
        sep(QStringLiteral("stats.sep.") + cat, heading);
        int i = 0;
        for (const auto& p : top) {
            const QString v = reading ? tr("%n page(s)", "", int(p.second.pagesRead))
                                      : PlayStats::formatDuration(p.second.mediaSeconds);
            info(QStringLiteral("stats.top.") + cat + QString::number(i++),
                 p.second.title.isEmpty() ? tr("(untitled)") : p.second.title, v);
        }
    };
    topSection(QStringLiteral("video"),   tr("Most watched"),  false);
    topSection(QStringLiteral("audio"),   tr("Most listened"), false);
    topSection(QStringLiteral("reading"), tr("Most read"),     true);

#ifdef MMV_HAVE_QML
    // Themed mode: render the rows on the Nav Contract (ThemedPanelHost) — Info/Separator rows are non-focusable
    // dividers, so the panel is read-only (Back pops to the hub). A hub child, exactly like openEmulatorManager.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // settings-area boundary (lifetime model at openCloudSync's connect block)
        themedPanelHost_->setStyle(settingsPanelStyle());
        auto onBack = [this] { openSettingsHub(); };
        if (themedPanelHost_->panelTitle() == tr("Stats"))
            themedPanelHost_->replaceTop(tr("Stats"), rows, [](const QString&, const QString&) {}, onBack);
        else
            themedPanelHost_->present(tr("Stats"), rows, [](const QString&, const QString&) {}, onBack);
        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif

    // Classic mode: the SAME rows as a scrollable label list (Separator = bold heading, Info = "label:   value").
    // Plain text throughout (no HTML) — a QLabel only interprets entities when it heuristically detects rich text
    // (needs a tag), so &nbsp; would render literally; real spaces + a bold QFont are unambiguous.
    showPanel(tr("Stats"), [rows](QVBoxLayout* v) {
        for (const PanelRow& r : rows) {
            if (r.kind == PanelRow::Separator) {
                auto* h = new QLabel(r.label);
                QFont f = h->font(); f.setBold(true); h->setFont(f);
                v->addWidget(h);
            } else {
                auto* lbl = new QLabel(r.value.isEmpty() ? r.label
                                                         : r.label + QStringLiteral(":   ") + r.value);
                lbl->setWordWrap(true);
                v->addWidget(lbl);
            }
        }
    }, [this] {
        if (panelReturnTo_ == home_ || panelReturnTo_ == themedHome_) showHomeScreen();
        else stack_->setCurrentWidget(panelReturnTo_);
    });
}

// Ask before wiping everything (this deletes the whole portable install folder, so downloads/saves/music inside
// it go too), then hand off to a detached script that removes the folder once we've exited.
void MainWindow::confirmUninstall()
{
    const QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    // An in-window confirm card (controller-navigable, no OS dialog); Cancel is focused and Back cancels.
    const int choice = NavConfirm::ask(
        tr("Permanently remove My Media Vault and all of its data?"),
        tr("This deletes the whole app folder:\n%1\n\n"
           "That includes your settings, cloud sign-in, downloaded games/music, emulator saves and save "
           "states, and installed emulators/cores, plus the cache and crash logs. This cannot be undone.\n\n"
           "If you want to keep any downloads, copy them out of that folder first.").arg(dir),
        { tr("Uninstall"), tr("Cancel") }, /*focusIndex=*/1, /*cancelIndex=*/1, this);
    if (choice == 0) performUninstall();
}

void MainWindow::performUninstall()
{
#if defined(Q_OS_WIN)
    const QString installDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString cacheDir   = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const QString localApp   = qEnvironmentVariable("LOCALAPPDATA");
    const qint64  pid        = QCoreApplication::applicationPid();

    // Run from %TEMP% (outside the install dir) so it can delete the install dir AND then delete itself. Waits for
    // our process to exit first — we can't remove the running exe's folder from inside the app.
    QString del = QStringLiteral(
        "@echo off\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq %1\" 2>NUL | find \"%1\" >NUL && ( timeout /t 1 /nobreak >NUL & goto wait )\r\n"
        "rmdir /S /Q \"%2\" 2>NUL\r\n").arg(QString::number(pid), installDir);
    if (!cacheDir.isEmpty())  del += QStringLiteral("rmdir /S /Q \"%1\" 2>NUL\r\n").arg(cacheDir);
    del += QStringLiteral("reg delete \"HKCU\\SOFTWARE\\Xenia\" /f >NUL 2>&1\r\n"); // the Xenia disclaimer flag we set
    if (!localApp.isEmpty())
        del += QStringLiteral("del /Q \"%1\\CrashDumps\\MyMediaVault.exe.*.dmp\" >NUL 2>&1\r\n")
                   .arg(QDir::toNativeSeparators(localApp));
    del += QStringLiteral("(goto) 2>nul & del \"%~f0\"\r\n"); // self-delete this script

    const QString cmdPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                .filePath(QStringLiteral("mmv-uninstall.cmd"));
    QFile cf(cmdPath);
    if (!cf.open(QIODevice::WriteOnly | QIODevice::Text))
    { notify(tr("Couldn't start the uninstaller."), kFeedbackLong); return; }
    cf.write(del.toLocal8Bit());
    cf.close();

    // Detached + minimized so it outlives us; then quit hard (skip the cloud-push-on-exit — everything's going).
    QProcess::startDetached(QStringLiteral("cmd"),
        { QStringLiteral("/c"), QStringLiteral("start"), QString(), QStringLiteral("/min"),
          QStringLiteral("cmd"), QStringLiteral("/c"), QDir::toNativeSeparators(cmdPath) });
    forceClose_ = true;
    mwLog(QStringLiteral("quit: uninstall"));
    qApp->quit();
#else
    // macOS/Linux: the app is a bundle/AppImage the user removes by deleting it. Clear our data dir + cache + quit.
    QDir(QCoreApplication::applicationDirPath()).removeRecursively();
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    forceClose_ = true;
    mwLog(QStringLiteral("quit: uninstall"));
    qApp->quit();
#endif
}

// Human-readable byte counts for the download rows ("612 MB", "1.4 GB").
static QString humanBytes(qint64 n)
{
    if (n <= 0) return QStringLiteral("—");
    const double kb = 1024.0;
    if (n < kb * kb)        return QStringLiteral("%1 KB").arg(n / kb, 0, 'f', 0);
    if (n < kb * kb * kb)   return QStringLiteral("%1 MB").arg(n / (kb * kb), 0, 'f', n < 10 * kb * kb ? 1 : 0);
    return QStringLiteral("%1 GB").arg(n / (kb * kb * kb), 0, 'f', 1);
}

// One line of status text for a job: "Downloading — 612 MB / 1.4 GB (43%)", "Paused — …", "Failed — <reason>".
static QString downloadStatusText(const DownloadJob& j)
{
    const QString have = humanBytes(j.received);
    const QString tot  = j.total > 0 ? humanBytes(j.total) : QStringLiteral("?");
    const int pct = j.total > 0 ? int(qRound(100.0 * double(j.received) / double(j.total))) : 0;
    switch (j.state) {
        case DownloadJob::Queued:  return MainWindow::tr("Queued");
        case DownloadJob::Active:  return MainWindow::tr("Downloading — %1 / %2 (%3%)").arg(have, tot).arg(pct);
        case DownloadJob::Paused:  return MainWindow::tr("Paused — %1 / %2").arg(have, tot);
        case DownloadJob::Failed:  return MainWindow::tr("Failed — %1").arg(j.error.isEmpty() ? MainWindow::tr("download stopped") : j.error);
        case DownloadJob::Done:    return MainWindow::tr("Done — %1").arg(tot);
    }
    return QString();
}

// Settings ▸ Downloads. Lists every persistent download job with a live progress bar and the actions that make
// sense for its state (Pause/Resume, Retry, Cancel, Remove). Fully navigable by the panel nav ring — arrow keys,
// a controller D-pad, or the mouse. Rebuilt on state changes; progress ticks update the bars in place.
void MainWindow::openDownloadManager()
{
#ifdef MMV_HAVE_QML
    // Themed mode: each job = one Progress row (label=title, progress=pct, value=status text). Activating a job
    // row opens a NavMenu action chooser (the established overlay — like the XMB game menu) with that job's
    // applicable actions (Pause/Resume/Retry/Cancel/Remove per the SAME state logic classic uses to decide its
    // per-job buttons), calling the SAME DownloadManager methods (showDownloadActionMenu). Empty state -> an Info
    // row. LIVE: jobProgress ticks patch the row in place via updateDownloadRow -> host updateRow("dl:"+id), which
    // no-ops when no stacked panel carries the row (the themed analogue of the classic dlPanelOpen_ guard). State
    // changes rebuild in place via replaceTop (reentry). Downloads is a hub child -> nested present(), Back ->
    // openSettingsHub. The classic dlPanelOpen_/dlBars_ machinery below is UNTOUCHED (classic mode).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        themedPanelHost_->setStyle(settingsPanelStyle());
        const QVector<DownloadJob>& jobs = dm_->jobs();

        QVector<PanelRow> rows;
        bool anyFinished = false;
        if (jobs.isEmpty())
        {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("dl.empty");
            r.label = tr("No downloads yet.");
            r.value = tr("Choose “Download” on a game, movie or book.");
            rows << r;
        }
        for (const DownloadJob& j : jobs)
        {
            if (j.state == DownloadJob::Done || j.state == DownloadJob::Failed) anyFinished = true;
            PanelRow r; r.kind = PanelRow::Progress; r.id = QStringLiteral("dl:") + j.id;
            r.label = j.title.isEmpty() ? tr("(untitled)") : j.title;
            r.value = downloadStatusText(j);
            r.progress = j.total > 0 ? int(qRound(100.0 * double(j.received) / double(j.total))) : 0;
            rows << r;
        }
        if (anyFinished)
        {
            PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("dl.clearfinished");
            r.label = tr("Clear finished"); rows << r;
        }

        auto onAct = [this](const QString& id, const QString&) {
            if (id == QStringLiteral("dl.clearfinished")) { dm_->clearFinished(); return; }
            if (id.startsWith(QStringLiteral("dl:")))      showDownloadActionMenu(id.mid(3));
        };
        auto onBack = [this] { openSettingsHub(); };   // Downloads is a hub child — Back pops to the hub

        if (themedPanelHost_->panelTitle() == tr("Downloads"))
            themedPanelHost_->replaceTop(tr("Downloads"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("Downloads"), rows, onAct, onBack);

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    dlBars_.clear();
    dlStatus_.clear();

    showPanel(tr("Downloads"), [this](QVBoxLayout* v) {
        const QVector<DownloadJob>& jobs = dm_->jobs();

        bool anyFinished = false;
        if (jobs.isEmpty()) {
            auto* none = new QLabel(tr("No downloads yet.\nChoose “Download” on a game, movie, or book to keep a copy here."));
            none->setWordWrap(true);
            none->setStyleSheet(QStringLiteral("color:#667085;font-size:16px;padding:8px 4px;"));
            v->addWidget(none);
        }

        for (const DownloadJob& job : jobs) {
            const DownloadJob j = job; // capture by value for the button lambdas
            if (j.state == DownloadJob::Done || j.state == DownloadJob::Failed) anyFinished = true;

            auto* card = new QFrame;
            card->setStyleSheet(QStringLiteral(
                "QFrame{border:1px solid rgba(0,0,0,0.12);border-radius:12px;background:rgba(0,0,0,0.03);}"));
            auto* cv = new QVBoxLayout(card);
            cv->setContentsMargins(16, 14, 16, 14);
            cv->setSpacing(8);

            auto* title = new QLabel(j.title.isEmpty() ? tr("(untitled)") : j.title);
            title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;border:none;background:transparent;"));
            title->setWordWrap(true);
            cv->addWidget(title);

            auto* bar = new QProgressBar;
            bar->setTextVisible(false);
            bar->setFixedHeight(8);
            bar->setRange(0, j.total > 0 ? 1000 : 0); // busy indicator until we know the size
            if (j.total > 0) bar->setValue(int(1000.0 * double(j.received) / double(j.total)));
            bar->setStyleSheet(QStringLiteral(
                "QProgressBar{border:none;border-radius:4px;background:rgba(0,0,0,0.10);}"
                "QProgressBar::chunk{border-radius:4px;background:#2C72C9;}"));
            cv->addWidget(bar);
            dlBars_.insert(j.id, bar);

            auto* status = new QLabel(downloadStatusText(j));
            status->setStyleSheet(QStringLiteral("color:#667085;font-size:14px;border:none;background:transparent;"));
            status->setWordWrap(true);
            cv->addWidget(status);
            dlStatus_.insert(j.id, status);

            // Action buttons — compact, focusable, so the panel nav ring picks them up in order.
            auto* row = new QHBoxLayout;
            row->setSpacing(10);
            auto actionBtn = [](const QString& text) {
                auto* b = new QPushButton(text);
                b->setMinimumHeight(40);
                b->setCursor(Qt::PointingHandCursor);
                b->setStyleSheet(QStringLiteral(
                    "QPushButton{font-size:15px;padding:8px 18px;border:1px solid rgba(0,0,0,0.15);"
                    "border-radius:9px;background:rgba(0,0,0,0.04);} QPushButton:hover{background:rgba(0,0,0,0.10);}"
                    "QPushButton:focus{border:2px solid #2C72C9;background:rgba(44,114,201,0.10);}"));
                return b;
            };
            const QString id = j.id;
            if (j.state == DownloadJob::Active || j.state == DownloadJob::Queued) {
                auto* b = actionBtn(tr("Pause"));
                connect(b, &QPushButton::clicked, this, [this, id] { dm_->pauseJob(id); });
                row->addWidget(b);
            }
            if (j.state == DownloadJob::Paused) {
                auto* b = actionBtn(tr("Resume"));
                connect(b, &QPushButton::clicked, this, [this, id] { dm_->resumeJob(id); });
                row->addWidget(b);
            }
            if (j.state == DownloadJob::Failed) {
                auto* b = actionBtn(tr("Retry"));
                connect(b, &QPushButton::clicked, this, [this, id] { dm_->retry(id); });
                row->addWidget(b);
            }
            if (j.state == DownloadJob::Active || j.state == DownloadJob::Queued || j.state == DownloadJob::Paused) {
                auto* b = actionBtn(tr("Cancel"));
                connect(b, &QPushButton::clicked, this, [this, id] { dm_->cancel(id); });
                row->addWidget(b);
            }
            if (j.state == DownloadJob::Failed || j.state == DownloadJob::Done) {
                auto* b = actionBtn(tr("Remove"));
                connect(b, &QPushButton::clicked, this, [this, id] { dm_->removeJob(id); });
                row->addWidget(b);
            }
            row->addStretch(1);
            cv->addLayout(row);
            v->addWidget(card);
        }

        if (anyFinished) {
            auto* clear = panelRow(tr("Clear finished"));
            connect(clear, &QPushButton::clicked, this, [this] { dm_->clearFinished(); });
            v->addWidget(clear);
        }
    }, [this] {
        dlPanelOpen_ = false;
        openSettingsHub();
    });
    dlPanelOpen_ = true; // set after showPanel (which clears it); the bars are now built and safe to update live
}

// Refresh a single job's progress bar + status line in place (called on every jobProgress tick — cheap, and it
// leaves keyboard/controller focus undisturbed, unlike a full panel rebuild).
void MainWindow::updateDownloadRow(const QString& id)
{
#ifdef MMV_HAVE_QML
    // Themed: patch the job's Progress row in place (no rebuild, cursor undisturbed). host->updateRow no-ops when
    // no stacked panel carries "dl:"+id — the themed analogue of the classic dlPanelOpen_ guard: a tick that
    // arrives after the user left Downloads finds no matching row and returns false (no stray update, no crash).
    if (themedHomeEnabled() && themedPanelHost_)
    {
        for (const DownloadJob& j : dm_->jobs()) {
            if (j.id != id) continue;
            PanelRow r; r.kind = PanelRow::Progress; r.id = QStringLiteral("dl:") + id;
            r.label = j.title.isEmpty() ? tr("(untitled)") : j.title;
            r.value = downloadStatusText(j);
            r.progress = j.total > 0 ? int(qRound(100.0 * double(j.received) / double(j.total))) : 0;
            themedPanelHost_->updateRow(QStringLiteral("dl:") + id, r);
            return;
        }
        return;
    }
#endif
    if (!dlPanelOpen_) return;
    for (const DownloadJob& j : dm_->jobs()) {
        if (j.id != id) continue;
        if (QProgressBar* bar = dlBars_.value(id)) {
            if (j.total > 0) { bar->setRange(0, 1000); bar->setValue(int(1000.0 * double(j.received) / double(j.total))); }
        }
        if (QLabel* s = dlStatus_.value(id)) s->setText(downloadStatusText(j));
        return;
    }
}

#ifdef MMV_HAVE_QML
// Themed Downloads: activating a job's Progress row opens a NavMenu action chooser with exactly the actions the
// classic panel would show as per-job buttons for that job's state — calling the SAME DownloadManager methods.
// The overlay mirrors itself as a level on the panel host's own NavGraph (like Osk::getText does), so Back closes
// the menu only and the panel graph depth tracks reality. Freed-pointer discipline: the job is looked up by id
// here AND again in the chosen-action lambda (the list can change between open and choice — never cache a job).
void MainWindow::showDownloadActionMenu(const QString& id)
{
    const DownloadJob* job = nullptr;
    for (const DownloadJob& j : dm_->jobs()) if (j.id == id) { job = &j; break; }
    if (!job) return;
    const DownloadJob::State st = job->state;

    QStringList labels;
    QVector<int> acts;                 // parallel to labels; the DownloadJob::State-driven action for each row
    enum { A_Pause, A_Resume, A_Retry, A_Cancel, A_Remove };
    if (st == DownloadJob::Active || st == DownloadJob::Queued)                     { labels << tr("Pause");  acts << A_Pause; }
    if (st == DownloadJob::Paused)                                                  { labels << tr("Resume"); acts << A_Resume; }
    if (st == DownloadJob::Failed)                                                  { labels << tr("Retry");  acts << A_Retry; }
    if (st == DownloadJob::Active || st == DownloadJob::Queued || st == DownloadJob::Paused) { labels << tr("Cancel"); acts << A_Cancel; }
    if (st == DownloadJob::Failed || st == DownloadJob::Done)                       { labels << tr("Remove"); acts << A_Remove; }
    if (labels.isEmpty()) return;      // nothing applicable (shouldn't happen — every state has an action)

    const QString title = job->title.isEmpty() ? tr("Download") : job->title;
    auto* menu = new NavMenu(title, labels, [this, id, acts](int row) {
        if (row < 0 || row >= acts.size()) return;             // backed out
        switch (acts[row]) {
            case A_Pause:  dm_->pauseJob(id);  break;
            case A_Resume: dm_->resumeJob(id); break;
            case A_Retry:  dm_->retry(id);     break;
            case A_Cancel: dm_->cancel(id);    break;
            case A_Remove: dm_->removeJob(id); break;
        }
    }, this);
    menu->setNavGraph(themedPanelHost_ ? themedPanelHost_->navGraph() : nullptr);
}
#endif

void MainWindow::openGeneralSettings()
{
#ifdef MMV_HAVE_QML
    // Themed mode: render General as a flat PanelRow descriptor list on the Nav Contract (ThemedPanelHost),
    // instead of the classic QWidget builder. Every row writes the SAME Settings key via the SAME setter the
    // classic handler used. This is a NESTED present() (the hub is already up at level 1) — NO reset(), so Back
    // is a graph-level pop that renderTop(restore)s the hub to the row we entered from (the first live exercise
    // of Task 1's pop-restore path). Section headers -> Separator rows; the subtitle-language combo -> a Choice;
    // credentials + the ROMs path/actions -> TextField/Action rows; the parental PIN keeps Osk::getText nesting.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        // Drop any Trakt live-status hookups from a previous presentation (the host persists — classic's child
        // labels auto-disconnected on teardown; we manage ours). Re-added after present() below.
        for (const QMetaObject::Connection& c : genSettingsConns_) disconnect(c);
        genSettingsConns_.clear();

        if (bgm_) bgm_->reload();                          // rescan music, exactly as the classic builder does
        themedPanelHost_->setStyle(settingsPanelStyle());  // active theme's settingsPanel block (hard fallbacks)

        // Subtitle language table (display <-> code). The Choice cycles the display names; the handler maps the
        // picked display back to its code via this same pair list (so a prior "(custom)" code round-trips exact).
        const QList<QPair<QString, QString>> langs = {
            { tr("Any / first available"), QString() }, { QStringLiteral("English"), QStringLiteral("eng") },
            { QStringLiteral("Spanish"), QStringLiteral("spa") }, { QStringLiteral("French"), QStringLiteral("fra") },
            { QStringLiteral("German"), QStringLiteral("deu") }, { QStringLiteral("Italian"), QStringLiteral("ita") },
            { QStringLiteral("Portuguese"), QStringLiteral("por") }, { QStringLiteral("Dutch"), QStringLiteral("nld") },
            { QStringLiteral("Russian"), QStringLiteral("rus") }, { QStringLiteral("Japanese"), QStringLiteral("jpn") },
            { QStringLiteral("Korean"), QStringLiteral("kor") }, { QStringLiteral("Chinese"), QStringLiteral("zho") },
            { QStringLiteral("Arabic"), QStringLiteral("ara") },
        };
        const QString curLang = Settings::subtitleLanguage();
        QList<QPair<QString, QString>> langOptPairs = langs;   // captured by the handler for display->code mapping
        QString curLangDisp;
        for (const auto& l : langs) if (l.second == curLang) { curLangDisp = l.first; break; }
        if (curLangDisp.isEmpty() && !curLang.isEmpty()) {     // keep a previously-set code the list doesn't carry
            curLangDisp = tr("%1 (custom)").arg(curLang);
            langOptPairs << qMakePair(curLangDisp, curLang);
        }
        if (curLangDisp.isEmpty()) curLangDisp = langs.first().first;
        QStringList langOpts;
        for (const auto& p : langOptPairs) langOpts << p.first;

        // Background-music volume: the contract has no Slider row, so the continuous 0..100 slider becomes a
        // discrete Choice in 10% steps (documented gap — see report). Same write path (setBgmVolume/setVolume).
        QStringList volOpts;
        for (int p = 0; p <= 100; p += 10) volOpts << QStringLiteral("%1%").arg(p);
        const QString curVolDisp = QStringLiteral("%1%").arg(int(qRound(Settings::bgmVolume() / 10.0) * 10));

        // External-player choice: Built-in + each detected desktop player (VLC/MPC) + Custom. (Android build:
        // Built-in + "Ask another app…".) The Choice delivers the display string; this pair list maps it back
        // to the stored key so nothing but builtin/vlc/mpc/custom is ever written on desktop — the "android"
        // key never appears in a desktop options list, so a desktop ini can't acquire it here.
        QList<QPair<QString, QString>> playerOptPairs;
        playerOptPairs << qMakePair(tr("Built-in player"), QStringLiteral("builtin"));
#ifdef Q_OS_ANDROID
        playerOptPairs << qMakePair(tr("Ask another app…"), QStringLiteral("android"));
#else
        for (const ExternalPlayer::Detected& d : ExternalPlayer::detect())
        {
            if      (d.kind == ExternalPlayer::Kind::Vlc) playerOptPairs << qMakePair(d.display, QStringLiteral("vlc"));
            else if (d.kind == ExternalPlayer::Kind::Mpc) playerOptPairs << qMakePair(d.display, QStringLiteral("mpc"));
        }
        playerOptPairs << qMakePair(tr("Custom…"), QStringLiteral("custom"));
#endif
        const QString curPlayerKey = Settings::externalPlayer();
        QString curPlayerDisp;
        for (const auto& p : playerOptPairs) if (p.second == curPlayerKey) { curPlayerDisp = p.first; break; }
        // A configured kind whose player isn't currently detected (e.g. VLC uninstalled) — keep it shown so the
        // setting is visible and changeable rather than silently snapping to Built-in.
        if (curPlayerDisp.isEmpty())
        {
            if (curPlayerKey == QStringLiteral("vlc"))      curPlayerDisp = tr("VLC media player");
            else if (curPlayerKey == QStringLiteral("mpc")) curPlayerDisp = tr("MPC-HC");
            if (!curPlayerDisp.isEmpty()) playerOptPairs << qMakePair(curPlayerDisp, curPlayerKey);
            else curPlayerDisp = playerOptPairs.first().first; // builtin/unknown -> Built-in
        }
        QStringList playerOpts;
        for (const auto& p : playerOptPairs) playerOpts << p.first;

        QVector<PanelRow> rows;
        auto sep    = [&rows](const QString& t) { PanelRow r; r.kind = PanelRow::Separator; r.label = t; rows << r; };
        auto info   = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto toggle = [&rows](const QString& id, const QString& label, bool on) {
            PanelRow r; r.kind = PanelRow::Toggle; r.id = id; r.label = label; r.checked = on; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        auto textf  = [&rows](const QString& id, const QString& label, const QString& value, bool masked = false) {
            PanelRow r; r.kind = PanelRow::TextField; r.id = id; r.label = label; r.value = value; r.masked = masked; rows << r; };
        auto choice = [&rows](const QString& id, const QString& label, const QStringList& opts, const QString& cur) {
            PanelRow r; r.kind = PanelRow::Choice; r.id = id; r.label = label; r.options = opts; r.value = cur; rows << r; };

        // --- Display ---
        sep(tr("Display"));
        toggle(QStringLiteral("disp.fullscreen"), tr("Open in full screen on startup"), Settings::startFullscreen());
        // --- Library ---
        sep(tr("Library"));
        // Global (not per-profile) override: reveal items any profile has marked hidden from the detail view.
        toggle(QStringLiteral("lib.showhidden"), tr("Show hidden items"),
               store().value(QStringLiteral("library/showHidden"), false).toBool());
        // --- Updates ---
        sep(tr("Updates"));
        info(QStringLiteral("update.version"), tr("Version"), AppUpdater::currentVersion());
        toggle(QStringLiteral("update.autocheck"), tr("Check for updates on startup"), Settings::checkUpdatesOnStartup());
        action(QStringLiteral("update.check"), tr("Check for updates now"));
        action(QStringLiteral("update.install"), (updater_ && updater_->updatePending())
                   ? tr("Install %1 and restart").arg(updater_->latestVersion()) : tr("Install update"));
        info(QStringLiteral("update.status"), tr("Status"), QString());
        // --- Game ROMs ---
        sep(tr("Game ROMs"));
        info(QStringLiteral("roms.path"), Settings::romsFolder(), QString());
        action(QStringLiteral("roms.change"), tr("Change ROMs folder…"));
        action(QStringLiteral("roms.open"), tr("Open ROMs folder"));
        toggle(QStringLiteral("roms.keepscrape"), tr("Keep scraped data in the ROMs folder (gamelist.xml)"),
               Settings::keepScrapedData());
        // --- Local Library (movies + TV) ---
        sep(tr("Local Library"));
        info(QStringLiteral("library.path"), Settings::libraryFolder(), QString());
        action(QStringLiteral("library.change"), tr("Change Local Library folder…"));
        action(QStringLiteral("library.rescan"), tr("Rescan Local Library"));
        toggle(QStringLiteral("library.resolveonline"), tr("Match local files to online catalogs"),
               Settings::resolveOnline());
        action(QStringLiteral("library.rematch"), tr("Re-match Local Library online"));
        // --- Playback ---
        sep(tr("Playback"));
        toggle(QStringLiteral("pb.autonext"), tr("Auto-play the next episode"), Settings::autoplayNextEpisode());
        // Videos play in the built-in player by default, or hand off to an installed/custom external player.
        // Hidden ENTIRELY for a restricted (kids) profile — no external escape hatch offered, PIN or not.
        if (!ProfileStore::current().restricted)
        {
            choice(QStringLiteral("player.external"), tr("Play videos with"), playerOpts, curPlayerDisp);
            action(QStringLiteral("player.custompath"), Settings::externalPlayerPath().isEmpty()
                       ? tr("Choose custom player program…")
                       : tr("Custom player: %1").arg(QFileInfo(Settings::externalPlayerPath()).fileName()));
        }
        toggle(QStringLiteral("pb.bezel"), tr("Show bezel / border art around games"), Settings::bezelEnabled());
        action(QStringLiteral("pb.bezelopen"), tr("Open bezels folder"));
        // --- Subtitles ---
        sep(tr("Subtitles"));
        toggle(QStringLiteral("subs.on"), tr("Show subtitles by default"), Settings::subtitlesOnByDefault());
        choice(QStringLiteral("subs.lang"), tr("Default language"), langOpts, curLangDisp);
        // --- Auto-download from OpenSubtitles (the password is masked — dots in the row; the OSK is unchanged) ---
        sep(tr("Auto-download from OpenSubtitles"));
        textf(QStringLiteral("os.api"), tr("API key"), Settings::openSubApiKey());
        textf(QStringLiteral("os.user"), tr("Username"), Settings::openSubUsername());
        textf(QStringLiteral("os.pass"), tr("Password"), Settings::openSubPassword(), /*masked=*/true);
        // --- Trakt.tv ---
        sep(tr("Trakt.tv"));
        textf(QStringLiteral("trakt.id"), tr("Client ID"), Settings::traktClientId());
        textf(QStringLiteral("trakt.secret"), tr("Client secret"), Settings::traktClientSecret(), /*masked=*/true);
        action(QStringLiteral("trakt.connect"), TraktClient::connected() ? tr("Disconnect from Trakt")
                                                                          : tr("Connect to Trakt"));
        info(QStringLiteral("trakt.status"), tr("Status"), TraktClient::connected() ? tr("Connected")
                                                                                     : tr("Not connected"));
        // --- Parental Controls (the PIN flows keep Osk::getText nesting exactly) ---
        sep(tr("Parental Controls"));
        info(QStringLiteral("parental.status"), tr("PIN"), Settings::hasParentalPin() ? tr("A PIN is set")
                                                                                       : tr("No PIN set"));
        action(QStringLiteral("parental.setpin"), Settings::hasParentalPin() ? tr("Change PIN") : tr("Set PIN"));
        action(QStringLiteral("parental.clearpin"), tr("Remove PIN"));
        info(QStringLiteral("parental.profileshdr"), tr("Restricted (kids) profiles"), QString());
        for (const Profile& pr : ProfileStore::list())
            toggle(QStringLiteral("profile:") + pr.id,
                   (pr.icon.isEmpty() ? QString() : pr.icon + QStringLiteral("  ")) + pr.name, pr.restricted);
        // --- Background Music ---
        sep(tr("Background Music"));
        toggle(QStringLiteral("bgm.on"), tr("Play background music"), Settings::bgmEnabled());
        choice(QStringLiteral("bgm.vol"), tr("Volume"), volOpts, curVolDisp);
        action(QStringLiteral("bgm.open"), tr("Open music folder"));
        // --- Steam (achievements + owned library) ---
        // One Steam Web API key serves both PC-game achievements and the owned-not-installed library on the Steam
        // console; the SteamID (64-bit) enables the latter. Key is MASKED. (Owned-games UI lives here on General
        // rather than an addon-config surface: it's a native feature, not a manifest-driven addon setting.)
        sep(tr("Steam (achievements + owned library)"));
        textf(QStringLiteral("steam.key"), tr("Steam Web API key"),
              Settings::steamWebApiKey(), /*masked=*/true);
        textf(QStringLiteral("steam.steamid"), tr("SteamID (64-bit) — shows owned, not-installed games"),
              Settings::steamId());
        // --- Streaming (Debrid) ---
        sep(tr("Streaming (Debrid)"));
        textf(QStringLiteral("debrid.torbox"), tr("TorBox API key"),
              store().value(QStringLiteral("debrid/torbox/apikey")).toString());

        // Small helpers to patch a status Info / an Action label in place (updateRow replaces the whole row).
        auto setInfo = [this](const QString& id, const QString& caption, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = caption; r.value = value;
            themedPanelHost_->updateRow(id, r); };
        auto setAction = [this](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label;
            themedPanelHost_->updateRow(id, r); };

        themedPanelHost_->present(tr("General"), rows,
            [this, langOptPairs, playerOptPairs, setInfo, setAction](const QString& id, const QString& val) {
                const bool on = (val == QStringLiteral("1"));   // Toggle rows deliver "1"/"0"
                if (id == QStringLiteral("disp.fullscreen")) {
                    Settings::setStartFullscreen(on);
                    if (on) showFullScreen(); else if (isFullScreen()) leaveFullScreen();
                }
                else if (id == QStringLiteral("lib.showhidden")) {
                    store().setValue(QStringLiteral("library/showHidden"), on);
                    store().sync();                 // flush so HomeView's QSettings sees it on the refresh below
                    home_->reloadForFilterChange(); // hidden rows appear/disappear on the live surface at once
                }
                else if (id == QStringLiteral("update.autocheck")) Settings::setCheckUpdatesOnStartup(on);
                else if (id == QStringLiteral("update.check")) {
                    if (!updater_) return;
                    setInfo(QStringLiteral("update.status"), tr("Status"), tr("Checking…"));
                    connect(updater_, &AppUpdater::updateAvailable, this,
                        [this, setInfo, setAction](const QString& ver, const QString&) {
                            setInfo(QStringLiteral("update.status"), tr("Status"), tr("Version %1 is available.").arg(ver));
                            setAction(QStringLiteral("update.install"), tr("Install %1 and restart").arg(ver));
                        }, Qt::SingleShotConnection);
                    connect(updater_, &AppUpdater::upToDate, this, [this, setInfo] {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("You're already on the latest version."));
                    }, Qt::SingleShotConnection);
                    connect(updater_, &AppUpdater::checkFailed, this, [this, setInfo](const QString& why) {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("Couldn't check for updates: %1").arg(why));
                    }, Qt::SingleShotConnection);
                    updater_->checkForUpdate();
                }
                else if (id == QStringLiteral("update.install")) {
                    if (updater_ && updater_->updatePending()) {
                        setInfo(QStringLiteral("update.status"), tr("Status"),
                                tr("Downloading and installing… the app will restart."));
                        updater_->downloadAndApply();
                    } else {
                        setInfo(QStringLiteral("update.status"), tr("Status"), tr("No update ready — check first."));
                    }
                }
                else if (id == QStringLiteral("roms.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the ROMs folder"),
                                                                          Settings::romsFolder());
                    if (dir.isEmpty()) return;
                    Settings::setRomsFolder(dir);
                    setInfo(QStringLiteral("roms.path"), dir, QString());
                    RomLibrary::ensureStructure();
                    const int added = RomLibrary::syncToDownloads();
                    statusBar()->showMessage(added > 0
                        ? tr("ROMs folder set to %1 — added %n game(s) to Downloaded.", "", added).arg(dir)
                        : tr("ROMs folder set to %1").arg(dir), 6000);
                }
                else if (id == QStringLiteral("roms.open")) {
                    RomLibrary::ensureStructure();
                    QDesktopServices::openUrl(QUrl::fromLocalFile(RomLibrary::root()));
                }
                else if (id == QStringLiteral("library.change")) {
                    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your local video library folder"),
                                                                          Settings::libraryFolder());
                    if (dir.isEmpty()) return;
                    Settings::setLibraryFolder(dir);
                    setInfo(QStringLiteral("library.path"), dir, QString());
                    rescanLocalLibrary();
                    statusBar()->showMessage(tr("Local Library folder set to %1 — rescanning…").arg(dir), 6000);
                }
                else if (id == QStringLiteral("library.rescan")) {
                    rescanLocalLibrary();
                    statusBar()->showMessage(tr("Rescanning your Local Library…"), 4000);
                }
                else if (id == QStringLiteral("library.resolveonline")) {
                    Settings::setResolveOnline(on);
                    if (on && resolver_) resolver_->enqueue(LocalLibrary::index().all());
                }
                else if (id == QStringLiteral("library.rematch")) {
                    // With resolveOnline off, clearing the cache then enqueue() (which no-ops) would
                    // wipe every resolved id on the next rebuild and never re-resolve — badges vanish.
                    // Require the toggle on before clearing.
                    if (!Settings::resolveOnline()) {
                        statusBar()->showMessage(tr("Turn on \"Match local files to online catalogs\" first."), 4000);
                    } else {
                        if (resolver_) resolver_->clearCacheAndRequeue(LocalLibrary::index().all());
                        statusBar()->showMessage(tr("Re-matching your Local Library online…"), 4000);
                    }
                }
                else if (id == QStringLiteral("roms.keepscrape")) Settings::setKeepScrapedData(on);
                else if (id == QStringLiteral("pb.autonext")) Settings::setAutoplayNextEpisode(on);
                else if (id == QStringLiteral("player.external")) {
                    QString key = val;                              // map the picked display back to the stored key
                    for (const auto& p : playerOptPairs) if (p.first == val) { key = p.second; break; }
                    Settings::setExternalPlayer(key);
                    // Choosing "Custom…" with no path yet: prompt for the program path right away (the on-screen
                    // keyboard, same nesting as the parental PIN) so the setting isn't left pointing nowhere.
                    if (key == QStringLiteral("custom") && Settings::externalPlayerPath().isEmpty()) {
                        const QString picked = Osk::getText(tr("Path to the player program:"), QString(),
                                                            QLineEdit::Normal, this, themedPanelHost_->navGraph());
                        if (!picked.isNull() && !picked.trimmed().isEmpty()) {
                            Settings::setExternalPlayerPath(picked.trimmed());
                            setAction(QStringLiteral("player.custompath"),
                                      tr("Custom player: %1").arg(QFileInfo(picked.trimmed()).fileName()));
                        }
                    }
                }
                else if (id == QStringLiteral("player.custompath")) {
                    const QString exe = QFileDialog::getOpenFileName(this, tr("Choose a media player program"),
                        QString(),
#ifdef Q_OS_WIN
                        tr("Programs (*.exe);;All files (*.*)"));
#else
                        tr("All files (*.*)"));
#endif
                    if (exe.isEmpty()) return;
                    Settings::setExternalPlayerPath(exe);
                    Settings::setExternalPlayer(QStringLiteral("custom")); // picking an exe implies Custom mode
                    setAction(QStringLiteral("player.custompath"),
                              tr("Custom player: %1").arg(QFileInfo(exe).fileName()));
                }
                else if (id == QStringLiteral("pb.bezel")) Settings::setBezelEnabled(on);
                else if (id == QStringLiteral("pb.bezelopen")) {
                    const QString d = AppPaths::dataDir() + QStringLiteral("/bezels");
                    QDir().mkpath(d);
                    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
                }
                else if (id == QStringLiteral("subs.on")) Settings::setSubtitlesOnByDefault(on);
                else if (id == QStringLiteral("subs.lang")) {
                    QString code = val;
                    for (const auto& p : langOptPairs) if (p.first == val) { code = p.second; break; }
                    Settings::setSubtitleLanguage(code);
                }
                else if (id == QStringLiteral("os.api"))  Settings::setOpenSubApiKey(val);
                else if (id == QStringLiteral("os.user")) Settings::setOpenSubUsername(val);
                else if (id == QStringLiteral("os.pass")) Settings::setOpenSubPassword(val);
                else if (id == QStringLiteral("trakt.id"))     Settings::setTraktClientId(val);
                else if (id == QStringLiteral("trakt.secret")) Settings::setTraktClientSecret(val);
                else if (id == QStringLiteral("trakt.connect")) {
                    if (TraktClient::connected()) { trakt_->disconnectAccount(); return; }
                    if (!TraktClient::configured()) {
                        setInfo(QStringLiteral("trakt.status"), tr("Status"),
                                tr("Enter your Client ID and Secret first.")); return;
                    }
                    setInfo(QStringLiteral("trakt.status"), tr("Status"), tr("Requesting a code from Trakt…"));
                    trakt_->connectAccount();
                }
                else if (id == QStringLiteral("parental.setpin")) {
                    if (Settings::hasParentalPin()) {
                        const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(),
                                                         QLineEdit::Password, this, themedPanelHost_->navGraph());
                        if (cur.isNull()) return;
                        if (!Settings::checkParentalPin(cur)) {
                            setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("Incorrect PIN.")); return; }
                    }
                    const QString a = Osk::getText(tr("New PIN:"), QString(), QLineEdit::Password, this,
                                                   themedPanelHost_->navGraph());
                    if (a.isNull() || a.isEmpty()) return;
                    const QString b = Osk::getText(tr("Confirm PIN:"), QString(), QLineEdit::Password, this,
                                                   themedPanelHost_->navGraph());
                    if (b.isNull()) return;
                    if (a != b) { setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("PINs didn't match.")); return; }
                    Settings::setParentalPin(a);
                    setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("A PIN is set"));
                    setAction(QStringLiteral("parental.setpin"), tr("Change PIN"));
                }
                else if (id == QStringLiteral("parental.clearpin")) {
                    if (!Settings::hasParentalPin()) {
                        setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("No PIN set")); return; }
                    const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(),
                                                     QLineEdit::Password, this, themedPanelHost_->navGraph());
                    if (cur.isNull()) return;
                    if (!Settings::checkParentalPin(cur)) {
                        setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("Incorrect PIN.")); return; }
                    Settings::setParentalPin(QString());
                    setInfo(QStringLiteral("parental.status"), tr("PIN"), tr("No PIN set"));
                    setAction(QStringLiteral("parental.setpin"), tr("Set PIN"));
                }
                else if (id.startsWith(QStringLiteral("profile:")))
                    ProfileStore::setRestricted(id.mid(8), on);
                else if (id == QStringLiteral("bgm.on")) {
                    Settings::setBgmEnabled(on); if (bgm_) bgm_->setEnabled(on); updateBackgroundMusic();
                }
                else if (id == QStringLiteral("bgm.vol")) {
                    const int p = val.left(val.size() - 1).toInt();   // strip the trailing "%"
                    Settings::setBgmVolume(p); if (bgm_) bgm_->setVolume(p);
                }
                else if (id == QStringLiteral("bgm.open")) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(BackgroundMusic::musicDir()));
                    if (bgm_) { bgm_->reload(); updateBackgroundMusic(); }
                }
                else if (id == QStringLiteral("steam.key")) {
                    Settings::setSteamWebApiKey(val); // never echoed back to the log
                    statusBar()->showMessage(tr("Saved Steam Web API key."), 4000);
                }
                else if (id == QStringLiteral("steam.steamid")) {
                    Settings::setSteamId(val);
                    statusBar()->showMessage(tr("Saved SteamID."), 4000);
                }
                else if (id == QStringLiteral("debrid.torbox")) {
                    store().setValue(QStringLiteral("debrid/torbox/apikey"), val.trimmed()); store().sync();
                }
            },
            [this] { openSettingsHub(); });   // defensive root onBack: General is nested, so a pop re-renders the hub

        // Live Trakt status (persist while the panel is up; dropped at the top of the next present via genSettingsConns_).
        genSettingsConns_ << connect(trakt_, &TraktClient::deviceCode, this,
            [setInfo](const QString& code, const QString& url) {
                setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"),
                        MainWindow::tr("Go to %1 and enter code: %2").arg(url, code)); });
        genSettingsConns_ << connect(trakt_, &TraktClient::connectError, this,
            [setInfo](const QString& m) { setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"), m); });
        genSettingsConns_ << connect(trakt_, &TraktClient::connectedChanged, this,
            [setInfo, setAction](bool conn) {
                setInfo(QStringLiteral("trakt.status"), MainWindow::tr("Status"),
                        conn ? MainWindow::tr("Connected") : MainWindow::tr("Not connected"));
                setAction(QStringLiteral("trakt.connect"),
                          conn ? MainWindow::tr("Disconnect from Trakt") : MainWindow::tr("Connect to Trakt")); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        updateBackgroundMusic();
        return;
    }
#endif

    showPanel(tr("General"), [this](QVBoxLayout* v) {
        // --- Display: open the app full screen on launch. ---
        auto* dispHeading = new QLabel(tr("Display"));
        dispHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dispHeading);
        auto* fs = new QCheckBox(tr("Open in full screen on startup"));
        fs->setStyleSheet(QStringLiteral("font-size:15px;"));
        fs->setChecked(Settings::startFullscreen());
        v->addWidget(fs);
        connect(fs, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setStartFullscreen(c);
            if (c) showFullScreen(); else if (isFullScreen()) leaveFullScreen(); // reflect the choice right away
        });
        v->addSpacing(10);

        // --- Updates: check GitHub Releases and install a newer build in place. ---
        auto* uHeading = new QLabel(tr("Updates"));
        uHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(uHeading);
        auto* uVer = new QLabel(tr("You're on version %1.").arg(AppUpdater::currentVersion()));
        uVer->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(uVer);
        auto* uAuto = new QCheckBox(tr("Check for updates on startup"));
        uAuto->setStyleSheet(QStringLiteral("font-size:15px;"));
        uAuto->setChecked(Settings::checkUpdatesOnStartup());
        v->addWidget(uAuto);
        connect(uAuto, &QCheckBox::toggled, this, [](bool c) { Settings::setCheckUpdatesOnStartup(c); });
        auto* uRow = new QHBoxLayout();
        auto* uCheck = new QPushButton(tr("Check now"));
        auto* uInstall = new QPushButton(tr("Install update"));
        uInstall->setVisible(updater_ && updater_->updatePending());
        if (updater_ && updater_->updatePending())
            uInstall->setText(tr("Install %1 and restart").arg(updater_->latestVersion()));
        uRow->addWidget(uCheck); uRow->addWidget(uInstall); uRow->addStretch(1);
        v->addLayout(uRow);
        auto* uStatus = new QLabel();
        uStatus->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(uStatus);
        // Wire the panel's controls to the shared updater. uStatus is the connection context, so the handlers
        // auto-disconnect when the panel closes; SingleShotConnection drops each after it fires once.
        connect(uCheck, &QPushButton::clicked, this, [this, uStatus, uInstall] {
            uStatus->setText(tr("Checking…"));
            connect(updater_, &AppUpdater::updateAvailable, uStatus, [uStatus, uInstall](const QString& ver, const QString&) {
                uStatus->setText(tr("Version %1 is available.").arg(ver));
                uInstall->setText(tr("Install %1 and restart").arg(ver));
                uInstall->setVisible(true);
            }, Qt::SingleShotConnection);
            connect(updater_, &AppUpdater::upToDate, uStatus, [uStatus] {
                uStatus->setText(tr("You're already on the latest version."));
            }, Qt::SingleShotConnection);
            connect(updater_, &AppUpdater::checkFailed, uStatus, [uStatus](const QString& why) {
                uStatus->setText(tr("Couldn't check for updates: %1").arg(why));
            }, Qt::SingleShotConnection);
            updater_->checkForUpdate();
        });
        connect(uInstall, &QPushButton::clicked, this, [this, uStatus] {
            uStatus->setText(tr("Downloading and installing… the app will restart."));
            updater_->downloadAndApply();
        });
        v->addSpacing(10);

        // --- Game ROMs: a local ROM library laid out RetroBat / ES-DE style (<root>/<system>/roms). ---
        auto* rHeading = new QLabel(tr("Game ROMs"));
        rHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(rHeading);
        auto* rNote = new QLabel(tr("ROMs live in per-system folders under one root (RetroBat / EmulationStation "
            "Desktop Edition layout). Point this anywhere on your system; games then appear under “Local ROMs” "
            "in the Library."));
        rNote->setWordWrap(true); rNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(rNote);
        auto* rRow = new QHBoxLayout();
        auto* rPath = new QLineEdit(Settings::romsFolder());
        rPath->setMinimumHeight(34);
        rPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        rRow->addWidget(rPath, 1);
        auto* rBrowse = new QPushButton(tr("Change…"));
        rRow->addWidget(rBrowse);
        v->addLayout(rRow);
        connect(rBrowse, &QPushButton::clicked, this, [this, rPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the ROMs folder"),
                                                                  Settings::romsFolder());
            if (dir.isEmpty()) return;
            Settings::setRomsFolder(dir);
            rPath->setText(dir);
            RomLibrary::ensureStructure();        // create the per-system sub-folders in the new location
            const int added = RomLibrary::syncToDownloads(); // pull any ROMs already there into Downloaded
            statusBar()->showMessage(added > 0 ? tr("ROMs folder set to %1 — added %n game(s) to Downloaded.", "", added).arg(dir)
                                               : tr("ROMs folder set to %1").arg(dir), 6000);
        });
        auto* rOpen = panelRow(tr("Open ROMs Folder"));
        connect(rOpen, &QPushButton::clicked, this, [this] {
            RomLibrary::ensureStructure(); // make sure the tree exists before we open it
            QDesktopServices::openUrl(QUrl::fromLocalFile(RomLibrary::root()));
        });
        v->addWidget(rOpen);
        auto* keepScrape = new QCheckBox(tr("Keep scraped data in the ROMs folder (EmulationStation gamelist.xml)"));
        keepScrape->setStyleSheet(QStringLiteral("font-size:15px;"));
        keepScrape->setChecked(Settings::keepScrapedData());
        keepScrape->setToolTip(tr("When a game is scraped online, also save its info + art into the system's "
                                  "gamelist.xml + media folders, so it's reused from the folder next time and by "
                                  "other EmulationStation/RetroBat frontends. Existing gamelist data is always read."));
        connect(keepScrape, &QCheckBox::toggled, this, [](bool c) { Settings::setKeepScrapedData(c); });
        v->addWidget(keepScrape);
        v->addSpacing(10);

        // --- Local Library (movies + TV): a folder of local video files surfaced under the video category. ---
        auto* llHeading = new QLabel(tr("Local Library"));
        llHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(llHeading);
        auto* llNote = new QLabel(tr("Point this at a folder of your own movies + TV episodes. They then appear "
            "under “Local Library” in the video category. Use “Rescan” after adding or removing files."));
        llNote->setWordWrap(true); llNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(llNote);
        auto* llRow = new QHBoxLayout();
        auto* llPath = new QLineEdit(Settings::libraryFolder());
        llPath->setMinimumHeight(34);
        llPath->setReadOnly(true); // chosen via the picker, so it's always a real folder
        llRow->addWidget(llPath, 1);
        auto* llBrowse = new QPushButton(tr("Change…"));
        llRow->addWidget(llBrowse);
        auto* llRescan = new QPushButton(tr("Rescan"));
        llRow->addWidget(llRescan);
        v->addLayout(llRow);
        connect(llBrowse, &QPushButton::clicked, this, [this, llPath] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose your local video library folder"),
                                                                  Settings::libraryFolder());
            if (dir.isEmpty()) return;
            Settings::setLibraryFolder(dir);
            llPath->setText(dir);
            rescanLocalLibrary();
            statusBar()->showMessage(tr("Local Library folder set to %1 — rescanning…").arg(dir), 6000);
        });
        connect(llRescan, &QPushButton::clicked, this, [this] {
            rescanLocalLibrary();
            statusBar()->showMessage(tr("Rescanning your local library…"), 4000);
        });
        v->addSpacing(10);

        auto* pbHeading = new QLabel(tr("Playback"));
        pbHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(pbHeading);
        auto* autoNext = new QCheckBox(tr("Auto-play the next episode"));
        autoNext->setStyleSheet(QStringLiteral("font-size:15px;"));
        autoNext->setChecked(Settings::autoplayNextEpisode());
        connect(autoNext, &QCheckBox::toggled, this, [](bool c) { Settings::setAutoplayNextEpisode(c); });
        v->addWidget(autoNext);

        // Play videos with: the built-in player, a detected desktop player (VLC/MPC), or a custom program.
        // Same Settings keys/setters as the themed panel — one write path, no drift. Hidden entirely for a
        // restricted (kids) profile (no external escape hatch), matching the themed panel.
        if (!ProfileStore::current().restricted)
        {
            auto* plRow = new QHBoxLayout();
            auto* plLbl = new QLabel(tr("Play videos with"));
            plLbl->setStyleSheet(QStringLiteral("font-size:15px;"));
            auto* player = new QComboBox();
            player->addItem(tr("Built-in player"), QStringLiteral("builtin"));
#ifdef Q_OS_ANDROID
            player->addItem(tr("Ask another app…"), QStringLiteral("android"));
#else
            for (const ExternalPlayer::Detected& d : ExternalPlayer::detect())
            {
                if      (d.kind == ExternalPlayer::Kind::Vlc) player->addItem(d.display, QStringLiteral("vlc"));
                else if (d.kind == ExternalPlayer::Kind::Mpc) player->addItem(d.display, QStringLiteral("mpc"));
            }
            player->addItem(tr("Custom…"), QStringLiteral("custom"));
#endif
            const QString curKey = Settings::externalPlayer();
            int sel = player->findData(curKey);
            if (sel < 0) { // a configured-but-undetected player: keep it visible
                const QString disp = curKey == QStringLiteral("vlc") ? tr("VLC media player")
                                   : curKey == QStringLiteral("mpc") ? tr("MPC-HC") : QString();
                if (!disp.isEmpty()) { player->addItem(disp, curKey); sel = player->count() - 1; } else sel = 0;
            }
            player->setCurrentIndex(qMax(0, sel));
            auto* plCustom = new QPushButton(tr("Choose custom program…"));
            plRow->addWidget(plLbl); plRow->addWidget(player); plRow->addWidget(plCustom); plRow->addStretch(1);
            v->addLayout(plRow);
            auto* plPath = new QLabel(Settings::externalPlayerPath().isEmpty()
                ? QString() : tr("Custom player: %1").arg(Settings::externalPlayerPath()));
            plPath->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
            v->addWidget(plPath);
            connect(player, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [player](int) { Settings::setExternalPlayer(player->currentData().toString()); });
            connect(plCustom, &QPushButton::clicked, this, [this, player, plPath] {
                const QString exe = QFileDialog::getOpenFileName(this, tr("Choose a media player program"),
                    QString(),
#ifdef Q_OS_WIN
                    tr("Programs (*.exe);;All files (*.*)"));
#else
                    tr("All files (*.*)"));
#endif
                if (exe.isEmpty()) return;
                Settings::setExternalPlayerPath(exe);
                Settings::setExternalPlayer(QStringLiteral("custom"));
                const int ci = player->findData(QStringLiteral("custom"));
                if (ci >= 0) player->setCurrentIndex(ci);
                plPath->setText(tr("Custom player: %1").arg(exe));
            });
        }

        auto* bezel = new QCheckBox(tr("Show bezel / border art around games"));
        bezel->setStyleSheet(QStringLiteral("font-size:15px;"));
        bezel->setChecked(Settings::bezelEnabled());
        connect(bezel, &QCheckBox::toggled, this, [](bool c) { Settings::setBezelEnabled(c); });
        v->addWidget(bezel);
        auto* bezelNote = new QLabel(tr("Drop PNGs into the bezels folder named <core>.png (e.g. fceumm.png) "
                                        "or default.png — with a transparent center where the game shows."));
        bezelNote->setWordWrap(true); bezelNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(bezelNote);
        auto* bezelOpen = new QPushButton(tr("Open bezels folder"));
        connect(bezelOpen, &QPushButton::clicked, this, [] {
            const QString d = AppPaths::dataDir() + QStringLiteral("/bezels");
            QDir().mkpath(d);
            QDesktopServices::openUrl(QUrl::fromLocalFile(d));
        });
        auto* bezelRow = new QHBoxLayout(); bezelRow->addWidget(bezelOpen); bezelRow->addStretch(1);
        v->addLayout(bezelRow);
        v->addSpacing(10);

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

        // --- Auto-download subtitles (OpenSubtitles.com). When a movie/episode has no subtitle in the chosen
        // language, fetch one automatically. Needs a free API key + the user's account (login is required to
        // download). All three blank => the feature stays off. ---
        v->addSpacing(10);
        auto* osHeading = new QLabel(tr("Auto-download from OpenSubtitles"));
        osHeading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        v->addWidget(osHeading);
        auto* osNote = new QLabel(tr("When “show subtitles by default” is on and a video has none in your "
                                     "language, fetch one automatically. Get a free API key at "
                                     "opensubtitles.com (Consumers → New consumer) and sign in with your "
                                     "OpenSubtitles account — a login is required to download."));
        osNote->setWordWrap(true);
        osNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(osNote);

        auto addCredRow = [this, v](const QString& label, const QString& value, bool secret,
                                    std::function<void(const QString&)> save) {
            auto* row = new QHBoxLayout();
            auto* l = new QLabel(label); l->setMinimumWidth(90);
            row->addWidget(l);
            auto* edit = new QLineEdit(value);
            edit->setMinimumHeight(30);
            if (secret) edit->setEchoMode(QLineEdit::Password);
            connect(edit, &QLineEdit::textChanged, this, [save](const QString& t) { save(t); }); // save as typed
            row->addWidget(edit, 1);
            v->addLayout(row);
        };
        addCredRow(tr("API key:"), Settings::openSubApiKey(), false,
                   [](const QString& t) { Settings::setOpenSubApiKey(t); });
        addCredRow(tr("Username:"), Settings::openSubUsername(), false,
                   [](const QString& t) { Settings::setOpenSubUsername(t); });
        addCredRow(tr("Password:"), Settings::openSubPassword(), true,
                   [](const QString& t) { Settings::setOpenSubPassword(t); });

        // --- Trakt.tv scrobbling: mark movies/episodes watched on your Trakt profile as you play them. ---
        v->addSpacing(12);
        auto* tkHeading = new QLabel(tr("Trakt.tv"));
        tkHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(tkHeading);
        auto* tkNote = new QLabel(tr("Sync what you watch to your Trakt profile (movies + episodes are marked "
                                     "watched automatically). Create a free API app at trakt.tv/oauth/applications "
                                     "(redirect URI: urn:ietf:wg:oauth:2.0:oob), paste its Client ID + Secret, then "
                                     "Connect."));
        tkNote->setWordWrap(true);
        tkNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(tkNote);
        addCredRow(tr("Client ID:"), Settings::traktClientId(), false,
                   [](const QString& t) { Settings::setTraktClientId(t); });
        addCredRow(tr("Client secret:"), Settings::traktClientSecret(), true,
                   [](const QString& t) { Settings::setTraktClientSecret(t); });

        auto* tkStatus = new QLabel(TraktClient::connected() ? tr("✓ Connected to Trakt.") : tr("Not connected."));
        tkStatus->setWordWrap(true);
        tkStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* tkBtn = new QPushButton(TraktClient::connected() ? tr("Disconnect") : tr("Connect to Trakt"));
        tkBtn->setMinimumHeight(32);
        auto* tkRow = new QHBoxLayout(); tkRow->addWidget(tkBtn); tkRow->addStretch(1);
        v->addLayout(tkRow);
        v->addWidget(tkStatus);
        // Wire this panel's Trakt signals; disconnected when the panel is torn down (the labels are its children).
        connect(trakt_, &TraktClient::deviceCode, tkStatus, [tkStatus](const QString& code, const QString& url) {
            tkStatus->setText(tr("Go to %1 and enter code:  %2").arg(url, code)); });
        connect(trakt_, &TraktClient::connectError, tkStatus, [tkStatus](const QString& m) { tkStatus->setText(m); });
        connect(trakt_, &TraktClient::connectedChanged, tkBtn, [tkBtn, tkStatus](bool on) {
            tkBtn->setText(on ? tr("Disconnect") : tr("Connect to Trakt"));
            tkStatus->setText(on ? tr("✓ Connected to Trakt.") : tr("Not connected.")); });
        connect(tkBtn, &QPushButton::clicked, this, [this, tkStatus] {
            if (TraktClient::connected()) { trakt_->disconnectAccount(); return; }
            if (!TraktClient::configured()) { tkStatus->setText(tr("Enter your Client ID and Secret first.")); return; }
            tkStatus->setText(tr("Requesting a code from Trakt…"));
            trakt_->connectAccount();
        });

        // --- Parental controls: a PIN that gates leaving a restricted (kids) profile. ---
        v->addSpacing(12);
        auto* pcHeading = new QLabel(tr("Parental Controls"));
        pcHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(pcHeading);
        auto* pcNote = new QLabel(tr("Set a PIN, then mark the kids' profiles below as restricted. While a "
                                     "restricted profile is active, switching profiles or opening Settings "
                                     "requires the PIN."));
        pcNote->setWordWrap(true); pcNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(pcNote);

        auto* pcStatus = new QLabel(Settings::hasParentalPin() ? tr("A PIN is set.") : tr("No PIN set."));
        pcStatus->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        auto* setPin = new QPushButton(Settings::hasParentalPin() ? tr("Change PIN") : tr("Set PIN"));
        auto* clrPin = new QPushButton(tr("Remove PIN"));
        setPin->setMinimumHeight(32); clrPin->setMinimumHeight(32);
        clrPin->setEnabled(Settings::hasParentalPin());
        auto* pcRow = new QHBoxLayout(); pcRow->addWidget(setPin); pcRow->addWidget(clrPin); pcRow->addStretch(1);
        v->addLayout(pcRow);
        v->addWidget(pcStatus);
        // PIN entry via the in-window on-screen keyboard (password echo): typeable from the couch, no popup.
        connect(setPin, &QPushButton::clicked, this, [this, setPin, clrPin, pcStatus] {
            if (Settings::hasParentalPin()) {
                const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(), QLineEdit::Password, this);
                if (cur.isNull()) return;
                if (!Settings::checkParentalPin(cur)) { pcStatus->setText(tr("Incorrect PIN.")); return; }
            }
            const QString a = Osk::getText(tr("New PIN:"), QString(), QLineEdit::Password, this);
            if (a.isNull() || a.isEmpty()) return;
            const QString b = Osk::getText(tr("Confirm PIN:"), QString(), QLineEdit::Password, this);
            if (b.isNull()) return;
            if (a != b) { pcStatus->setText(tr("PINs didn't match.")); return; }
            Settings::setParentalPin(a);
            pcStatus->setText(tr("A PIN is set.")); setPin->setText(tr("Change PIN")); clrPin->setEnabled(true);
        });
        connect(clrPin, &QPushButton::clicked, this, [this, setPin, clrPin, pcStatus] {
            const QString cur = Osk::getText(tr("Enter the current PIN:"), QString(), QLineEdit::Password, this);
            if (cur.isNull()) return;
            if (!Settings::checkParentalPin(cur)) { pcStatus->setText(tr("Incorrect PIN.")); return; }
            Settings::setParentalPin(QString());
            pcStatus->setText(tr("No PIN set.")); setPin->setText(tr("Set PIN")); clrPin->setEnabled(false);
        });

        v->addSpacing(6);
        auto* pcProfiles = new QLabel(tr("Restricted (kids) profiles:"));
        pcProfiles->setStyleSheet(QStringLiteral("font-size:13px;color:#bbb;"));
        v->addWidget(pcProfiles);
        for (const Profile& pr : ProfileStore::list()) {
            auto* cb = new QCheckBox((pr.icon.isEmpty() ? QString() : pr.icon + QStringLiteral("  ")) + pr.name);
            cb->setStyleSheet(QStringLiteral("font-size:15px;"));
            cb->setChecked(pr.restricted);
            const QString id = pr.id;
            connect(cb, &QCheckBox::toggled, this, [id](bool c) { ProfileStore::setRestricted(id, c); });
            v->addWidget(cb);
        }

        // --- Background music: play tracks dropped in <data>/music while browsing the menus. ---
        v->addSpacing(10);
        if (bgm_) bgm_->reload(); // rescan so files added since last time are picked up
        auto* bHeading = new QLabel(tr("Background Music"));
        bHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(bHeading);
        auto* bNote = new QLabel(tr("Drop audio files into the music folder to play them quietly while you browse "
                                    "(they pause during games and video)."));
        bNote->setWordWrap(true); bNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(bNote);

        auto* bOn = new QCheckBox(tr("Play background music"));
        bOn->setStyleSheet(QStringLiteral("font-size:15px;"));
        bOn->setChecked(Settings::bgmEnabled());
        v->addWidget(bOn);
        connect(bOn, &QCheckBox::toggled, this, [this](bool c) {
            Settings::setBgmEnabled(c); if (bgm_) bgm_->setEnabled(c); updateBackgroundMusic(); });

        auto* volRow = new QHBoxLayout();
        volRow->addWidget(new QLabel(tr("Volume")));
        auto* bVol = new QSlider(Qt::Horizontal); bVol->setRange(0, 100); bVol->setValue(Settings::bgmVolume());
        bVol->setEnabled(bOn->isChecked());
        connect(bOn, &QCheckBox::toggled, bVol, &QSlider::setEnabled);
        connect(bVol, &QSlider::valueChanged, this,
                [this](int val) { Settings::setBgmVolume(val); if (bgm_) bgm_->setVolume(val); });
        volRow->addWidget(bVol, 1);
        v->addLayout(volRow);

        auto* openMusic = panelRow(tr("Open Music Folder"));
        connect(openMusic, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(BackgroundMusic::musicDir()));
            if (bgm_) { bgm_->reload(); updateBackgroundMusic(); } // pick up any files already there
        });
        v->addWidget(openMusic);

        // --- Steam (achievements + owned library): a Steam Web API key shows an installed PC game's Steam
        // achievements; the key + a 64-bit SteamID also surface owned-but-not-installed games on the Steam console. ---
        v->addSpacing(10);
        auto* sHeading = new QLabel(tr("Steam (achievements + owned library)"));
        sHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(sHeading);
        auto* sNote = new QLabel(tr("Paste a Steam Web API key (steamcommunity.com/dev/apikey) to show an installed "
            "PC game's Steam achievements in the Triple theme, with the ones you've unlocked highlighted. Add your "
            "64-bit SteamID to also list owned-but-not-installed games on the Steam console (activating one installs "
            "it via Steam). Leave the SteamID blank to keep the console installed-only."));
        sNote->setWordWrap(true); sNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sNote);
        auto* sKey = new QLineEdit(Settings::steamWebApiKey());
        sKey->setMinimumHeight(34); sKey->setEchoMode(QLineEdit::Password);
        sKey->setPlaceholderText(tr("Steam Web API key"));
        v->addWidget(sKey);
        auto* sId = new QLineEdit(Settings::steamId());
        sId->setMinimumHeight(34);
        sId->setPlaceholderText(tr("64-bit SteamID (optional — for owned games)"));
        v->addWidget(sId);
        auto* sSave = panelRow(tr("Save Steam Key + SteamID"));
        connect(sSave, &QPushButton::clicked, this, [this, sKey, sId] {
            Settings::setSteamWebApiKey(sKey->text());
            Settings::setSteamId(sId->text());
            statusBar()->showMessage(tr("Saved Steam Web API key + SteamID."), 4000);
        });
        v->addWidget(sSave);

        // --- Streaming (Debrid): a TorBox API key turns Stremio torrent results into playable streams. ---
        v->addSpacing(10);
        auto* dHeading = new QLabel(tr("Streaming (Debrid)"));
        dHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(dHeading);
        v->addWidget(new QLabel(tr("TorBox API key")));
        auto* tbKey = new QLineEdit(store().value(QStringLiteral("debrid/torbox/apikey")).toString());
        tbKey->setMinimumHeight(34);
        tbKey->setPlaceholderText(tr("Paste your TorBox API key"));
        v->addWidget(tbKey);
        connect(tbKey, &QLineEdit::editingFinished, this, [tbKey] {
            store().setValue(QStringLiteral("debrid/torbox/apikey"), tbKey->text().trimmed());
            store().sync();
        });
        auto* dNote = new QLabel(tr("Lets Stremio torrent addons (Debridio, Torrentio…) play: cached torrents "
            "are resolved to a stream through your TorBox account. Find the key at torbox.app → Settings → API. "
            "Stored on this device."));
        dNote->setWordWrap(true);
        dNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(dNote);
    }, [this] { openSettingsHub(); });
}

// The two legs of the panel async-connection lifetime model (full statement at openCloudSync's connect block).
void MainWindow::clearPanelPageConns()
{
    for (const QMetaObject::Connection& c : panelPageConns_) disconnect(c);
    panelPageConns_.clear();
}

// True when the themed panel host is the CURRENT stack page AND `title` is its live top panel — the gate every
// panelPageConns_ REBUILD handler runs before re-presenting: a late async event (an OAuth completing minutes
// after the user navigated to Debug/home) must be dropped, never present a panel over an unrelated screen.
bool MainWindow::themedPanelIsTop(const QString& title) const
{
#ifdef MMV_HAVE_QML
    // ALSO require no overlay (OSK / NavMenu) above the top panel: an overlay doesn't change panelTitle(), so
    // without this a late async handler could rebuild the panel UNDER a live edit — force-cancelling the user's
    // OSK text, or (presentAddByUrl's deferred handleBack) popping the OSK's mirrored level and stacking a
    // duplicate level. While an overlay is up the rebuild is DROPPED; the state persists and renders on the next
    // top-gated event or re-present (the user keeps their edit).
    return themedPanelHost_ && stack_->currentWidget() == themedPanelHost_
           && themedPanelHost_->panelTitle() == title
           && !themedPanelHost_->overlayAbove();
#else
    Q_UNUSED(title);
    return false;
#endif
}

void MainWindow::openCloudSync()
{
    if (!cloud_) cloud_ = std::make_unique<CloudSync>(this);
#ifdef MMV_HAVE_QML
    // Themed mode: the four sign-in Actions become state-gated PanelRows (omitted, not just hidden, per the SAME
    // isConfigured()/isSignedIn() checks the classic refresh() uses), over a status Info row. The row SET flips
    // with sign-in state, so the async signals rebuild IN PLACE via replaceTop (reentry) — no stacked level. Same
    // flows: signIn()/signOut()/cloudSyncNow()/openCloudClientSetup().
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // this present replaces the pool (re-armed below)
        themedPanelHost_->setStyle(settingsPanelStyle());

        const bool cfg = CloudSync::isConfigured();
        const bool in  = cloud_->isSignedIn();
        QString status;
        if (!cfg)    status = tr("Google sign-in isn't set up yet — choose \"Set up sign-in…\" to paste a Desktop-app client.");
        else if (in) status = tr("Signed in as %1.").arg(cloud_->accountEmail());
        else         status = tr("Not signed in — choose \"Sign in with Google\".");

        QVector<PanelRow> rows;
        auto info   = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        info(QStringLiteral("cloud.status"), tr("Status"), status);
        if (cfg && !in) action(QStringLiteral("cloud.signin"), tr("Sign in with Google"));
        if (in)         action(QStringLiteral("cloud.syncnow"), tr("Sync now"));
        if (in)         action(QStringLiteral("cloud.signout"), tr("Sign out"));
        action(QStringLiteral("cloud.setup"), cfg ? tr("Change sign-in client…") : tr("Set up sign-in…"));

        auto setStatus = [this](const QString& s) {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cloud.status"); r.label = MainWindow::tr("Status");
            r.value = s; themedPanelHost_->updateRow(QStringLiteral("cloud.status"), r); };

        auto onAct = [this, setStatus](const QString& id, const QString&) {
            if      (id == QStringLiteral("cloud.signin"))  { setStatus(tr("Opening your browser…")); cloud_->signIn(); }
            else if (id == QStringLiteral("cloud.syncnow")) { setStatus(tr("Syncing…")); cloudSyncNow(); }
            else if (id == QStringLiteral("cloud.signout")) cloud_->signOut();
            else if (id == QStringLiteral("cloud.setup"))   openCloudClientSetup();
        };
        auto onBack = [this] { openSettingsHub(); };   // Cloud is a hub child — Back pops to the hub

        // Re-entry (an async sign-in state change while Cloud is the top panel) rebuilds the row SET in place;
        // the first entry from the hub is a nested present().
        if (themedPanelHost_->panelTitle() == tr("Cloud Sync"))
            themedPanelHost_->replaceTop(tr("Cloud Sync"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("Cloud Sync"), rows, onAct, onBack);

        // CONNECTION-LIFETIME MODEL (panelPageConns_, shared by Cloud + RetroAchievements — THE statement):
        //  * Armed when their panel PRESENTS (here). NOT cleared by nested children: a child's Back pop restores
        //    this panel via renderTop WITHOUT re-running openCloudSync, so these listeners must survive the
        //    drill — clearing them in openCloudClientSetup left "Sign in with Google" silently DEAD after
        //    backing out of the setup form (the regression the review found).
        //  * Replaced wholesale whenever ANY pool user re-presents (the clearPanelPageConns at the top).
        //  * Cleared at the settings-area boundaries: openSettingsHub entry + the hub root's leave-to-home.
        //  * REBUILD handlers self-gate on themedPanelIsTop and DROP otherwise — an OAuth completing after the
        //    user navigated away must not present a Cloud panel over Debug/home (CloudSync persists the sign-in
        //    state, so the next entry renders it; the gate also makes the Save-time signOut in
        //    openCloudClientSetup safe, since "Sign-in client" is top at that moment).
        //  * ROW-PATCH handlers (setStatus) need no gate: updateRow patches by row id and no-ops when no stacked
        //    panel carries the row — a backgrounded Cloud (under the setup child) still receives the status.
        panelPageConns_ << connect(cloud_.get(), &CloudSync::signedIn, this, [this](const QString&) {
            if (!themedPanelIsTop(tr("Cloud Sync"))) return;   // navigated away — drop (state persists)
            openCloudSync(); raise(); activateWindow(); cloudSyncNow(); });   // rebuild (now signed-in) + first push
        panelPageConns_ << connect(cloud_.get(), &CloudSync::signInFailed, this, [setStatus](const QString& e) {
            setStatus(MainWindow::tr("Sign-in failed: %1").arg(e)); });
        panelPageConns_ << connect(cloud_.get(), &CloudSync::signedOut, this, [this] {
            if (!themedPanelIsTop(tr("Cloud Sync"))) return;   // e.g. Save-time signOut in the setup child — drop
            openCloudSync(); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
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

// ---- BIOS check (RetroBat-style): per-system BIOS presence + download-missing ---------------------------

namespace
{
// Where system <id>'s BIOS should live: a standalone emulator keeps it under its own tree; in-process cores
// read from the shared libretro "system" folder.
QString biosDestDir(const QString& systemId)
{
    const GameSystem* s = SystemCatalog::byId(systemId);
    if (s && !s->externalEmulator.isEmpty())
        return EmulatorManager::emulatorsRoot() + QStringLiteral("/") + s->externalEmulator + QStringLiteral("/bios");
    return CoreManager::systemDir();
}

// Path of the BIOS file if it's present — in the libretro system folder, or anywhere under its emulator's
// tree (PCSX2 stores it in a "bios" sub-folder whose exact location depends on the build). Empty if missing.
QString biosFilePath(const QString& systemId, const QString& fileName)
{
    const QString inSys = CoreManager::systemDir() + QStringLiteral("/") + fileName;
    if (QFile::exists(inSys)) return inSys;
    const GameSystem* s = SystemCatalog::byId(systemId);
    if (s && !s->externalEmulator.isEmpty())
    {
        const QString emuRoot = EmulatorManager::emulatorsRoot() + QStringLiteral("/") + s->externalEmulator;
        if (QDir(emuRoot).exists())
        {
            QDirIterator it(emuRoot, QStringList{ fileName }, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext()) return it.next();
        }
    }
    return QString();
}

// MD5 of a file, lowercase hex (streamed, so a multi-MB BIOS isn't slurped into memory). Empty on read error.
QString fileMd5(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Md5);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}
} // namespace

void MainWindow::openBiosCheck()
{
#ifdef MMV_HAVE_QML
    // Themed mode: the RichText per-file MD5 report degrades to one Info row per system (name + a tick/cross/warn
    // glyph summarising that system's files), under a summary Info row. Same MD5 verification (biosFilePath +
    // fileMd5), same Download/Repair (drop wrong-hash files, then ensureBios) and Open Folder flows. Download
    // rebuilds the panel in place (replaceTop) so the ticks refresh without stacking a level.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        themedPanelHost_->setStyle(settingsPanelStyle());

        int total = 0, good = 0, bad = 0, missing = 0;
        QVector<PanelRow> sysRows;
        for (const BiosCatalog::BiosSystem& bs : BiosCatalog::systemsWithBios())
        {
            const QList<BiosFile>& files = BiosCatalog::forSystem(bs.systemId);
            if (files.isEmpty()) continue;
            int sgood = 0, sbad = 0, smiss = 0;
            for (const BiosFile& bf : files)
            {
                ++total;
                const QString path = biosFilePath(bs.systemId, bf.fileName);
                if (path.isEmpty()) { ++missing; ++smiss; }
                else if (!bf.md5.isEmpty() && fileMd5(path).compare(bf.md5, Qt::CaseInsensitive) != 0) { ++bad; ++sbad; }
                else { ++good; ++sgood; }
            }
            QString value;
            if (smiss == 0 && sbad == 0)
                value = tr("✓  %1/%2 present").arg(sgood).arg(files.size());
            else if (sbad > 0)
                value = smiss > 0 ? tr("⚠  %1 wrong MD5, %2 missing").arg(sbad).arg(smiss)
                                  : tr("⚠  %1 wrong MD5").arg(sbad);
            else
                value = tr("✗  %1 of %2 missing").arg(smiss).arg(files.size());
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("bios.sys:") + bs.systemId;
            r.label = bs.name; r.value = value; sysRows << r;
        }

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("bios.summary"); r.label = tr("BIOS files");
          r.value = bad > 0 ? tr("%1/%2 OK, %n failed", "", bad).arg(good).arg(total)
                            : tr("%1 of %2 present").arg(good).arg(total); rows << r; }
        { PanelRow r; r.kind = PanelRow::Separator; r.label = tr("By system"); rows << r; }
        rows += sysRows;

        const bool needsDownload = (missing > 0 || bad > 0);
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("bios.download");
          r.label = needsDownload ? tr("Download / Repair BIOS") : tr("Re-check"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("bios.open"); r.label = tr("Open BIOS Folder"); rows << r; }

        auto onAct = [this](const QString& id, const QString&) {
            if (id == QStringLiteral("bios.download")) {
                statusBar()->showMessage(tr("Checking BIOS…"));
                for (const BiosCatalog::BiosSystem& bs : BiosCatalog::systemsWithBios())
                {
                    for (const BiosFile& bf : BiosCatalog::forSystem(bs.systemId))
                    {
                        if (bf.md5.isEmpty()) continue;
                        const QString p = biosFilePath(bs.systemId, bf.fileName);
                        if (!p.isEmpty() && fileMd5(p).compare(bf.md5, Qt::CaseInsensitive) != 0) QFile::remove(p);
                    }
                    CoreManager::ensureBios(bs.systemId, biosDestDir(bs.systemId),
                                            [this](const QString& s) { statusBar()->showMessage(s); });
                }
                statusBar()->showMessage(tr("BIOS check complete."), 4000);
                openBiosCheck();   // rebuild (replaceTop, we're the top panel) so the ticks refresh
            }
            else if (id == QStringLiteral("bios.open"))
                QDesktopServices::openUrl(QUrl::fromLocalFile(CoreManager::systemDir()));
        };
        auto onBack = [this] { openSettingsHub(); };

        if (themedPanelHost_->panelTitle() == tr("BIOS Check"))
            themedPanelHost_->replaceTop(tr("BIOS Check"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("BIOS Check"), rows, onAct, onBack);

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("BIOS Check"), [this](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("Required BIOS / firmware for each system, verified by MD5. Missing files "
            "are fetched automatically the first time you launch a game for that system — or download them all "
            "now. BIOS dumps are copyrighted, so they aren't shipped with the app."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:13px;"));
        v->addWidget(intro);

        int total = 0, good = 0, bad = 0, missing = 0;
        QString html;
        for (const BiosCatalog::BiosSystem& bs : BiosCatalog::systemsWithBios())
        {
            const QList<BiosFile>& files = BiosCatalog::forSystem(bs.systemId);
            if (files.isEmpty()) continue;
            html += QStringLiteral("<p style='margin:10px 0 2px 0;'><b>%1</b></p>").arg(bs.name.toHtmlEscaped());
            for (const BiosFile& bf : files)
            {
                ++total;
                const QString path = biosFilePath(bs.systemId, bf.fileName);
                QString colour, mark, note;
                if (path.isEmpty())
                { colour = QStringLiteral("#e03131"); mark = QStringLiteral("&#10007;"); note = tr(" — missing"); ++missing; }
                else if (!bf.md5.isEmpty() && fileMd5(path).compare(bf.md5, Qt::CaseInsensitive) != 0)
                { colour = QStringLiteral("#f08c00"); mark = QStringLiteral("&#9888;"); note = tr(" — wrong MD5 (corrupt or a different dump)"); ++bad; }
                else if (bf.md5.isEmpty())
                { colour = QStringLiteral("#37b24d"); mark = QStringLiteral("&#10003;"); note = tr(" — present (MD5 not verified)"); ++good; }
                else
                { colour = QStringLiteral("#37b24d"); mark = QStringLiteral("&#10003;"); ++good; }

                html += QStringLiteral("<div style='margin-left:14px;color:%1;'>%2&nbsp;&nbsp;%3</div>")
                            .arg(colour, mark, bf.fileName.toHtmlEscaped() + note);
            }
        }

        auto* summary = new QLabel(bad > 0
            ? tr("%1 of %2 BIOS files OK — %n failed the MD5 check.", "", bad).arg(good).arg(total)
            : tr("%1 of %2 BIOS files present.").arg(good).arg(total));
        summary->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;margin-top:6px;"));
        v->addWidget(summary);

        auto* report = new QLabel(this);
        report->setTextFormat(Qt::RichText); report->setWordWrap(true);
        report->setText(html);
        v->addWidget(report);

        // Offer a download when anything is missing OR a present file failed its hash (re-fetch overwrites it).
        const bool needsDownload = (missing > 0 || bad > 0);
        auto* dl = panelRow(needsDownload ? tr("Download / Repair BIOS") : tr("Re-check"));
        connect(dl, &QPushButton::clicked, this, [this] {
            statusBar()->showMessage(tr("Checking BIOS…"));
            for (const BiosCatalog::BiosSystem& bs : BiosCatalog::systemsWithBios())
            {
                // Drop any present-but-wrong-hash file so ensureBios (which skips existing files) re-fetches it.
                for (const BiosFile& bf : BiosCatalog::forSystem(bs.systemId))
                {
                    if (bf.md5.isEmpty()) continue;
                    const QString p = biosFilePath(bs.systemId, bf.fileName);
                    if (!p.isEmpty() && fileMd5(p).compare(bf.md5, Qt::CaseInsensitive) != 0) QFile::remove(p);
                }
                CoreManager::ensureBios(bs.systemId, biosDestDir(bs.systemId),
                                        [this](const QString& s) { statusBar()->showMessage(s); });
            }
            statusBar()->showMessage(tr("BIOS check complete."), 4000);
            openBiosCheck(); // rebuild the panel so the ticks refresh
        });
        v->addWidget(dl);

        auto* open = panelRow(tr("Open BIOS Folder"));
        connect(open, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(CoreManager::systemDir()));
        });
        v->addWidget(open);
    }, [this] { openSettingsHub(); });
}

// Inline form (no popup) to paste the Google OAuth client id/secret used for Drive sign-in.
void MainWindow::openCloudClientSetup()
{
#ifdef MMV_HAVE_QML
    // Themed mode: two TextField rows (client id/secret via the OSK) + Save. Same write path (the cloud/clientId,
    // cloud/clientSecret ini keys) and same follow-through (signOut + back to a refreshed Cloud panel). Values are
    // held pending and only committed on Save — exactly like the classic form (Back discards). This is a nested
    // present() on the Cloud panel; Save pops back to Cloud and rebuilds it (now configured) via replaceTop.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        // Do NOT clear panelPageConns_ here: this is a nested child of Cloud, and Back restores the CACHED Cloud
        // parent (renderTop) without re-running openCloudSync — the parent's sign-in listeners must stay armed or
        // "Sign in with Google" goes silently dead after backing out (the lifetime model at openCloudSync's
        // connect block). The Save-time signOut below is safe: the signedOut rebuild handler self-gates on Cloud
        // being top, and "Sign-in client" is top at that moment.
        themedPanelHost_->setStyle(settingsPanelStyle());

        const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");
        QSettings s(iniPath, QSettings::IniFormat);
        auto pending = std::make_shared<QPair<QString, QString>>(
            s.value(QStringLiteral("cloud/clientId")).toString(),
            s.value(QStringLiteral("cloud/clientSecret")).toString());

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.note"); r.label = tr("Client");
          r.value = tr("Google Desktop-app OAuth"); rows << r; }
        { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("cc.id"); r.label = tr("Client id");
          r.value = pending->first; rows << r; }
        { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("cc.secret"); r.label = tr("Client secret");
          r.value = pending->second; rows << r; }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.err"); r.label = tr("Status"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.save"); r.label = tr("Save"); rows << r; }

        themedPanelHost_->present(tr("Sign-in client"), rows,
            [this, pending, iniPath](const QString& id, const QString& val) {
                if      (id == QStringLiteral("cc.id"))     pending->first = val;
                else if (id == QStringLiteral("cc.secret")) pending->second = val;
                else if (id == QStringLiteral("cc.save")) {
                    if (pending->first.trimmed().isEmpty()) {
                        PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.err"); r.label = tr("Status");
                        r.value = tr("Enter a client id."); themedPanelHost_->updateRow(QStringLiteral("cc.err"), r);
                        return;
                    }
                    QSettings s(iniPath, QSettings::IniFormat);
                    s.setValue(QStringLiteral("cloud/clientId"), pending->first.trimmed());
                    s.setValue(QStringLiteral("cloud/clientSecret"), pending->second.trimmed());
                    s.sync();
                    cloud_->signOut();   // a new client invalidates the old sign-in (the signedOut rebuild
                                         // handler self-gates on Cloud being top — Setup is top here, so it drops)
                    // Pop back to Cloud and rebuild it (now configured) — deferred so this activate() unwinds first.
                    QTimer::singleShot(0, this, [this] { themedPanelHost_->handleBack(); openCloudSync(); });
                }
            },
            [this] { openCloudSync(); });   // defensive root onBack (nested: a pop just renders the Cloud parent)

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("Sign-in client"), [this](QVBoxLayout* v) {
        QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
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
            QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
            s.setValue(QStringLiteral("cloud/clientId"), id);
            s.setValue(QStringLiteral("cloud/clientSecret"), secEdit->text().trimmed());
            s.sync();
            cloud_->signOut();    // a new client invalidates the old sign-in
            openCloudSync();      // back to the sync panel (now configured)
        });
        v->addWidget(save);
    }, [this] { openCloudSync(); });
}

// Manual "Sync now": save the current state up to Drive immediately (same as the automatic exit push).
void MainWindow::cloudSyncNow()
{
    if (!cloud_ || !cloud_->isSignedIn()) return;
    statusBar()->showMessage(tr("Saving to Google Drive…"));
    cloud_->pushLocal([this](bool ok, const QString& m) {
        statusBar()->showMessage(ok ? tr("Saved to Google Drive.") : m, ok ? kFeedbackShort : kFeedbackLong); // success -> Short, error -> Long (J22)
    });
}

// ---- "Continue watching" cross-device sync ---------------------------------------------------------------
// Resume positions ("resume/<hash>/{pos,dur,ts,title}") and the per-profile recent lists ("recent/<id>/items")
// both already live in the ini; this syncs JUST those, in a small file pushed far more often than the heavy
// state bundle, and MERGES on pull (by recency) so two devices' progress combine instead of clobbering.

// The serialize/merge logic MOVED to core/CloudMerge (mdsync T2): a pure ini-in/json-out module that now
// covers EVERY per-item store (resume, recents, marks, favourites, playlists) — merged by recency with
// deletion tombstones — not just resume+recents. These two are thin wrappers so the existing push/pull
// plumbing (pushProgressNow / pullAndMergeProgress, the 15s debounce, the startup pull) is unchanged.
QByteArray MainWindow::serializeProgress() const
{
    QJsonObject root;
    CloudMerge::serializeAll(root);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void MainWindow::mergeProgress(const QByteArray& json)
{
    CloudMerge::mergeAll(QJsonDocument::fromJson(json).object());
}

void MainWindow::pushProgressNow()
{
    if (!cloud_ || !cloud_->isSignedIn()) return;
    if (progressSyncTimer_) progressSyncTimer_->stop(); // this covers any pending debounce
    cloud_->pushProgress(serializeProgress(), [](bool) {}); // best-effort
}

void MainWindow::scheduleProgressSync()
{
    if (!cloud_ || !cloud_->isSignedIn()) return;
    if (!progressSyncTimer_)
    {
        progressSyncTimer_ = new QTimer(this);
        progressSyncTimer_->setSingleShot(true);
        connect(progressSyncTimer_, &QTimer::timeout, this, [this] { pushProgressNow(); });
    }
    progressSyncTimer_->start(15000); // debounce: push ~15s after the last change (i.e. shortly after you stop)
}

void MainWindow::pullAndMergeProgress()
{
    if (!cloud_ || !cloud_->isSignedIn()) return;
    cloud_->pullProgress([this](bool ok, const QByteArray& json) {
        if (!ok || json.isEmpty()) return;
        mergeProgress(json);
        // If the home is on screen, rebuild it so freshly-merged resume progress + recent entries show at once.
        QWidget* cur = stack_->currentWidget();
        if ((cur == home_ || cur == themedHome_) && home_) { home_->refresh(); showHomeScreen(); }
    });
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    session_->persistResume(); // flush the current media's playback position before anything else on exit
    pushProgressNow(); // and push the small "continue watching" file (runs alongside the bundle push below)
    retro_->stop();  // flush battery-RAM saves before the cloud push captures them (no-op if no game running)

    // Already pushed (or nothing to do)? let the close through.
    if (forceClose_ || !cloud_ || !cloud_->isSignedIn()) { QMainWindow::closeEvent(e); return; }

    // Always save the current state to Drive on exit. Defer the quit until the push finishes; a watchdog
    // force-closes after a few seconds so a slow/absent network can never trap the app open.
    e->ignore();
    auto finishClose = [this] { forceClose_ = true; close(); };
    QTimer::singleShot(8000, this, [this, finishClose] { if (!forceClose_) finishClose(); });
    statusBar()->showMessage(tr("Saving to Google Drive…"));
    cloud_->pushLocal([finishClose](bool, const QString&) { finishClose(); });
}

void MainWindow::openRetroAchievements()
{
#ifdef MMV_HAVE_QML
    // Themed mode: the login form (username/password TextFields) OR the signed-in state (web API key TextField +
    // Sign Out) per the SAME ach_->isLoggedIn() branch. Password + API key rows are MASKED — dots both in the
    // value display AND while editing (the host opens the OSK in QLineEdit::Password echo for a masked row; see
    // ThemedPanelHost::onGraphActivated). The row SET flips with login state, so the async loginResult / a logout
    // rebuild IN PLACE via replaceTop (reentry). Same flow: loginWithPassword() / the ra/apikey ini key.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        clearPanelPageConns();   // this present replaces the pool (lifetime model at openCloudSync's connect block)
        themedPanelHost_->setStyle(settingsPanelStyle());

        const QString iniPath = AppPaths::dataDir() + QStringLiteral("/mymediavault.ini");
        const bool in = ach_ && ach_->isLoggedIn();

        QVector<PanelRow> rows;
        auto info = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        auto textf = [&rows](const QString& id, const QString& label, const QString& value, bool masked) {
            PanelRow r; r.kind = PanelRow::TextField; r.id = id; r.label = label; r.value = value; r.masked = masked; rows << r; };

        // Held-pending edits (username/password when signed out; the api key when signed in).
        auto pend = std::make_shared<QHash<QString, QString>>();

        if (in)
        {
            QSettings raStore(iniPath, QSettings::IniFormat);
            (*pend)[QStringLiteral("apikey")] = raStore.value(QStringLiteral("ra/apikey")).toString();
            info(QStringLiteral("ra.status"), tr("Account"), tr("Signed in as %1.").arg(ach_->username()));
            textf(QStringLiteral("ra.apikey"), tr("Web API key"), (*pend)[QStringLiteral("apikey")], /*masked=*/true);
            action(QStringLiteral("ra.savekey"), tr("Save API Key"));
            action(QStringLiteral("ra.signout"), tr("Sign Out"));
        }
        else
        {
            info(QStringLiteral("ra.status"), tr("Status"), tr("Sign in to earn achievements while you play."));
            textf(QStringLiteral("ra.user"), tr("Username"), QString(), /*masked=*/false);
            textf(QStringLiteral("ra.pass"), tr("Password"), QString(), /*masked=*/true);
            action(QStringLiteral("ra.signin"), tr("Sign In"));
        }

        auto setStatus = [this](const QString& label, const QString& s) {
            PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ra.status"); r.label = label; r.value = s;
            themedPanelHost_->updateRow(QStringLiteral("ra.status"), r); };

        auto onAct = [this, pend, iniPath, setStatus](const QString& id, const QString& val) {
            if      (id == QStringLiteral("ra.user"))   (*pend)[QStringLiteral("user")] = val;
            else if (id == QStringLiteral("ra.pass"))   (*pend)[QStringLiteral("pass")] = val;
            else if (id == QStringLiteral("ra.apikey")) (*pend)[QStringLiteral("apikey")] = val;
            else if (id == QStringLiteral("ra.savekey")) {
                QSettings s(iniPath, QSettings::IniFormat);
                s.setValue(QStringLiteral("ra/apikey"), (*pend)[QStringLiteral("apikey")].trimmed()); s.sync();
                statusBar()->showMessage(tr("Saved RetroAchievements web API key."), 4000);
            }
            else if (id == QStringLiteral("ra.signout")) { if (ach_) ach_->logout(); openRetroAchievements(); }
            else if (id == QStringLiteral("ra.signin")) {
                const QString u = (*pend)[QStringLiteral("user")].trimmed(), p = (*pend)[QStringLiteral("pass")];
                if (!ach_ || u.isEmpty() || p.isEmpty()) { setStatus(tr("Status"), tr("Enter your username and password.")); return; }
                setStatus(tr("Status"), tr("Signing in…"));
                ach_->loginWithPassword(u, p);
            }
        };
        auto onBack = [this] { openSettingsHub(); };   // RA is a hub child

        if (themedPanelHost_->panelTitle() == tr("RetroAchievements"))
            themedPanelHost_->replaceTop(tr("RetroAchievements"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("RetroAchievements"), rows, onAct, onBack);

        // Async login result: success rebuilds into the signed-in state — SELF-GATED on RA being the live top
        // (the lifetime model): a login completing after the user navigated away is dropped (the token is stored
        // by Achievements, so the next entry renders signed-in). Failure patches the status row (updateRow is
        // inherently safe — it no-ops when no stacked panel carries ra.status).
        if (ach_)
            panelPageConns_ << connect(ach_, &Achievements::loginResult, this, [this, setStatus](bool ok, const QString& msg) {
                if (ok) { if (themedPanelIsTop(tr("RetroAchievements"))) openRetroAchievements(); }
                else    setStatus(MainWindow::tr("Status"), MainWindow::tr("Sign-in failed: %1").arg(msg));
            });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("RetroAchievements"), [this](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("Sign in with your <b>RetroAchievements</b> account to earn achievements while "
            "playing. Your password is exchanged for a token (stored locally) and not kept. Softcore for now — "
            "save states stay enabled."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);

        auto* status = new QLabel(this);
        status->setStyleSheet(QStringLiteral("font-size:14px;font-weight:bold;"));
        v->addWidget(status);

        if (ach_ && ach_->isLoggedIn())
        {
            status->setText(tr("Signed in as %1.").arg(ach_->username()));

            // Optional web API key: lets the Triple/XMB info panel show a game's achievements (with the ones
            // you've earned highlighted) while browsing. It's separate from your login token.
            auto* keyIntro = new QLabel(tr("Optional: paste your RetroAchievements <b>web API key</b> (RA site → "
                "Settings → Keys) to show each game's achievements — the ones you've earned highlighted — in the "
                "Triple theme's info panel."));
            keyIntro->setWordWrap(true); keyIntro->setStyleSheet(QStringLiteral("font-size:13px;"));
            v->addWidget(keyIntro);
            QSettings raStore(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
            auto* keyEdit = new QLineEdit(); keyEdit->setMinimumHeight(34);
            keyEdit->setEchoMode(QLineEdit::Password); keyEdit->setPlaceholderText(tr("Web API key"));
            keyEdit->setText(raStore.value(QStringLiteral("ra/apikey")).toString());
            v->addWidget(keyEdit);
            auto* saveKey = panelRow(tr("Save API Key"));
            connect(saveKey, &QPushButton::clicked, this, [this, keyEdit] {
                QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
                s.setValue(QStringLiteral("ra/apikey"), keyEdit->text().trimmed()); s.sync();
                statusBar()->showMessage(tr("Saved RetroAchievements web API key."), 4000);
            });
            v->addWidget(saveKey);

            auto* out = panelRow(tr("Sign Out"));
            connect(out, &QPushButton::clicked, this, [this] { if (ach_) ach_->logout(); openRetroAchievements(); });
            v->addWidget(out);
            return;
        }

        auto* userEdit = new QLineEdit(); userEdit->setMinimumHeight(34); userEdit->setPlaceholderText(tr("Username"));
        v->addWidget(new QLabel(tr("Username")));
        v->addWidget(userEdit);
        auto* passEdit = new QLineEdit(); passEdit->setMinimumHeight(34);
        passEdit->setEchoMode(QLineEdit::Password); passEdit->setPlaceholderText(tr("Password"));
        v->addWidget(new QLabel(tr("Password")));
        v->addWidget(passEdit);

        auto* signIn = panelRow(tr("Sign In"));
        auto doLogin = [this, userEdit, passEdit, status] {
            if (!ach_ || userEdit->text().trimmed().isEmpty() || passEdit->text().isEmpty())
            { status->setText(tr("Enter your username and password.")); return; }
            status->setText(tr("Signing in…"));
            ach_->loginWithPassword(userEdit->text().trimmed(), passEdit->text());
        };
        connect(signIn, &QPushButton::clicked, this, doLogin);
        connect(passEdit, &QLineEdit::returnPressed, this, doLogin);
        v->addWidget(signIn);

        // Reflect the async result; on success rebuild the panel to show the signed-in state.
        connect(ach_, &Achievements::loginResult, status, [this, status](bool ok, const QString& msg) {
            if (ok) openRetroAchievements();
            else    status->setText(tr("Sign-in failed: %1").arg(msg));
        });
    }, [this] { openSettingsHub(); });
}

void MainWindow::openDebug()
{
#ifdef MMV_HAVE_QML
    // Themed mode: the diagnostic log becomes a scrollable read-only LogView row (activate = scroll mode: Up/Down
    // scroll the tail, Esc returns to row selection — NavTextField's read-only two-state semantics at the panel
    // level) alongside Refresh / Clear / Open-location Actions and the UI-test Toggle. Same flows: the same log
    // path + 500-line tail, the same Settings::setUiTestChannel + updateUiTestServer.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        themedPanelHost_->setStyle(settingsPanelStyle());
        const QString path = AppPaths::dataDir() + QStringLiteral("/stream_debug.log");

        auto loadTail = [path]() -> QString {
            QFile f(path);
            QString text = f.open(QIODevice::ReadOnly | QIODevice::Text)
                               ? QString::fromUtf8(f.readAll()) : MainWindow::tr("(no log entries yet)");
            const QStringList lines = text.split(QLatin1Char('\n'));
            constexpr int keep = 500;
            return lines.size() > keep ? lines.mid(lines.size() - keep).join(QLatin1Char('\n')) : text;
        };

        QVector<PanelRow> rows;
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("debug.path"); r.label = tr("Log file");
          r.value = path; rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("debug.refresh"); r.label = tr("Refresh"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("debug.clear"); r.label = tr("Clear log"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("debug.openloc"); r.label = tr("Open file location"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("debug.uitest");
          r.label = tr("UI test channel (local automation pipe — no focus needed)");
          r.checked = Settings::uiTestChannel(); rows << r; }
        { PanelRow r; r.kind = PanelRow::LogView; r.id = QStringLiteral("debug.log"); r.label = tr("Log");
          r.value = loadTail(); rows << r; }

        auto setLog = [this](const QString& t) {
            PanelRow r; r.kind = PanelRow::LogView; r.id = QStringLiteral("debug.log"); r.label = MainWindow::tr("Log");
            r.value = t; themedPanelHost_->updateRow(QStringLiteral("debug.log"), r); };

        themedPanelHost_->present(tr("Debug"), rows,
            [this, path, loadTail, setLog](const QString& id, const QString& val) {
                if      (id == QStringLiteral("debug.refresh")) setLog(loadTail());
                else if (id == QStringLiteral("debug.clear"))   { QFile::remove(path); setLog(loadTail()); }
                else if (id == QStringLiteral("debug.openloc"))
                    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                else if (id == QStringLiteral("debug.uitest")) {
                    Settings::setUiTestChannel(val == QStringLiteral("1"));
                    updateUiTestServer();   // start/stop the pipe right away
                }
            },
            [this] { openSettingsHub(); });

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("Debug"), [this](QVBoxLayout* v) {
        const QString path = AppPaths::dataDir() + QStringLiteral("/stream_debug.log");

        auto* intro = new QLabel(tr("Diagnostic log. Errors and stream-resolution traces are recorded here "
                                    "(no API keys or tokens are written). The log is kept in the app folder "
                                    "and trimmed automatically when it gets large."));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);

        auto* pathLbl = new QLabel(path);
        pathLbl->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        pathLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(pathLbl);

        auto* view = new QPlainTextEdit();
        view->setReadOnly(true);
        view->setLineWrapMode(QPlainTextEdit::NoWrap);
        view->setStyleSheet(QStringLiteral("font-family:Consolas,'Courier New',monospace;font-size:12px;"));
        v->addWidget(view, 1);

        // Show the tail (newest entries) so a long log stays responsive; scroll to the bottom.
        auto loadLog = [view, path] {
            QFile f(path);
            QString text = f.open(QIODevice::ReadOnly | QIODevice::Text)
                               ? QString::fromUtf8(f.readAll()) : tr("(no log entries yet)");
            const QStringList lines = text.split(QLatin1Char('\n'));
            constexpr int keep = 500;
            view->setPlainText(lines.size() > keep ? lines.mid(lines.size() - keep).join(QLatin1Char('\n')) : text);
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        };
        loadLog();

        auto* row = new QHBoxLayout();
        auto* refresh = new QPushButton(tr("Refresh"));
        auto* clear   = new QPushButton(tr("Clear log"));
        auto* openLoc = new QPushButton(tr("Open file location"));
        row->addWidget(refresh); row->addWidget(clear); row->addWidget(openLoc); row->addStretch(1);
        v->addLayout(row);

        connect(refresh, &QPushButton::clicked, this, [loadLog] { loadLog(); });
        connect(clear,   &QPushButton::clicked, this, [path, loadLog] { QFile::remove(path); loadLog(); });
        connect(openLoc, &QPushButton::clicked, this, [path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });

        // UI-test/automation channel: a local pipe test tools drive the app through (navigate, inspect,
        // screenshot) without it needing focus. Applies immediately and persists across launches.
        auto* uitest = new QCheckBox(tr("UI test channel (local automation pipe for testing — no focus needed)"));
        uitest->setChecked(Settings::uiTestChannel());
        connect(uitest, &QCheckBox::toggled, this, [this](bool on) {
            Settings::setUiTestChannel(on);
            updateUiTestServer(); // start/stop the pipe right away
        });
        v->addSpacing(8);
        v->addWidget(uitest);
    }, [this] { openSettingsHub(); });
}

void MainWindow::openEmulatorSettings()
{
    // The dialog loads cores headlessly to read their options; only one libretro core can be live at a
    // time (the C ABI routes through a global), so stop any running game first.
    retro_->stop();
#ifdef MMV_HAVE_QML
    // Themed mode: the per-system core picker on the Nav Contract (ThemedPanelHost) instead of the classic
    // SettingsDialog. Same combo data (SystemCatalog + Settings::coreFor); the per-core options editor page
    // becomes a nested panel level. A hub child -> nested present(), Back -> openSettingsHub.
    if (themedHomeEnabled() && themedPanelHost_) { presentEmulatorCorePicker(); return; }
#endif
    auto* dlg = new SettingsDialog(this);
    showDialogPanel(tr("Emulator Settings"), dlg, [this](int) { openSettingsHub(); },
                    [this] { openSettingsHub(); });
}

#ifdef MMV_HAVE_QML
// Themed core picker: one Choice row per system (cycles the libretro core, applied+persisted immediately via
// Settings::setCoreFor — the themed-panel convention, no separate Save) + an indented "Options…" Action per
// system that drills into that core's options as a nested panel level. retro_->stop() (openEmulatorSettings)
// keeps sole use of the single libretro slot before any headless core load in editCoreOptions.
void MainWindow::presentEmulatorCorePicker()
{
    themedPanelHost_->setStyle(settingsPanelStyle());

    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("intro");
      r.label = tr("Libretro core per system"); r.value = tr("Auto-used on launch"); rows << r; }
    for (const GameSystem& sys : SystemCatalog::systems())
    {
        if (!sys.externalEmulator.isEmpty()) continue;   // standalone emulators have no libretro core to pick
        QString chosen = Settings::coreFor(sys.id);
        if (chosen.isEmpty()) chosen = sys.cores.value(0);
        { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("core:") + sys.id; r.label = sys.name;
          r.value = chosen; r.options = sys.cores; rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("opts:") + sys.id;
          r.label = QStringLiteral("      ") + tr("Options…"); rows << r; } // indented: belongs to the row above
    }

    auto onAct = [this](const QString& id, const QString& val) {
        if (id.startsWith(QStringLiteral("core:")))
            Settings::setCoreFor(id.mid(5), val);            // immediate apply (persists to ini)
        else if (id.startsWith(QStringLiteral("opts:")))
            editCoreOptions(id.mid(5));                       // nested per-core options page
    };
    auto onBack = [this] { openSettingsHub(); };             // core picker is a hub child

    if (themedPanelIsTop(tr("Emulator Settings")))
        themedPanelHost_->replaceTop(tr("Emulator Settings"), rows, onAct, onBack);
    else
        themedPanelHost_->present(tr("Emulator Settings"), rows, onAct, onBack);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}

// The per-core options editor as a nested panel level: load the selected core headlessly (downloading first if
// needed), read its CoreOptions, and render each as a Choice row. Values persist keyed by core name — the SAME
// Settings::optionValue/setOptionValue the classic editor uses. Applied immediately on cycle (themed convention).
void MainWindow::editCoreOptions(const QString& systemId)
{
    QString core;
    for (const GameSystem& sys : SystemCatalog::systems())
        if (sys.id == systemId) { core = Settings::coreFor(sys.id); if (core.isEmpty()) core = sys.cores.value(0); break; }
    if (core.isEmpty()) { notify(tr("No core selected for this system.")); return; }

    // Ensure the core is present (download on first use), then load it headlessly to read its options. Progress +
    // failures surface as the themed notification toast (the panel isn't presented during the blocking load).
    QString dlErr;
    const QString corePath = CoreManager::ensureCore(core, &dlErr, [this, core](int pct) {
        notify(tr("Downloading core ‘%1’… %2%").arg(core).arg(pct), 2000);
    });
    if (corePath.isEmpty())
    {
        notify(dlErr.isEmpty() ? tr("Couldn't download core ‘%1’.").arg(core) : dlErr);
        return;
    }

    LibretroCore tmp;
    std::string err;
    if (!tmp.loadCore(corePath.toStdString(), &err))
    {
        notify(tr("Couldn't load core ‘%1’: %2").arg(core, QString::fromStdString(err)));
        return;
    }
    const std::vector<CoreOption> opts = tmp.options();   // copy out before unloading
    tmp.unload();

    // Per Choice row: the options list shows LABELS (what the classic combo shows); map each label back to its
    // value for setOptionValue. One label->value map per option key, captured for the activation handler.
    auto label2value = std::make_shared<QHash<QString, QHash<QString, QString>>>();
    QVector<PanelRow> rows;
    if (opts.empty())
    {
        PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("none");
        r.label = tr("This core doesn't expose any configurable options."); rows << r;
    }
    else for (const CoreOption& o : opts)
    {
        const QString key = QString::fromStdString(o.key);
        QStringList labels; QHash<QString, QString> l2v; QString curLabel;
        QString curVal = Settings::optionValue(core, key);
        if (curVal.isEmpty()) curVal = QString::fromStdString(o.defaultValue);
        for (const auto& vp : o.values)
        {
            const QString value = QString::fromStdString(vp.first), lbl = QString::fromStdString(vp.second);
            labels << lbl; l2v.insert(lbl, value);
            if (value == curVal) curLabel = lbl;
        }
        if (curLabel.isEmpty()) curLabel = labels.value(0);
        label2value->insert(key, l2v);
        PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("opt:") + key;
        r.label = QString::fromStdString(o.desc); r.value = curLabel; r.options = labels; rows << r;
    }

    themedPanelHost_->present(tr("%1 — Core Options").arg(core), rows,
        [this, core, label2value](const QString& rid, const QString& val) {
            if (!rid.startsWith(QStringLiteral("opt:"))) return;
            const QString key = rid.mid(4);
            const QString value = label2value->value(key).value(val, val);
            Settings::setOptionValue(core, key, value);      // immediate apply (persists to ini)
        },
        [] { /* nested: Back pops back to the core list */ });
}
#endif // MMV_HAVE_QML

void MainWindow::openInputMapping()
{
    // Stop the game so the remap has sole use of the controller (for "press a button" capture).
    retro_->stop();
#ifdef MMV_HAVE_QML
    // Themed mode: a themed SHELL on the Nav Contract. player/scope/turbo Choices + per-button binding Action
    // rows; activating a binding row enters CAPTURE (the ControllerRemapDialog's capture machinery — keyboard
    // grab + pad poll — driven headlessly from the shell). Classic ControllerRemapDialog stays for classic mode.
    if (themedHomeEnabled() && themedPanelHost_) { presentInputMapping(); return; }
#endif
    auto* dlg = new ControllerRemapDialog(retro_->gamepad(), retro_->keymap(), this);
    showDialogPanel(tr("Input Mapping"), dlg, [this](int) { openSettingsHub(); },
                    [this] { openSettingsHub(); });
}

#ifdef MMV_HAVE_QML
// The RetroPad buttons in editing order (mirrors ControllerRemapDialog::kRows — the stable
// RETRO_DEVICE_ID_JOYPAD_* ABI numbers). Kept local to the themed shell.
namespace {
struct RemapBtn { int retroId; const char* label; };
const RemapBtn kRemapRows[] = {
    { 4, "D-Pad Up" }, { 5, "D-Pad Down" }, { 6, "D-Pad Left" }, { 7, "D-Pad Right" },
    { 8, "A" },        { 0, "B" },          { 9, "X" },          { 1, "Y" },
    { 10, "L" },       { 11, "R" },         { 12, "L2" },        { 13, "R2" },
    { 14, "L3" },      { 15, "R3" },        { 2, "Select" },     { 3, "Start" },
};
}

// Present/rebuild the themed input-mapping shell. Bindings apply+persist immediately (Gamepad::setBinding /
// Keymap::setKey / Settings::setTurbo* all "update live + persist" to the CURRENT input scope) — the themed-panel
// convention, so there's no working-copy/Save step. A hub child -> nested present(), Back -> openSettingsHub.
void MainWindow::presentInputMapping()
{
    remapScope_ = Settings::inputScope();
    remapPort_ = 0;
    themedPanelHost_->setStyle(settingsPanelStyle());
    buildInputMappingRows(/*replace*/ false);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}

void MainWindow::buildInputMappingRows(bool replace)
{
    Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
    Keymap*  keys = retro_ ? retro_->keymap() : nullptr;

    // --- header Choices: player / profile-scope / turbo speed ---
    QStringList players;
    for (int p = 0; p < ControllerRemapDialog::kPlayers; ++p) players << tr("Player %1").arg(p + 1);

    QStringList scopeNames; QString curScopeName = tr("All systems (default)");
    scopeNames << tr("All systems (default)");
    for (const GameSystem& s : SystemCatalog::systems())
    {
        scopeNames << s.name;
        if (s.id == remapScope_) curScopeName = s.name;
    }

    QStringList turboNames; turboNames << tr("Slow") << tr("Medium") << tr("Fast") << tr("Ultra");
    const int hp = Settings::turboHalfPeriod();
    const QString curTurbo = hp >= 5 ? tr("Slow") : hp == 3 ? tr("Medium") : hp == 2 ? tr("Fast") : tr("Ultra");

    // --- controller status Info (a snapshot; refreshed on rebuild) ---
    QString status;
    if (pad && pad->portConnected(remapPort_))
    {
        const std::string n = pad->name(remapPort_);
        status = tr("Player %1 controller: %2").arg(remapPort_ + 1)
                     .arg(n.empty() ? tr("connected") : QString::fromStdString(n));
    }
    else if (pad && pad->connected())
        status = tr("No controller in this slot — capturing from any connected controller.");
    else
        status = tr("No controller detected — connect one to assign controller buttons.");

    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("player"); r.label = tr("Editing player");
      r.value = players.value(remapPort_); r.options = players; rows << r; }
    { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("scope"); r.label = tr("Profile");
      r.value = curScopeName; r.options = scopeNames; rows << r; }
    { PanelRow r; r.kind = PanelRow::Choice; r.id = QStringLiteral("turbo"); r.label = tr("Turbo speed");
      r.value = curTurbo; r.options = turboNames; rows << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("status"); r.label = status; rows << r; }
    { PanelRow s; s.kind = PanelRow::Separator; s.id = QStringLiteral("sep"); s.label = tr("Buttons"); rows << s; }

    // --- per-button: controller binding + keyboard binding Actions + a turbo Toggle ---
    for (const RemapBtn& b : kRemapRows)
    {
        const QString bn = tr(b.label);
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("pad:") + QString::number(b.retroId);
          r.label = tr("%1 — Controller").arg(bn);
          r.value = pad ? QString::fromStdString(Gamepad::labelFor(pad->binding(remapPort_, b.retroId)))
                        : tr("—"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("key:") + QString::number(b.retroId);
          r.label = tr("%1 — Keyboard").arg(bn);
          r.value = keys ? Keymap::labelFor(keys->key(remapPort_, b.retroId)) : tr("—"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Toggle; r.id = QStringLiteral("turbo:") + QString::number(b.retroId);
          r.label = tr("%1 — Turbo (autofire)").arg(bn);
          r.checked = Settings::turboButton(remapPort_, b.retroId); rows << r; }
    }

    // Classic parity: Reset to Defaults for the currently-shown player (pad + keyboard + turbo), applied
    // immediately (the themed convention — classic staged it until Save).
    { PanelRow s; s.kind = PanelRow::Separator; s.id = QStringLiteral("sep2"); rows << s; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("reset");
      r.label = tr("Reset to Defaults (this player)"); rows << r; }

    auto onAct = [this](const QString& id, const QString& val) {
        // Defensive: a graph activation must never arrive mid-capture (the qApp filter swallows keys AND mouse
        // while remap_.active), but if one ever does, cancel first so the filter/pad-timer can't leak.
        if (remap_.active) endInputCapture(/*cancelled*/ true);
        if (id == QStringLiteral("reset"))
        {
            Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
            Keymap*  keys = retro_ ? retro_->keymap() : nullptr;
            for (const RemapBtn& b : kRemapRows)
            {
                if (pad)  pad->setBinding(remapPort_, b.retroId, Gamepad::defaultBinding(b.retroId));
                if (keys) keys->setKey(remapPort_, b.retroId, Keymap::defaultKey(remapPort_, b.retroId));
                Settings::setTurboButton(remapPort_, b.retroId, false);
            }
            buildInputMappingRows(/*replace*/ true);
        }
        else if (id == QStringLiteral("player"))
        {
            const int idx = qMax(0, val.section(' ', 1, 1).toInt() - 1);   // "Player N" -> N-1
            remapPort_ = qBound(0, idx, ControllerRemapDialog::kPlayers - 1);
            buildInputMappingRows(/*replace*/ true);
        }
        else if (id == QStringLiteral("scope"))
        {
            QString newScope;   // "All systems (default)" -> "" (global)
            for (const GameSystem& s : SystemCatalog::systems()) if (s.name == val) { newScope = s.id; break; }
            remapScope_ = newScope;
            Settings::setInputScope(newScope);
            if (Gamepad* g = retro_ ? retro_->gamepad() : nullptr) g->reloadMapping();
            if (Keymap*  k = retro_ ? retro_->keymap()  : nullptr) k->reload();
            buildInputMappingRows(/*replace*/ true);
        }
        else if (id == QStringLiteral("turbo"))
        {
            const int hp2 = val == tr("Slow") ? 5 : val == tr("Medium") ? 3 : val == tr("Fast") ? 2 : 1;
            Settings::setTurboHalfPeriod(hp2);
        }
        else if (id.startsWith(QStringLiteral("turbo:")))
        {
            const int rid = id.mid(6).toInt();
            Settings::setTurboButton(remapPort_, rid, val == QStringLiteral("1"));   // Toggle delivers "1"/"0"
        }
        else if (id.startsWith(QStringLiteral("pad:")))  beginInputCapture(id.mid(4).toInt(), /*keyboard*/ false);
        else if (id.startsWith(QStringLiteral("key:")))  beginInputCapture(id.mid(4).toInt(), /*keyboard*/ true);
    };
    // Leaving the panel mid-capture must tear the capture down with it (filter removed, pad timer stopped) —
    // unreachable while the modal filter swallows keys+mouse, but the boundary owns its own cleanup regardless.
    auto onBack = [this] { if (remap_.active) endInputCapture(/*cancelled*/ true); openSettingsHub(); };

    if (replace && themedPanelIsTop(tr("Input Mapping")))
        themedPanelHost_->replaceTop(tr("Input Mapping"), rows, onAct, onBack);
    else
        themedPanelHost_->present(tr("Input Mapping"), rows, onAct, onBack);
}

// Enter CAPTURE for one binding: patch the row to the "Press a …" prompt, then run the SAME capture machinery as
// ControllerRemapDialog — a pad-poll timer for a controller input, an application-level key filter for a keyboard
// key (installed on qApp so it sees the key BEFORE the QQuickWidget's QML nav consumes it). Esc cancels. Modal:
// while active, every physical key is swallowed by inputCaptureKeyFilter.
void MainWindow::beginInputCapture(int retroId, bool keyboard)
{
    if (remap_.active) endInputCapture(/*cancelled*/ true);
    remap_ = { true, keyboard, remapPort_, retroId, false };

    // Patch the target row to the capture prompt.
    PanelRow r; r.kind = PanelRow::Action;
    r.id = (keyboard ? QStringLiteral("key:") : QStringLiteral("pad:")) + QString::number(retroId);
    const char* bn = "";
    for (const RemapBtn& b : kRemapRows) if (b.retroId == retroId) { bn = b.label; break; }
    r.label = keyboard ? tr("%1 — Keyboard").arg(tr(bn)) : tr("%1 — Controller").arg(tr(bn));
    r.value = keyboard ? tr("Press a key…  (Esc cancels)") : tr("Press a button…  (Esc cancels)");
    themedPanelHost_->updateRow(r.id, r);

    qApp->installEventFilter(this);   // see physical keys before the QML scene (removed in endInputCapture)
    if (!keyboard)
    {
        if (!remapPadTimer_)
        {
            remapPadTimer_ = new QTimer(this);
            remapPadTimer_->setInterval(30);
            connect(remapPadTimer_, &QTimer::timeout, this, &MainWindow::onInputCapturePadTick);
        }
        remapPadTimer_->start();
    }
}

void MainWindow::onInputCapturePadTick()
{
    if (!remap_.active || remap_.keyboard) return;
    Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
    if (!pad) return;
    pad->poll();
    const int code = pad->anyPressed(remap_.port);      // prefer this player's controller
    if (!remap_.sawRelease) { if (code == Gamepad::kUnbound) remap_.sawRelease = true; return; }
    if (code != Gamepad::kUnbound)
    {
        pad->setBinding(remap_.port, remap_.retroId, code);   // update live + persist
        endInputCapture(/*cancelled*/ false);                 // ends + schedules the deferred row refresh
    }
}

// The app-level key filter while capturing: Esc cancels; in keyboard mode the next key binds; everything is
// swallowed (modal). Returns true when the event is consumed.
bool MainWindow::inputCaptureKeyFilter(QKeyEvent* e)
{
    if (!remap_.active) return false;
    // Esc cancels; Key_Back (Android/TV-remote Back, which has no Escape) is accepted as a cancel alias so a
    // remote user can always back out of capture.
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Back) { endInputCapture(/*cancelled*/ true); return true; }
    if (remap_.keyboard)
    {
        Keymap* keys = retro_ ? retro_->keymap() : nullptr;
        const int rid = remap_.retroId, port = remap_.port, k = e->key();
        if (keys)
        {
            keys->setKey(port, rid, k);   // update live + persist
            // A key drives one button within this profile; clear it from any other button (data only — the whole
            // grid's labels are re-patched by the deferred refresh below, so every cleared row updates too).
            for (const RemapBtn& b : kRemapRows)
                if (b.retroId != rid && keys->key(port, b.retroId) == k)
                    keys->setKey(port, b.retroId, Keymap::kUnbound);
        }
        endInputCapture(/*cancelled*/ false);
    }
    // pad mode: swallow non-Esc keys (stay in capture — the button arrives via the pad tick)
    return true;
}

void MainWindow::endInputCapture(bool /*cancelled*/)
{
    if (!remap_.active) return;
    remap_.active = false;
    if (remapPadTimer_) remapPadTimer_->stop();
    qApp->removeEventFilter(this);
    // Refresh the binding labels AFTER this event dispatch returns to the loop. Patching the model mid-dispatch
    // (we're inside the qApp key filter, or a pad-timer tick) reliably repaints only the SELECTED delegate — a
    // non-selected row cleared by the duplicate-key rule kept showing its stale binding. A singleShot(0) runs the
    // updateRow patches from a clean context, so the bound row AND every cleared row refresh. Cursor is preserved
    // (updateRow patches in place; both cancelled and committed captures re-read current state).
    QTimer::singleShot(0, this, [this] { refreshInputButtonRows(); });
}

// Re-patch every button row's current binding label (controller + keyboard) in place — cursor untouched. Used
// after a capture commits/cancels so the whole grid reflects current state (incl. duplicate-cleared rows).
void MainWindow::refreshInputButtonRows()
{
    if (!themedPanelIsTop(tr("Input Mapping"))) return;
    Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;
    Keymap*  keys = retro_ ? retro_->keymap() : nullptr;
    for (const RemapBtn& b : kRemapRows)
    {
        const QString bn = tr(b.label);
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("pad:") + QString::number(b.retroId);
          r.label = tr("%1 — Controller").arg(bn);
          r.value = pad ? QString::fromStdString(Gamepad::labelFor(pad->binding(remapPort_, b.retroId))) : tr("—");
          themedPanelHost_->updateRow(r.id, r); }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("key:") + QString::number(b.retroId);
          r.label = tr("%1 — Keyboard").arg(bn);
          r.value = keys ? Keymap::labelFor(keys->key(remapPort_, b.retroId)) : tr("—");
          themedPanelHost_->updateRow(r.id, r); }
    }
}
#endif // MMV_HAVE_QML

void MainWindow::onDuration(double seconds)
{
    duration_ = seconds;
    session_->setDuration(seconds);
#ifdef MMV_HAVE_QML
    if (themedAudioSession_) updateThemedAudioProgress(); // refresh the page's total-time once the length is known
#endif
    // Resume where we left off, now that the file is loaded and its length is known. Skip if the saved spot
    // is essentially the end (treat a near-finished file as "watched" and start it fresh).
    const double at = session_->takeResumeSeek(); // one-shot
    if (at > 1.0 && at < seconds - 5.0)
        player_->setPosition(at);
}

void MainWindow::onPosition(double seconds)
{
    if (!sliderDown_ && duration_ > 0.0)
        seek_->setValue(static_cast<int>(seconds / duration_ * 1000.0));
    time_->setText(fmt(seconds) + QStringLiteral(" / ") + fmt(duration_));

    session_->setPosition(seconds); // updates the tracked position and throttles resume writes internally

    // Themed audio now-playing page: feed the progress bar at ~1 Hz (a whole-second change), not at mpv's
    // event rate — the bar steps once a second, never re-rendering the full-screen QML page continuously.
#ifdef MMV_HAVE_QML
    if (themedAudioSession_)
    {
        const int sec = int(seconds);
        if (sec != themedAudioPushSec_) { themedAudioPushSec_ = sec; updateThemedAudioProgress(); }
    }
#endif
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
