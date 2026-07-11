#include "MainWindow.h"
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
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
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
#include "../core/PcGameStore.h"
#include "../core/DownloadsStore.h"
#include "../core/DownloadManager.h"
#include "../core/PlayStats.h"
#include "../core/RomLibrary.h"
#include "../core/BiosCatalog.h"
#include "../core/ProfileStore.h"
#include "../core/Theme.h"
#include "../core/CloudSync.h"
#include "ProfileDialog.h"
#include "RegistryBrowser.h"
#include "../core/MetaCache.h"
#include "../core/UiTestServer.h"
#include "nav/Nav.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"
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
#include <QWindow>
#include <QPointer>
#include <QImage>
#include <QScreen>
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

#ifdef MMV_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include <QQuickItem>
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

// ---- .m3u / .m3u8 playlist support ------------------------------------------------------------
// Three flavours share this extension: an HLS manifest (segment list / master) which libmpv streams
// directly; an IPTV-style media playlist (a list of channel/track URLs) which we turn into a queue;
// and a PlayStation multi-disc list which the emulator loads. handleM3u() tells them apart.
namespace {
struct M3uEntry { QString title; QString url; };

// True when the URL/path points at a playlist file (ignoring any ?query).
bool isM3uRef(const QString& s)
{
    QString p = s;
    const int q = p.indexOf(QLatin1Char('?'));
    if (q >= 0) p = p.left(q);
    p = p.toLower();
    return p.endsWith(QStringLiteral(".m3u")) || p.endsWith(QStringLiteral(".m3u8"));
}

// HLS manifests carry #EXT-X-* tags (TARGETDURATION, STREAM-INF, MEDIA-SEQUENCE, …); a plain media
// playlist has only #EXTM3U/#EXTINF and full entry URLs. The former is one stream for libmpv to chew.
bool isHlsManifest(const QString& text) { return text.contains(QStringLiteral("#EXT-X-")); }

// Parse #EXTINF titles + entry URLs, resolving relative entries against the playlist's own location.
QVector<M3uEntry> parseM3u(const QString& text, const QString& src)
{
    QVector<M3uEntry> out;
    const bool srcIsUrl = src.contains(QStringLiteral("://"));
    const int slash = src.lastIndexOf(QLatin1Char('/'));
    const QString base = srcIsUrl ? (slash >= 0 ? src.left(slash + 1) : src)
                                  : (QFileInfo(src).absolutePath() + QLatin1Char('/'));
    QString title;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("#EXTINF")))
        {
            const int c = line.indexOf(QLatin1Char(','));
            if (c >= 0) title = line.mid(c + 1).trimmed(); // text after the last comma is the display name
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) continue; // any other directive
        QString url;
        if (line.contains(QStringLiteral("://")))        url = line;                              // absolute
        else if (srcIsUrl)                               url = QUrl(base).resolved(QUrl(line)).toString();
        else if (QFileInfo(line).isAbsolute())           url = line;
        else                                             url = base + line;                       // relative to file
        out.push_back({ title.isEmpty() ? QFileInfo(line).fileName() : title, url });
        title.clear();
    }
    return out;
}

// A PlayStation multi-disc list: every entry is a disc image the libretro core can swap between.
bool looksLikeDiscPlaylist(const QVector<M3uEntry>& entries)
{
    if (entries.isEmpty()) return false;
    static const QStringList disc = { "cue", "chd", "bin", "iso", "pbp", "img", "ccd" };
    for (const M3uEntry& e : entries)
    {
        const QString path = QUrl(e.url).path().isEmpty() ? e.url : QUrl(e.url).path();
        if (!disc.contains(QFileInfo(path).suffix().toLower())) return false;
    }
    return true;
}
} // namespace

// Per-profile settings store (resume positions, etc.), mirroring the accessor the other views use.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

static QPushButton* panelRow(const QString& label); // large TV-friendly menu row (defined below)

// Stable, path-derived key prefix for one file's resume state (shared by video / audio / audiobooks).
static QString mediaResumeKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("resume/") + QString::fromLatin1(h) + QStringLiteral("/");
}

// Pre-generalization audiobooks were stored under "audiobook/"; read those too so in-progress books resume.
static QString legacyAudiobookKey(const QString& path)
{
    const QByteArray h = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    return QStringLiteral("audiobook/") + QString::fromLatin1(h) + QStringLiteral("/");
}

MainWindow::MainWindow(bool chooseProfileAtStart, QWidget* parent)
    : QMainWindow(parent), startupChooseProfile_(chooseProfileAtStart)
{
    player_ = new MpvWidget(this);
    retro_ = new RetroView(this);
    if (retro_->gamepad()) mwLog(QString::fromStdString(retro_->gamepad()->describeControllers()));
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
    connect(castMgr_, &CastManager::castError, this, [this](const QString& msg) { notify(msg, 6000); });
    connect(castMgr_, &CastManager::castStopped, this, [this] { notify(tr("Stopped casting."), 3000); });

    subFetcher_ = new SubtitleFetcher(this);
    connect(subFetcher_, &SubtitleFetcher::log, this, [this](const QString& line) { mwLog(line); });
    connect(player_, &MpvWidget::fileLoaded, this, [this](bool hasSub, bool isVideo) {
        if (speedBtn_) speedBtn_->setText(QString::number(player_->speed(), 'g', 3) + QStringLiteral("×")); // reset to 1× per file
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
        retro_->showAchievement(title, desc, pts, badge); // on-screen popup over the game (full-screen has no status bar)
        statusBar()->showMessage(tr("🏆  Achievement unlocked: %1  (%2 pts)").arg(title).arg(pts), 8000);
    });
    connect(ach_, &Achievements::gameLoaded, this, [this](bool ok, const QString& title, int unlocked, int total) {
        if (ok && total > 0)
            statusBar()->showMessage(tr("🏆  %1 — %2/%3 achievements").arg(title).arg(unlocked).arg(total), 6000);
        else if (ok && title.contains(QStringLiteral("Unsupported Game Version"), Qt::CaseInsensitive))
            // RA recognized the ROM but it isn't the dump the achievement set is tied to — tell the user why
            // there are no achievements so they know to grab the RetroAchievements-supported (No-Intro) dump.
            notify(tr("🏆  This ROM isn't a RetroAchievements-supported version — no achievements. "
                      "Try the No-Intro/RA-verified dump."), 8000);
        else if (ok)
            statusBar()->showMessage(tr("🏆  No RetroAchievements set for this game."), 5000);
    });
    ach_->tryLoginWithStoredToken();

    addons_ = std::make_unique<AddonManager>();
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
    // State changes (queued/active/failed/removed) change which buttons a row needs, so rebuild the panel.
    connect(dm_, &DownloadManager::changed, this, [this] {
        if (dlPanelOpen_ && stack_->currentWidget() == panelPage_) openDownloadManager();
    });

    home_ = new HomeView(addons_.get(), this);
    connect(home_, &HomeView::openItem, this, &MainWindow::openLibraryItem);
    connect(home_, &HomeView::downloadItem, this, &MainWindow::enqueueDownload);
    connect(home_, &HomeView::openImagePages, this, &MainWindow::openImagePages);

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
    bgm_ = new BackgroundMusic(this);
    connect(stack_, &QStackedWidget::currentChanged, this, [this] {
        updateBackgroundMusic();
        // Remember the full-screen state as we ENTER content (a game / video / reader), so exiting back to the
        // home restores it instead of always dropping to a window (see openHome). Content doesn't change the
        // full-screen state itself, so isFullScreen() here is the browsing state we want to come back to.
        QWidget* w = stack_->currentWidget();
        const bool content = (w == retro_ || w == playerPage_ || w == book_ || w == pdf_ || w == comic_ || w == emuPage_);
        if (content && !inContent_) fsBeforeContent_ = isFullScreen();
        inContent_ = content;
    });
    connect(bgm_, &BackgroundMusic::nowPlayingChanged, this, [this] { updateThemedNowPlaying(); }); // Triple theme readout
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
    connect(updater_, &AppUpdater::applyFailed, this, [this](const QString& why) { notify(why, 8000); });
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

        // Opening a movie/book/… from the themed home shows the classic info page (it renders on HomeView, which
        // the themed home hides). Surface it; the themed column is left untouched (this switch makes its guard
        // above false), so backing out returns to exactly where we were.
        connect(home_, &HomeView::infoPageRequested, this, [this] {
            QWidget* cur = stack_->currentWidget();
            if (!themedHomeEnabled() || (cur != themedHome_ && cur != themedBrowse_)) return;
            themedDetailFrom_ = cur;
            themedReturnAfterDetail_ = true;
            stack_->setCurrentWidget(home_);
            home_->focusContent();
        });
        // When the user backs out of that info page (HomeView leaves the detail level), return to the themed
        // home/browse we came from - still showing the same column at the same item.
        connect(home_, &HomeView::browseItemsChanged, this, [this](bool) {
            if (!themedReturnAfterDetail_ || stack_->currentWidget() != home_ || home_->atDetailLevel()) return;
            themedReturnAfterDetail_ = false;
            QWidget* back = themedDetailFrom_ ? themedDetailFrom_ : themedHome_;
            if (back) { stack_->setCurrentWidget(back); back->setFocus(Qt::OtherFocusReason); }
        });

        // Triple/XMB: the live metadata for a browse-item arrived (a skeleton, then synopsis + facts). Merge
        // it into the cross's metadata panel - but only if it's still the selected row of the open catalog.
        connect(home_, &HomeView::themedMetaReady, this, [this](int index, const QVariantMap& meta) {
            if (!themedHomeIsXmb_ || !themedXmbInCatalog_ || stack_->currentWidget() != themedHome_) return;
            QQuickItem* r = ThemeEngine::rootItem(themedHome_);
            if (!r || r->property("currentIndex").toInt() != index) return; // moved on -> stale, ignore
            QVariantMap cur = r->property("selectedMeta").toMap();
            for (auto it = meta.constBegin(); it != meta.constEnd(); ++it) cur.insert(it.key(), it.value());
            r->setProperty("selectedMeta", cur);
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
        player_->stop(); mediaControls_->hide(); videoBack_->hide(); clearAudioQueue(); openHome();
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
        showPlayerNotice(tr("Finding another source…"), 30000);
        home_->requestNextSource();
    });

    // Transient centred message over the player for next-source feedback (visible in full screen, where the
    // status bar isn't). Hidden by default.
    playerNotice_ = new QLabel(player_);
    playerNotice_->setObjectName(QStringLiteral("playerNotice"));
    playerNotice_->setStyleSheet(QStringLiteral(
        "#playerNotice { background: rgba(20,20,24,0.90); color:#f2f2f2; border-radius:8px; padding:10px 18px;"
        " font-size:15px; font-weight:bold; }"));
    playerNotice_->setAlignment(Qt::AlignCenter);
    playerNotice_->hide();
    playerNoticeTimer_ = new QTimer(this);
    playerNoticeTimer_->setSingleShot(true);
    connect(playerNoticeTimer_, &QTimer::timeout, this, [this] { if (playerNotice_) playerNotice_->hide(); });

    // Notification overlay (download/resolve progress + errors). A CHILD widget of the central area, raised
    // over the current page — NOT a separate top-level window. A top-level window is trapped behind a
    // foreground fullscreen main window (Windows' boosted fullscreen z-band), so it only appeared when you
    // alt-tabbed away. As a child it's part of the window and composites over everything: the QQuickWidget
    // themed home and the libmpv QOpenGLWidget both composite with sibling widgets. Click-through, no focus.
    notice_ = new QLabel(centralWidget());
    notice_->setObjectName(QStringLiteral("mwNotice"));
    notice_->setAttribute(Qt::WA_TransparentForMouseEvents);
    notice_->setFocusPolicy(Qt::NoFocus);
    notice_->setWordWrap(true);
    notice_->setAlignment(Qt::AlignCenter);
    notice_->setStyleSheet(QStringLiteral(
        "#mwNotice { background:rgba(18,20,26,0.95); color:#f4f6f8; border:1px solid rgba(255,255,255,0.18);"
        " border-radius:10px; padding:12px 22px; font-size:12pt; font-weight:600; }"));
    notice_->hide();
    noticeTimer_ = new QTimer(this);
    noticeTimer_->setSingleShot(true);
    connect(noticeTimer_, &QTimer::timeout, this, [this] { if (notice_) notice_->hide(); });

    controlsHideTimer_ = new QTimer(this);
    controlsHideTimer_->setSingleShot(true);
    connect(controlsHideTimer_, &QTimer::timeout, this, [this] {
        // Hide after the inactivity timeout. Every interaction (mouse move or arrow-key navigation) calls
        // revealMediaControls(), which restarts this timer - so the controls stay up while you're actively
        // navigating and fade out a few seconds after you stop. Clear keyboard focus so the next arrow press
        // cleanly re-reveals and re-focuses a button. In full screen also blank the cursor.
        QWidget* fw = focusWidget();
        if (fw && (fw == videoBack_ || fw == streamIssueBtn_ || (mediaControls_ && mediaControls_->isAncestorOf(fw))))
            fw->clearFocus();
        mediaControls_->hide();
        videoBack_->hide();
        streamIssueBtn_->hide();
        if (isFullScreen() && stack_->currentWidget() == playerPage_ && !subOverlay_)
            player_->setCursor(Qt::BlankCursor); // but never hide the cursor while the subtitle panel is open
    });

    // Reveal the controls on mouse movement over the player / controls.
    player_->setMouseTracking(true);
    player_->installEventFilter(this);
    mediaControls_->installEventFilter(this);

    // HomeView's progress/error toasts now render as our window-level overlay so they stay visible over any
    // theme (a themed home is a native QQuickView the view's own toast couldn't cover).
    connect(home_, &HomeView::toastRequested, this, &MainWindow::notify);
    connect(home_, &HomeView::toastHideRequested, this, &MainWindow::hideNotice);
    connect(home_, &HomeView::requestOpenFile, this, &MainWindow::onRequestOpenFile);
    connect(home_, &HomeView::openRecent, this, &MainWindow::openRecent);
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
        book_->persist(); pdf_->persist(); comic_->persist();
        stack_->setCurrentWidget(home_);
        home_->focusContent();
    };
    connect(comic_, &ComicView::backRequested, this, returnFromReader);
    connect(book_,  &EbookView::backRequested, this, returnFromReader);
    connect(pdf_,   &PdfView::backRequested,   this, returnFromReader);
    // Reader "Issue with Streaming": ask the file provider for the next source and re-open the new file.
    connect(book_, &EbookView::streamIssueRequested, this, [this] {
        showNextSourceFeedback(tr("Finding another source…")); home_->requestNextSource(); });
    connect(pdf_,  &PdfView::streamIssueRequested,   this, [this] {
        showNextSourceFeedback(tr("Finding another source…")); home_->requestNextSource(); });
    // Result of a next-source request: on success the new file opens itself; on failure show why.
    connect(home_, &HomeView::nextSourceResult, this, [this](bool ok, const QString& msg) {
        if (ok) { if (playerNotice_) playerNotice_->hide(); }
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
    connect(retro_, &RetroView::exitRequested, this, [this] { retro_->stop(); openHome(); });
    // Bank the elapsed session whenever the full-screen game is torn down (Exit, or switching to other content).
    connect(retro_, &RetroView::gameStopped, this, [this] { endPlaySession(); });
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
    auto* splitShortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
    connect(splitShortcut, &QShortcut::activated, this, [this] { if (splitMode_) exitSplitScreen(); else enterSplitScreen(); });

    showHomeScreen(); // the catalog landing screen (classic, or the themed home if enabled) is shown first

    // Pull another device's "continue watching" progress and merge it in, shortly after startup so it doesn't
    // block launch or hit the network before the UI is up. No-op if not signed into cloud sync.
    QTimer::singleShot(1500, this, [this] { pullAndMergeProgress(); });
}

MainWindow::~MainWindow() = default; // AddonManager is complete in this translation unit

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
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
    positionNotice();
}

void MainWindow::moveEvent(QMoveEvent* event)
{
    QMainWindow::moveEvent(event);
    positionNotice(); // the notice is a separate top-level window; keep it stuck to us as we drag
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
    connect(menu, &NavOverlay::closed, this, [this](int) {
#ifdef MMV_HAVE_QML
        // A QQuickWidget forwards focus to its QML scene, but after a keyboard grab the scene's active-focus
        // item may need a kick so its Keys handler (and the selection) resume exactly where they were.
        QWidget* cur = stack_->currentWidget();
        if (cur == themedHome_ || cur == themedBrowse_)
        {
            cur->setFocus(Qt::OtherFocusReason);
            if (auto* r = ThemeEngine::rootItem(cur)) r->forceActiveFocus();
        }
#endif
    });
}

void MainWindow::hideEscMenu()
{
    if (auto* overlay = qobject_cast<NavOverlay*>(escMenuOverlay_.data())) overlay->dismiss(-1);
}

// ---- Controller navigation of the menus (EmulationStation-style) ---------------------------------------

// libretro RETRO_DEVICE_ID_JOYPAD_* ids used for menu navigation (B/south = confirm, A/east = back — the
// Gamepad's default mapping; the left stick also drives the d-pad ids past a deadzone).
namespace { constexpr int PAD_B = 0, PAD_START = 3, PAD_UP = 4, PAD_DOWN = 5, PAD_LEFT = 6, PAD_RIGHT = 7, PAD_A = 8; }

void MainWindow::sendNavKey(int key)
{
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
    QWidget* cur = stack_->currentWidget();
    // 4. The themed home/browse is a QQuickWidget — hand it the key directly; its QML Keys handler does the
    //    arrow nav AND its own multi-level Back (drill up, then the pause menu), matching goBack's rule.
    if (cur && (cur == themedHome_ || cur == themedBrowse_)) { deliver(cur, key); return; }
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

// The one Back rule for the whole app: Escape, Backspace and the controller's Back all call this, so a
// "go back" gesture behaves identically on every screen — it always takes you to the previous screen, and
// only at the home root does it open the app pause menu (Resume / Exit). Anything layered on top (an
// overlay/menu/keyboard, or the pause menu itself) is dismissed first.
void MainWindow::goBack()
{
    if (NavOverlay* top = NavOverlay::topmost()) { top->dismiss(-1); return; } // close the thing on top
    if (subOverlay_ && subOverlay_->isVisible()) { hideSubtitleMenu(); return; }
    if (escMenuVisible()) { hideEscMenu(); return; }                            // pause menu -> resume

    QWidget* cur = stack_->currentWidget();

    // Themed (QML) home/browse: it owns a multi-level back (drill up a catalog, then the pause menu at its
    // root) — run the same handler its own back() signal uses.
    if ((cur == themedHome_ || cur == themedBrowse_) && themedOnBack_) { themedOnBack_(); return; }
    // Classic home: pop a drill level, or (at the root) emit backRequested -> the pause menu.
    if (cur == home_) { home_->goBack(); return; }
    // Settings / dialog panels: their header Back = the previous panel or the screen we came from.
    if (cur == panelPage_) { if (panelBack_) panelBack_->click(); return; }
    // Library: unwind a pushed sub-page (registry/addon settings), else back to the Settings hub.
    if (cur == library_) { if (!library_->navBack()) openSettingsHub(); return; }
    // Readers: back to the home they were opened from (persist positions first).
    if (cur == book_ || cur == pdf_ || cur == comic_)
    { book_->persist(); pdf_->persist(); comic_->persist(); stack_->setCurrentWidget(home_); home_->focusContent(); return; }
    // Standalone-emulator wait page: close the emulator.
    if (cur == emuPage_) { if (emuStopBtn_) emuStopBtn_->click(); return; }
    // Split screen: leave it.
    if (splitMode_) { exitSplitScreen(); return; }
    // Player (and any other content page): stop playback and return home.
    player_->stop();
    if (mediaControls_) mediaControls_->hide();
    if (videoBack_) videoBack_->hide();
    clearAudioQueue();
    openHome();
}

// Re-register the nav kit for the page the stack just switched to: which ring drives the arrow keys and
// what the pad's Back does there. Screens with their own complete internal navigation (home, themed QML,
// readers — which already map Backspace to back — and the emulator) register neither, so their key
// handling is untouched; overlays still route above them.
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
        navCtx_->setActiveRing(nullptr);
        navCtx_->setBackAction(nullptr);
    }
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
    // What Enter would act on right now: a corner button, the inline action chooser, or the tile above.
    if (r->property("focusZone").toInt() == 1)
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

void MainWindow::updateUiTestServer()
{
    if (!UiTestServer::wanted())
    {
        if (uiTest_) { delete uiTest_; uiTest_ = nullptr; mwLog(QStringLiteral("uitest: control channel stopped")); }
        return;
    }
    if (uiTest_) return; // already listening
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
        o.insert(QStringLiteral("escMenu"), escMenuVisible());
        o.insert(QStringLiteral("fullscreen"), isFullScreen());
        o.insert(QStringLiteral("active"), isActiveWindow());
        o.insert(QStringLiteral("size"), QStringLiteral("%1x%2").arg(width()).arg(height()));
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    h.screenshot = [this](const QString& path) { return grab().save(path); };
    uiTest_ = new UiTestServer(h, this);
    mwLog(QStringLiteral("uitest: control channel listening (%1)").arg(UiTestServer::serverName()));
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
    if (!NavOverlay::topmost() && (qobject_cast<QTextEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw))) return;

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
    if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Backspace)
    {
        QWidget* fw = focusWidget();
        const bool typing = e->key() == Qt::Key_Backspace
                            && (qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw)
                                || qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QAbstractSpinBox*>(fw));
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
        case Qt::Key_Escape: case Qt::Key_Backspace: hideSubtitleMenu(); return;
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
    // Become the active window and drop keyboard focus into the home view so arrow keys work without a
    // click first. Deferred a tick so it runs after the window is actually on screen / activated.
    raise();
    activateWindow();
    QTimer::singleShot(0, this, [this] {
        activateWindow();
        if (startupChooseProfile_) { promptStartupProfile(); return; } // pick a user before anything else
        if (stack_->currentWidget() == home_ && home_) home_->focusContent();
    });
}

// When the window is re-activated (alt-tab back, restore from minimised, the Esc menu closing), the themed
// QQuickWidget doesn't automatically restore its QML scene's active focus, leaving arrow keys dead until a
// click. Re-focus it AND force focus onto the QML root so navigation keeps working.
void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        QTimer::singleShot(0, this, [this] {
            QWidget* w = stack_ ? stack_->currentWidget() : nullptr;
            if (w && (w == themedHome_ || w == themedBrowse_))
            {
                w->setFocus(Qt::ActiveWindowFocusReason);
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
    auto* dlg = new ProfileDialog(/*mustChoose*/ true, this);
    showDialogPanel(tr("Who's using My Media Vault?"), dlg, [this, dlg](int result) {
        if (result == QDialog::Accepted && !dlg->selectedId().isEmpty())
        {
            ProfileStore::setCurrent(dlg->selectedId());
            openHome();                    // render for the chosen profile
        }
        else
        {
            QApplication::quit(); // declined to choose -> exit (matches the old must-choose behaviour)
        }
    }, [this] { QApplication::quit(); }); // Back == decline -> exit
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
    if (playerNotice_ && playerNotice_->isVisible())
    {
        playerNotice_->adjustSize();
        playerNotice_->move((player_->width() - playerNotice_->width()) / 2, margin + videoBack_->height() + 14);
    }
    if (subOverlay_) subOverlay_->setGeometry(player_->rect()); // keep the subtitle scrim covering the player
}

void MainWindow::showPlayerNotice(const QString& msg, int ms)
{
    if (!playerNotice_) return;
    playerNotice_->setText(msg);
    playerNotice_->adjustSize();
    const int margin = 16;
    playerNotice_->move((player_->width() - playerNotice_->width()) / 2, margin + videoBack_->height() + 14);
    playerNotice_->show();
    playerNotice_->raise();
    playerNoticeTimer_->start(ms);
}

void MainWindow::positionNotice()
{
    if (!notice_ || !notice_->isVisible()) return;
    QWidget* area = notice_->parentWidget() ? notice_->parentWidget() : this;
    notice_->setMaximumWidth(qMax(280, int(area->width() * 0.7)));
    notice_->adjustSize();
    // Child overlay: local coordinates over the bottom-centre of the central area.
    const int x = (area->width() - notice_->width()) / 2;
    const int y = area->height() - notice_->height() - 56; // floats just above the bottom edge
    notice_->move(qMax(8, x), qMax(8, y));
    notice_->raise(); // keep it above the current page
}

void MainWindow::notify(const QString& text, int ms)
{
    if (!notice_) return;
    notice_->setText(text);
    notice_->setMaximumWidth(qMax(280, int(width() * 0.7)));
    notice_->adjustSize();
    notice_->show();
    notice_->raise();
    positionNotice();
    notice_->repaint(); // paint synchronously now, so a message set right before a blocking step (e.g. archive
                        // extraction) is actually visible instead of queued behind the freeze
    if (noticeTimer_) { if (ms > 0) noticeTimer_->start(ms); else noticeTimer_->stop(); } // ms<=0 => sticky
}

void MainWindow::hideNotice()
{
    if (notice_) notice_->hide();
    if (noticeTimer_) noticeTimer_->stop();
}

void MainWindow::showNextSourceFeedback(const QString& msg)
{
    // Over the player while playing (the status bar may be hidden in full screen); otherwise the status bar
    // (the book/PDF readers keep it visible).
    if (stack_->currentWidget() == playerPage_) showPlayerNotice(msg);
    else                                        statusBar()->showMessage(msg, 6000);
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

void MainWindow::openVideoPath(const QString& path)
{
    if (isM3uRef(path)) { openM3u(path, QFileInfo(path).completeBaseName()); return; } // playlist, not a plain file
    if (splitTarget_) { splitTarget_->openVideo(path, QFileInfo(path).completeBaseName()); finishSplitOpen(); return; }
    subCtx_ = {};                      // a local file isn't matched to a catalog title/IMDB id for subtitles
    currentNextSourceCapable_ = false; // a local file has no Allarr alternate source
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();      // saves+clears any previous timed media
    beginResume(path);      // track this video's position (and resume it if we've watched it before)
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
    currentNextSourceCapable_ = false; // a local file/folder has no Allarr alternate source
    const QFileInfo fi(path);
    QStringList queue;
    int start = 0;
    if (fi.suffix().toLower() == QStringLiteral("m4b"))
    {
        // An audiobook is one self-contained file (often with chapters) - don't queue the rest of the folder.
        // (Resume is handled generically for all timed media by playTrack/beginResume.)
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
    setAudioQueue(queue, start);
    RecentStore::add({ fi.absoluteFilePath(), fi.completeBaseName(), QStringLiteral("audio"), QString() });
}

void MainWindow::setAudioQueue(const QStringList& files, int startIndex, const QStringList& titles)
{
    tracks_ = files;
    playlist_->clear();
    for (int i = 0; i < tracks_.size(); ++i)
        playlist_->addItem(i < titles.size() && !titles[i].isEmpty()
                               ? titles[i] : QFileInfo(tracks_[i]).completeBaseName());
    playlist_->setVisible(true);
    stack_->setCurrentWidget(playerPage_);
    playTrack(startIndex);
    revealMediaControls();
}

void MainWindow::playTrack(int index)
{
    if (index < 0 || index >= tracks_.size()) return;
    persistResume();              // save where we were in the outgoing track (if any)
    trackIndex_ = index;
    playlist_->setCurrentRow(index);
    statusBar()->showMessage(tr("Track %1 of %2").arg(index + 1).arg(tracks_.size()), 3000);
    beginResume(tracks_[index]);  // track the new file's position (and resume it if seen before)
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
    finishResume(); // the file played to the end -> drop its resume mark (next open starts fresh)
    stopScrobble(); // Trakt: a finished video scrobbles a stop at ~100% -> marked watched
    // Auto-advance the audio queue when a track finishes (ignored for video / single files).
    if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) { playTrack(trackIndex_ + 1); return; }
    // A TV episode that played to the end -> roll to the next episode, if enabled.
    if (Settings::autoplayNextEpisode()) tryPlayNextEpisode();
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
    showPlayerNotice(tr("Up next — finding the next episode…"), 20000);
    addons_->resolveStreamByImdb(QStringLiteral("series"), nextEp,
        [this, nextEp, nextSeason](const QString& url, const QString& mime) {
        if (!url.isEmpty()) { playResolvedEpisode(nextEp, url, mime); return; }
        // End of season? Try the first episode of the next one before giving up.
        addons_->resolveStreamByImdb(QStringLiteral("series"), nextSeason,
            [this, nextSeason](const QString& url2, const QString& mime2) {
            if (!url2.isEmpty()) playResolvedEpisode(nextSeason, url2, mime2);
            else { if (playerNotice_) playerNotice_->hide(); notify(tr("No next episode found — that looks like the finale."), 6000); }
        });
    });
}

void MainWindow::playResolvedEpisode(const QString& imdbStreamId, const QString& url, const QString& mime)
{
    if (playerNotice_) playerNotice_->hide();
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

void MainWindow::beginResume(const QString& path)
{
    resumePath_ = path;
    double pos = store().value(mediaResumeKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    if (pos <= 0.0) pos = store().value(legacyAudiobookKey(path) + QStringLiteral("pos"), 0.0).toDouble();
    resumeSeek_ = pos;       // applied once the duration is known (see onDuration)
    audioPos_ = 0.0;
    lastSavedPos_ = -100.0;
}

void MainWindow::persistResume()
{
    if (resumePath_.isEmpty() || audioPos_ <= 1.0) return; // nothing meaningful to remember yet
    const QString k = mediaResumeKey(resumePath_);
    store().setValue(k + QStringLiteral("pos"), audioPos_);
    store().setValue(k + QStringLiteral("dur"), duration_); // lets the home screen show a progress bar
    store().setValue(k + QStringLiteral("title"), QFileInfo(resumePath_).completeBaseName());
    store().setValue(k + QStringLiteral("ts"), QDateTime::currentSecsSinceEpoch()); // for cross-device merge-by-recency
    store().sync();
    lastSavedPos_ = audioPos_;
    scheduleProgressSync(); // push this position to the cloud "continue watching" file (debounced)
}

void MainWindow::finishResume()
{
    if (resumePath_.isEmpty()) return;
    store().remove(mediaResumeKey(resumePath_));
    store().remove(legacyAudiobookKey(resumePath_)); // also clear any legacy audiobook bookmark
    store().sync();
    resumePath_.clear();
    lastSavedPos_ = -100.0;
}

void MainWindow::clearAudioQueue()
{
    persistResume();      // save where we left off before leaving this media
    resumePath_.clear();
    resumeSeek_ = 0.0;
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

// A disc dumped as a descriptor + raw tracks (Redump: "Game.cue" + "Game (Track N).bin"; or a GDI dump: a
// ".gdi" + "trackNN.bin/.raw") must be booted via the .cue/.gdi — handing the emulator a raw data track mounts
// nothing and it exits immediately (the Flycast "process exited (code 0)" symptom). If `rom` is such a track,
// return its descriptor; otherwise return `rom` unchanged. Safe for direct images (.iso/.chd/.cdi) and for a
// lone .bin with no descriptor beside it (e.g. an Atari 2600 cart), which are left untouched.
static QString resolveDiscDescriptor(const QString& rom)
{
    const QFileInfo fi(rom);
    static const QSet<QString> trackExts = { QStringLiteral("bin"), QStringLiteral("img"), QStringLiteral("raw") };
    if (!trackExts.contains(fi.suffix().toLower())) return rom;

    const QFileInfoList descs = fi.absoluteDir().entryInfoList(
        { QStringLiteral("*.cue"), QStringLiteral("*.gdi") }, QDir::Files);
    if (descs.isEmpty()) return rom;

    const QString binName = fi.fileName();
    // 1) A descriptor that textually references this exact track file is definitive (handles GDI dumps whose
    //    track names — track03.bin — don't resemble the .gdi's name, and multi-.cue folders).
    for (const QFileInfo& d : descs)
    {
        QFile f(d.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        if (QString::fromUtf8(f.read(1 << 20)).contains(binName, Qt::CaseInsensitive))
            return d.absoluteFilePath();
    }
    // 2) Else match by name: strip a trailing " (Track N)" and compare base names (Redump layout).
    QString base = fi.completeBaseName();
    base.remove(QRegularExpression(QStringLiteral("\\s*\\(Track\\s*\\d+\\)\\s*$"),
                                   QRegularExpression::CaseInsensitiveOption));
    for (const QFileInfo& d : descs)
        if (d.completeBaseName().compare(base, Qt::CaseInsensitive) == 0) return d.absoluteFilePath();

    // No positive evidence this .bin belongs to any descriptor here (it may be a cart, e.g. an Atari 2600
    // .bin sitting next to an unrelated .cue) — leave it as-is rather than redirect to the wrong disc.
    return rom;
}

void MainWindow::openGamePath(const QString& rom, const QString& title, const QString& thumb, const QString& key,
                             const QString& systemHint)
{
    // Archived ROM (.zip / .7z): extract the inner ROM once and open that. Every libretro core and external
    // emulator loads from a path, so this single spot handles archives for all of them. If we know the
    // system (from the hint), narrow the member pick to its extensions; otherwise take the largest file.
    // The hint carries through the re-entry so disambiguation still works.
    if (ArchiveRom::isArchive(rom))
    {
        QStringList wanted;
        if (!systemHint.isEmpty())
        {
            const GameSystem* hs = SystemCatalog::byId(systemHint);
            if (!hs) hs = SystemCatalog::forConsoleName(systemHint);
            if (hs)
                for (const QString& e : hs->extensions)
                    wanted << (QStringLiteral(".") + e);
        }
        QString aerr;
        const QString extracted = ArchiveRom::extractToTemp(rom, wanted, &aerr);
        if (extracted.isEmpty())
        {
            mwLog(QStringLiteral("game: archive extract failed for \"%1\": %2").arg(QFileInfo(rom).fileName(), aerr));
            notify(tr("Couldn't open the archived game: %1").arg(aerr), 7000);
            return;
        }
        mwLog(QStringLiteral("game: extracted \"%1\" from \"%2\"")
                  .arg(QFileInfo(extracted).fileName(), QFileInfo(rom).fileName()));
        openGamePath(extracted, title, thumb, key, systemHint);
        return;
    }

    const QString ext = QFileInfo(rom).suffix().toLower();
    // Prefer the console/platform the game was opened from (when known): it disambiguates extensions shared
    // across systems (PSP .iso vs GameCube .iso, PSP .pbp vs PlayStation .pbp). Fall back to the extension.
    const GameSystem* sys = nullptr;
    if (!systemHint.isEmpty())
    {
        sys = SystemCatalog::byId(systemHint);
        if (!sys) sys = SystemCatalog::forConsoleName(systemHint);
    }
    const bool byHint = sys != nullptr;
    if (!sys) sys = SystemCatalog::forExtension(ext);
    mwLog(QStringLiteral("game: open \"%1\" (.%2)%3 -> system %4")
              .arg(QFileInfo(rom).fileName(), ext,
                   systemHint.isEmpty() ? QString() : QStringLiteral(" hint=\"%1\"%2").arg(systemHint,
                       byHint ? QString() : QStringLiteral("(unmatched)")),
                   sys ? sys->id : QStringLiteral("(none)")));
    if (!sys)
    {
        mwLog(QStringLiteral("game: no system for .%1 — aborting").arg(ext));
        notify(tr("No system is configured for .%1 files.").arg(ext), 6000);
        return;
    }

    // If the user opened a raw disc track (a "(Track N).bin" / GDI track), boot its .cue/.gdi descriptor instead —
    // the emulator can't mount a bare track. No-op for direct images and lone .bin carts. Covers cores + externals.
    const QString launchRom = resolveDiscDescriptor(rom);
    if (launchRom != rom)
        mwLog(QStringLiteral("game: track \"%1\" -> disc descriptor \"%2\"")
                  .arg(QFileInfo(rom).fileName(), QFileInfo(launchRom).fileName()));

    // The Recent entry shows the catalog item's name/cover when we have them; otherwise the descriptor's file
    // name. A remote ROM is cached under a hashed file name, so without the passed title it would show as that hash.
    const QString recentTitle = title.isEmpty() ? QFileInfo(launchRom).completeBaseName() : title;

    // Standalone-emulator systems (GameCube/Wii → Dolphin) launch an external process instead of a core.
    // Not possible on Android (the sandbox can't spawn downloaded desktop executables - see android-port.md).
    if (!sys->externalEmulator.isEmpty())
    {
#if defined(Q_OS_ANDROID)
        statusBar()->showMessage(tr("“%1” needs a standalone emulator, which isn't supported on Android.")
                                     .arg(sys->name), 6000);
#else
        launchExternalGame(sys, launchRom, recentTitle, thumb, key);
#endif
        return;
    }

    QString core = Settings::coreFor(sys->id);
    if (core.isEmpty())
        core = sys->cores.value(0); // catalog default
    mwLog(QStringLiteral("game: core '%1' for system %2 (configured=%3)")
              .arg(core, sys->id, Settings::coreFor(sys->id).isEmpty() ? QStringLiteral("no, default") : QStringLiteral("yes")));

    // No prompt: use the configured core, downloading it from the buildbot if it isn't installed. Progress
    // shows inline in the status bar; failures report there too.
    QString dlErr;
    const QString corePath = CoreManager::ensureCore(core, this, &dlErr, [this, core](int pct) {
        statusBar()->showMessage(tr("Downloading core ‘%1’… %2%").arg(core).arg(pct));
    });
    if (corePath.isEmpty())
    {
        mwLog(QStringLiteral("game: core '%1' unavailable: %2").arg(core, dlErr.isEmpty() ? QStringLiteral("download failed") : dlErr));
        notify(dlErr.isEmpty() ? tr("Couldn't download core ‘%1’.").arg(core) : dlErr, 6000);
        return;
    }
    mwLog(QStringLiteral("game: core ready at %1").arg(QFileInfo(corePath).fileName()));

    // Some systems (3DO, Saturn, PlayStation) need a BIOS in the libretro system folder. Fetch any that
    // are missing before the core loads — best-effort, so a failure just falls back to the core's own
    // "BIOS not found" message rather than blocking the launch.
    CoreManager::ensureBios(sys->id, CoreManager::systemDir(),
                            [this](const QString& s) { statusBar()->showMessage(s); });

    // Split screen: run the ROM in the focused pane's own emulator instead of the full-screen one.
    if (splitTarget_)
    {
        mwLog(QStringLiteral("game: launching in split pane"));
        splitTarget_->openGame(corePath, launchRom, core);
        RecentStore::add({ launchRom, recentTitle, QStringLiteral("game"), thumb, key, sys->id });
        PlayStats::markPlayed(PlayStats::identity(key, launchRom)); // split panes aren't session-timed; stamp last-played
        finishSplitOpen();
        return;
    }

    player_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();
    QString err;
    if (retro_->openGame(corePath, launchRom, core, &err))
    {
        mwLog(QStringLiteral("game: running \"%1\"").arg(recentTitle));
        stack_->setCurrentWidget(retro_);
        RecentStore::add({ launchRom, recentTitle, QStringLiteral("game"), thumb, key, sys->id });
        beginPlaySession(PlayStats::identity(key, launchRom));
    }
    else
    {
        mwLog(QStringLiteral("game: openGame failed: %1").arg(err));
        notify(tr("Can't run game: %1").arg(err), 7000);
    }
}

// ---- External (standalone) emulators: the RetroBat / ES-DE launch-and-monitor model -----------------

void MainWindow::ensureEmu()
{
    if (emu_) return;
    emu_ = new EmulatorManager(this);

    connect(emu_, &EmulatorManager::status, this, [this](const QString& t, int pct) {
        const QString line = pct >= 0 ? tr("%1  %2%").arg(t).arg(pct) : t;
        statusBar()->showMessage(line);
        if (emuPage_ && stack_->currentWidget() == emuPage_)
        {
            emuLabel_->setText(line);
            emuStopBtn_->setVisible(false);
        }
    });
    connect(emu_, &EmulatorManager::launched, this, [this](const QString& name) {
        ensureEmuPage();
        emuLabel_->setText(tr("Playing in %1.\n\nClose the %1 window — or press Start+Select on your controller, "
                              "or Esc — to return to My Media Vault.").arg(name));
        emuStopBtn_->setVisible(true);
        if (!pendingEmuRom_.isEmpty()) // record now that it actually started
        {
            RecentStore::add({ pendingEmuRom_, pendingEmuTitle_, QStringLiteral("game"),
                               pendingEmuThumb_, pendingEmuKey_, pendingEmuSystem_ });
            beginPlaySession(PlayStats::identity(pendingEmuKey_, pendingEmuRom_));
        }
        // Step aside so the emulator is unobstructed and in front; we restore when it exits. (Our window is
        // often full screen and would otherwise sit on top of the freshly-launched emulator.)
        emuReturnState_ = windowState();
        showMinimized();
        emuDisplayName_ = name;
        emuUserClosing_ = false;
        emuRunClock_.start();  // to spot a boot that fails and exits instantly (missing BIOS/firmware)
        startEmuHotkeyWatch(); // Start+Select / Esc closes the standalone emulator back to MMV
    });
    connect(emu_, &EmulatorManager::finished, this, [this](int code) {
        mwLog(QStringLiteral("emu: process exited (code %1)").arg(code));
        stopEmuHotkeyWatch();
        endPlaySession(); // bank the external emulator's play time

        if (isMinimized()) // come back to where we were before handing off to the emulator
        {
            if (emuReturnState_ & Qt::WindowFullScreen)    showFullScreen();
            else if (emuReturnState_ & Qt::WindowMaximized) showMaximized();
            else                                            showNormal();
            raise();
            activateWindow();
        }
        if (stack_->currentWidget() == emuPage_) openHome();

        // Closed within a couple of seconds, and we didn't ask it to? That's a failed boot, not a play session —
        // most often a missing console BIOS/firmware, which -batch-style launches quit on silently. Tell the user
        // what happened instead of just bouncing back to the home screen with no explanation.
        if (!emuUserClosing_ && emuRunClock_.isValid() && emuRunClock_.elapsed() < 4000)
        {
            const QString emuName = emuDisplayName_.isEmpty() ? tr("The emulator") : emuDisplayName_;
            const bool needsBios = !pendingEmuSystem_.isEmpty()
                                   && !BiosCatalog::forSystem(pendingEmuSystem_).isEmpty();
            const GameSystem* sys = pendingEmuSystem_.isEmpty() ? nullptr : SystemCatalog::byId(pendingEmuSystem_);
            const QString sysName = sys ? sys->name : tr("This system");
            if (needsBios)
                notify(tr("%1 closed immediately. %2 games need a console BIOS to boot. I try to fetch it "
                          "automatically — if it still won’t start, the BIOS couldn’t be downloaded and you’ll "
                          "need to place it in the emulator’s “bios” folder yourself.").arg(emuName, sysName), 12000);
            else
                notify(tr("%1 closed immediately — the game may be missing files it needs to boot, or the "
                          "emulator needs firmware set up.").arg(emuName), 9000);
        }
        emuRunClock_.invalidate();
    });
    connect(emu_, &EmulatorManager::installed, this, [this](const QString& name) {
        statusBar()->showMessage(tr("%1 is installed.").arg(name), 5000);
    });
    connect(emu_, &EmulatorManager::failed, this, [this](const QString& msg) {
        mwLog(QStringLiteral("emu: failed: %1").arg(msg));
        stopEmuHotkeyWatch();
        statusBar()->showMessage(msg, 9000);
        if (stack_->currentWidget() == emuPage_) openHome();
    });
}

// ---- Standalone-emulator exit hotkey: close melonDS/Dolphin/etc. back to MMV on Start+Select or Esc ---------
// A libretro core shows MMV's own pause menu on Start+Select; a standalone emulator is a separate process we
// can't inject a menu into, so the RetroBat-equivalent is to close it and come back. We poll while MMV is
// minimized (Qt gets no input then): the pad works because SDL keeps device state live in the background
// (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS), and Esc is read from the global key state on Windows.
void MainWindow::startEmuHotkeyWatch()
{
    if (!emuHotkeyTimer_)
    {
        emuHotkeyTimer_ = new QTimer(this);
        emuHotkeyTimer_->setInterval(60);
        connect(emuHotkeyTimer_, &QTimer::timeout, this, &MainWindow::pollEmuExitHotkey);
    }
    // Prime the edge-detectors as "held" so a combo/Esc still down from the moment of launch doesn't instantly
    // close the emulator — we only act on a fresh press.
    emuComboPrev_ = true;
    emuEscPrev_ = true;
    emuHotkeyTimer_->start();
}

void MainWindow::stopEmuHotkeyWatch()
{
    if (emuHotkeyTimer_) emuHotkeyTimer_->stop();
}

void MainWindow::pollEmuExitHotkey()
{
    if (!emu_ || !emu_->busy()) { stopEmuHotkeyWatch(); return; }
    bool exitNow = false;

    // Controller: Start+Select on any connected pad. Reuse RetroView's Gamepad (idle while a standalone emulator
    // runs, so borrowing it to poll is free and avoids opening the device twice).
    if (retro_ && retro_->gamepad() && retro_->gamepad()->available())
    {
        Gamepad* pad = retro_->gamepad();
        pad->poll();
        bool combo = false;
        for (unsigned p = 0; p < Gamepad::kMaxPlayers && !combo; ++p)
            combo = pad->button(p, RETRO_DEVICE_ID_JOYPAD_START) && pad->button(p, RETRO_DEVICE_ID_JOYPAD_SELECT);
        if (combo && !emuComboPrev_) exitNow = true;
        emuComboPrev_ = combo;
    }

#if defined(Q_OS_WIN)
    // Keyboard: Qt can't see Esc while the emulator owns focus, so read the global key state.
    const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (esc && !emuEscPrev_) exitNow = true;
    emuEscPrev_ = esc;
#endif

    if (exitNow)
    {
        mwLog(QStringLiteral("emu: exit hotkey (Start+Select / Esc) — closing the standalone emulator"));
        stopEmuHotkeyWatch();   // one shot: don't fire again while it's tearing down
        emuUserClosing_ = true; // a deliberate close — don't mistake it for a failed boot
        emu_->closeGame();
    }
}

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
    connect(emuStopBtn_, &QPushButton::clicked, this, [this] { emuUserClosing_ = true; if (emu_) emu_->terminateGame(); });
    v->addWidget(emuStopBtn_, 0, Qt::AlignHCenter);
    v->addStretch(1);
    stack_->addWidget(emuPage_);
}

void MainWindow::launchExternalGame(const GameSystem* sys, const QString& rom, const QString& title,
                                    const QString& thumb, const QString& key)
{
    const ExternalEmulator* em = EmulatorRegistry::byId(sys->externalEmulator);
    if (!em)
    {
        mwLog(QStringLiteral("game: external emulator '%1' not registered").arg(sys->externalEmulator));
        statusBar()->showMessage(tr("No emulator is configured for %1.").arg(sys->name), 6000);
        return;
    }
    if (splitTarget_) // a standalone emulator owns its own window; it can't embed in a split pane
    {
        statusBar()->showMessage(tr("%1 opens in its own window, not a split pane.").arg(em->displayName), 5000);
        finishSplitOpen();
    }
    runEmulator(*em, rom, title, thumb, key, sys->id);
}

void MainWindow::runEmulator(const ExternalEmulator& em, const QString& rom, const QString& title,
                             const QString& thumb, const QString& key, const QString& system)
{
    ensureEmu();
    if (emu_->busy())
    {
        statusBar()->showMessage(tr("An emulator is already running."), 4000);
        return;
    }
    // Hand the screen + audio to the external emulator: stop our own playback first.
    player_->stop();
    retro_->stop();
    book_->persist(); pdf_->persist(); comic_->persist();
    clearAudioQueue();

    pendingEmuRom_ = rom; pendingEmuTitle_ = title; pendingEmuThumb_ = thumb; pendingEmuKey_ = key; pendingEmuSystem_ = system;
    // Tell the emulator's SDL to ignore any phantom controller (e.g. a Keychron HE keyboard that presents a
    // gamepad interface). Otherwise it can take the first device slot and the emulator, bound to "SDL-0", listens
    // to the keyboard instead of the real pad. The child QProcess inherits these; we set both the SDL2 and SDL3
    // hint names since standalone emulators use either.
    if (retro_ && retro_->gamepad())
    {
        const std::string ignore = retro_->gamepad()->phantomControllerIgnoreList();
        if (!ignore.empty())
        {
            const QByteArray v = QByteArray::fromStdString(ignore);
            qputenv("SDL_JOYSTICK_IGNORE_DEVICES", v);       // SDL3 (DuckStation, current PCSX2…)
            qputenv("SDL_GAMECONTROLLER_IGNORE_DEVICES", v); // SDL2-based emulators
            mwLog(QStringLiteral("emu: ignoring phantom controller(s) for the emulator: %1").arg(QString::fromUtf8(v)));
        }
        else { qunsetenv("SDL_JOYSTICK_IGNORE_DEVICES"); qunsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES"); }
    }
    ensureEmuPage();
    emuLabel_->setText(EmulatorManager::isInstalled(em)
                           ? tr("Starting %1…").arg(em.displayName)
                           : tr("%1 isn't installed yet — downloading it…").arg(em.displayName));
    emuStopBtn_->setVisible(false);
    stack_->setCurrentWidget(emuPage_);
    mwLog(QStringLiteral("emu: run %1 \"%2\"")
              .arg(em.displayName, rom.isEmpty() ? QStringLiteral("(no game)") : QFileInfo(rom).fileName()));
    emu_->play(em, rom);
}

void MainWindow::openEmulatorManager()
{
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
                ensureEmu();
                if (emu_->busy()) { statusBar()->showMessage(tr("An emulator operation is already running."), 4000); return; }
                statusBar()->showMessage(tr("Downloading %1…").arg(emCopy.displayName));
                emu_->install(emCopy);
            });
            btnRow->addWidget(dl, 1);
            // Launch the emulator with no game - opens its own UI. Primary use for launcher-style emulators
            // (TeknoParrot); for the others it's handy for first-run setup (BIOS/firmware/keys).
            auto* launchBtn = new QPushButton(tr("Launch"));
            connect(launchBtn, &QPushButton::clicked, this, [this, emCopy] { runEmulator(emCopy); });
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
    // Playlists need fetching + dispatch (HLS stream vs. channel list vs. disc set); everything else
    // is a single link libmpv can play straight away. openM3u() routes back here for the HLS case.
    if (isM3uRef(url)) { openM3u(url, title); return; }
    playStream(url, resumeKey, title);
}

void MainWindow::playStream(const QString& url, const QString& resumeKey, const QString& title)
{
    subCtx_ = {};                      // a pasted/Recent link has no catalog metadata to match a subtitle by
    stopScrobble();                    // leaving whatever was playing
    castUrl_ = url; castTitle_ = title; castMime_.clear(); // a pasted/Recent link is castable as-is
    currentNextSourceCapable_ = false; // a pasted/Recent stream link isn't a swappable Allarr source
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();      // saves+clears any previous timed media
    // Resume + Recent keyed by the stable id when given (a re-opened catalog stream), else by the link itself.
    const QString rkey = resumeKey.isEmpty() ? url : resumeKey;
    beginResume(rkey);
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

// Read an .m3u/.m3u8 (local file or remote URL), then hand its text to handleM3u() for dispatch.
void MainWindow::openM3u(const QString& src, const QString& title)
{
    if (!src.contains(QStringLiteral("://")))
    {
        QFile f(src);
        QString text;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) text = QString::fromUtf8(f.readAll());
        handleM3u(src, text, title.isEmpty() ? QFileInfo(src).completeBaseName() : title);
        return;
    }
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    statusBar()->showMessage(tr("Loading playlist…"));
    mwLog(QStringLiteral("m3u: GET %1").arg(logSafeUrl(src)));
    QNetworkRequest rq{ QUrl(src) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = docNam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, src, title] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            // Couldn't fetch the manifest text (auth, headers, live-only) - let libmpv try the URL itself.
            mwLog(QStringLiteral("m3u: fetch failed (%1) -> player").arg(reply->errorString()));
            playStream(src, QString(), title);
            return;
        }
        handleM3u(src, QString::fromUtf8(reply->readAll()), title);
    });
}

void MainWindow::handleM3u(const QString& src, const QString& text, const QString& title)
{
    if (isHlsManifest(text))                       // a single adaptive stream: libmpv handles the segments
    {
        mwLog(QStringLiteral("m3u: HLS manifest -> player"));
        playStream(src, QString(), title);
        return;
    }
    const QVector<M3uEntry> entries = parseM3u(text, src);
    if (entries.isEmpty())                         // not a recognisable list - best effort: play the URL
    {
        mwLog(QStringLiteral("m3u: no entries -> player"));
        playStream(src, QString(), title);
        return;
    }
    if (looksLikeDiscPlaylist(entries))            // PlayStation multi-disc: the emulator swaps discs itself
    {
        mwLog(QStringLiteral("m3u: %1-disc playlist -> emulator").arg(entries.size()));
        openGamePath(src, title);
        return;
    }
    // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
    mwLog(QStringLiteral("m3u: %1 entries -> queue").arg(entries.size()));
    QStringList urls, titles;
    for (const M3uEntry& e : entries) { urls << e.url; titles << e.title; }
    currentNextSourceCapable_ = false;
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    setAudioQueue(urls, 0, titles);
    RecentStore::add({ src, title.isEmpty() ? QFileInfo(src).completeBaseName() : title,
                       QStringLiteral("video"), QString(), src });
}

void MainWindow::openAudioStream(const QString& url, const QString& resumeKey, const QString& title,
                                 const QString& thumbnailUrl)
{
    if (splitTarget_) { splitTarget_->openVideo(url, title); finishSplitOpen(); return; }
    subCtx_ = {};           // audio has no subtitles to fetch
    stopScrobble();         // leaving whatever video was playing
    retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
    clearAudioQueue();      // saves+clears any previous timed media, then we build a one-track queue
    const QString t = !title.isEmpty() ? title : QUrl(url).fileName();
    tracks_ = QStringList{ url };
    trackIndex_ = 0;
    playlist_->clear();
    playlist_->addItem(t);
    playlist_->setCurrentRow(0);
    playlist_->setVisible(true); // the now-playing list (vs. the bare video surface) marks this as audio
    stack_->setCurrentWidget(playerPage_);
    const QString rkey = resumeKey.isEmpty() ? url : resumeKey;
    beginResume(rkey);      // a long audiobook must resume where you left off, keyed by the stable id
    player_->play(url);
    revealMediaControls();
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

void MainWindow::openDocumentPath(const QString& f)
{
    const QString ext = QFileInfo(f).suffix().toLower();
    QString err;

    if (splitTarget_)
    {
        if (ext == QStringLiteral("pdf")) splitTarget_->openPdf(f);
        else if (ext == QStringLiteral("cbz")) splitTarget_->openComic(f);
        else splitTarget_->openBook(f); // .epub
        finishSplitOpen();
        return;
    }

    if (ext == QStringLiteral("pdf"))
    {
        if (!pdf_->openPdf(f, &err)) { notify(tr("Can't open PDF: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(pdf_);
    }
    else if (ext == QStringLiteral("cbz"))
    {
        if (!comic_->openComic(f, &err)) { notify(tr("Can't open comic: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(comic_);
    }
    else // treat everything else as an EPUB (the reader validates and reports if it isn't one)
    {
        if (!book_->openBook(f, &err)) { notify(tr("Can't open book: %1").arg(err), 6000); return; }
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
    hideSubtitleMenu(); // dismiss the subtitle overlay if it was up
    stopScrobble();     // Trakt: close out the current watch
    player_->stop();
    retro_->stop();
    book_->persist();
    pdf_->persist();
    comic_->persist();
    clearAudioQueue();
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
    // Rebuild a fresh themed view on return so it reflects the current theme/catalogs. The themed pages are
    // QQuickWidgets (plain widgets, no native child window), so this is safe: no compositing tricks needed.
    if (themedHomeEnabled()) { showThemedHome(); return; }
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
    return store().value(QStringLiteral("themedHome/enabled"), false).toBool();
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
    const QString q = Osk::getText(label, QString(), QLineEdit::Normal, this);
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
    auto onBack  = [this] { showEscMenu(); };
    themedOnBack_ = onBack; // the base window's goBack() shares this themed back
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
                                        {}, {}, {}, {}, {}, onButton);
    // Re-highlight the system we last opened (so returning from a catalog lands back on it, not the top).
    if (QQuickItem* r = ThemeEngine::rootItem(w))
        r->setProperty("currentIndex", qBound(0, themedHomeIndex_, int(items.size()) - 1));
    QWidget* old = themedHome_;
    themedHome_ = w;
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
    if (themedMetaTimer_) themedMetaTimer_->start();
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
                home_->requestThemedMeta(themedMetaWant_);
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
        if (themedXmbCatalogs_.size() == 1) // single catalog -> open straight into its contents
        {
            themedXmbInCatalog_ = true;
            themedXmbAutoOpened_ = true;
            if (r) { r->setProperty("items", QVariantList()); r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // clear + spinner while it loads
            home_->activateNav(themedXmbCatalogs_[0].toMap().value(QStringLiteral("navKey")).toString());
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
            const QString navKey = themedXmbCatalogs_[itemIdx].toMap().value(QStringLiteral("navKey")).toString();
            if (navKey.isEmpty()) return;
            themedXmbCatalogIndex_ = itemIdx;       // remember the catalog, so Back re-selects it in the list
            themedXmbInCatalog_ = true;
            if (r) { r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // spinner while it loads
            home_->activateNav(navKey); // its items land via browseItemsChanged (which now targets this column)
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
            if (expandable || synthetic) home_->browseActivate(itemIdx);
            else if (r)
            {
                r->setProperty("actionItem", itemIdx);
                r->setProperty("actionFav", home_->isThemedLeafFavorite(itemIdx));
                r->setProperty("actionIndex", 0);
                r->setProperty("actionsOpen", true);
            }
        }
    };
    auto onBack = [this, showCatalogs] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int cat = r ? r->property("catIndex").toInt() : 0;
        if (themedXmbInCatalog_)
        {
            if (home_->browseBack()) return;          // popped a deeper level; browseItemsChanged refreshes the column
            if (!themedXmbAutoOpened_)                // multi-catalog bucket: back out, re-select the catalog we opened
            { showCatalogs(cat, themedXmbCatalogIndex_); return; }
            // single-catalog bucket: its contents ARE the root -> fall through to the app menu
        }
        showEscMenu(); // at the top of the themed home: bring up the app pause menu (Resume / Exit)
    };
    themedOnBack_ = onBack;
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
            if (!q.isNull()) home_->searchInBrowse(q);
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

    QWidget* w = ThemeEngine::buildView(themeDir, QVariantList(), system, this,
                                        onActivated, onBack, onCycle, onSearch, onNearEnd, onCategory,
                                        onSelect, onAction, onPlaylistAdd);
    if (QQuickItem* r = ThemeEngine::rootItem(w))
    {
        r->setProperty("categories", cats);
        r->setProperty("catIndex", startCat);
    }
    QWidget* old = themedHome_;
    themedHome_ = w;
    updateThemedNowPlaying(); // seed the Triple theme's now-playing readout
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    w->setFocus();
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
    nudgeThemedHome(); // repaint the rebuilt themed home

    showCatalogs(startCat, themedXmbCatalogIndex_); // populate the starting bucket's catalog list (restore on rebuild)

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
    auto onActivated = [this](int idx) { home_->browseActivate(idx); }; // opens media, or drills (-> refresh)
    auto onBack = [this] { if (!home_->browseBack()) showThemedHome(); }; // up a level, or back to the system view
    themedOnBack_ = onBack;
    auto onCycle = [this, themes, themeName] {
        if (themes.isEmpty()) return;
        const QString next = themes[(qMax(0, int(themes.indexOf(themeName))) + 1) % themes.size()];
        store().setValue(QStringLiteral("themedHome/theme"), next); store().sync();
        showThemedBrowse();
    };
    // "/" searches within the current catalog/console; the result refreshes via browseItemsChanged.
    auto onSearch = [this] {
        const QString q = promptThemedSearch(home_->browseTitle());
        if (!q.isNull()) home_->searchInBrowse(q);
    };
    // Selection neared the end -> pull the next page (if any). browseItemsChanged appends + keeps selection.
    auto onNearEnd = [this] { if (home_->browseHasMore()) home_->browseLoadMore(); };

    QWidget* w = ThemeEngine::buildView(ThemeEngine::themesRoot() + QStringLiteral("/") + themeName,
                                        home_->browseItems(), system, this, onActivated, onBack, onCycle,
                                        onSearch, onNearEnd);
    if (QQuickItem* r = ThemeEngine::rootItem(w)) r->setProperty("currentView", QStringLiteral("browse"));
    QWidget* old = themedBrowse_;
    themedBrowse_ = w;
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    w->setFocus();
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
}

// The home theme picker, as a full-screen panel page in the main window (like the other settings screens).
// Changes save as you make them and preview live; backing out (-> the settings hub -> home) applies them.
void MainWindow::openAppearance()
{
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
            stack_->setCurrentWidget(home_);     // pick it from Home; the other pane keeps playing in the background
            home_->focusContent();
        });
    }
    // Park the playing views (don't leave a movie playing behind the split) and show the empty split.
    // The window state is left exactly as it is — no screen ever changes it on the user's behalf.
    player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); clearAudioQueue();
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
    // A PC game re-opens through its remembered install (exe from PcGameStore) - even when the exact path
    // this Recent entry recorded (e.g. its one-time installer) is stale or gone.
    if (kind == QStringLiteral("pcgame")) { relaunchPcGame(resumeKey, title, thumb, path); return; }
    // A streamed link has no local file to check; route it straight to libmpv.
    const bool isUrl = path.contains(QStringLiteral("://"));
    if (!isUrl && !QFileInfo::exists(path))
    {
        statusBar()->showMessage(tr("That file can no longer be found: %1").arg(path), 5000);
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
    notify(tr("Incorrect PIN."), 3000);
    return false;
}

void MainWindow::onSwitchProfile()
{
    if (!parentalUnlock(tr("Enter the parental PIN to switch profiles."))) return;
    auto* dlg = new ProfileDialog(/*mustChoose*/ false, this);
    showDialogPanel(tr("Profiles"), dlg, [this, dlg](int result) {
        if (result == QDialog::Accepted && !dlg->selectedId().isEmpty())
            ProfileStore::setCurrent(dlg->selectedId());
        // This entry point lives on Home; openHome() switches back and refreshes for the active profile
        // (covers a switched user, a deletion repointing "current", or an edited name/icon).
        openHome();
    }, [this] { openHome(); });
}

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
        notify(tr("Couldn't start downloading “%1”.").arg(item.title), 6000);
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
            statusBar()->showMessage(e, 8000);
            notify(e, 8000);
            part->remove();
            return;
        }

        const QString name = fileName->isEmpty() ? safeFileName(item.title) : safeFileName(*fileName);
        const QString dest = dir + QStringLiteral("/") + name;
        QFile::remove(dest);
        if (!QFile::rename(partPath, dest))
        {
            statusBar()->showMessage(tr("Couldn't save “%1”.").arg(item.title), 6000);
            notify(tr("Couldn't save “%1”.").arg(item.title), 6000);
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
                statusBar()->showMessage(e, 8000);
                notify(e, 8000);
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

// Run a resolved PC game exe and record it in Recent as a "pcgame" so re-opening from Home relaunches this
// exact executable (not the installer).
// Start timing a game session: close any session still open, stamp last-played, and note the start time.
void MainWindow::beginPlaySession(const QString& identity)
{
    endPlaySession();
    if (identity.isEmpty()) return;
    PlayStats::markPlayed(identity);
    activePlayId_ = identity;
    activePlayStart_ = QDateTime::currentSecsSinceEpoch();
}

// End the active session (if any) and bank its elapsed time into the game's total.
void MainWindow::endPlaySession()
{
    if (activePlayId_.isEmpty()) return;
    const qint64 secs = QDateTime::currentSecsSinceEpoch() - activePlayStart_;
    PlayStats::addSession(activePlayId_, secs);
    activePlayId_.clear();
    activePlayStart_ = 0;
}

void MainWindow::launchPcExe(const QString& exe, const QString& id, const QString& title, const QString& thumb)
{
    mwLog(QStringLiteral("pcgame: launch \"%1\"").arg(QFileInfo(exe).fileName()));
    notify(tr("Launching “%1”…").arg(title), 5000);
    RecentStore::add({ exe, title, QStringLiteral("pcgame"), thumb, id });
    DownloadsStore::add({ exe, title, QStringLiteral("pcgame"), thumb, id, QStringLiteral("pc") });
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
    if (!QProcess::startDetached(exe, QStringList(), workDir))
    {
        mwLog(QStringLiteral("pcgame: startDetached failed for \"%1\" — falling back to shell open").arg(QFileInfo(exe).fileName()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(exe));
    }
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
        notify(tr("Cancelled — “%1” was removed. Open it again to download and install it.").arg(title), 7000);
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
    notify(tr("Couldn't find “%1”. Open it from the library to download it again.").arg(title), 7000);
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
    const double pct = duration_ > 0.0 ? qBound(0.0, audioPos_ / duration_ * 100.0, 100.0) : 0.0;
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
    rightCol->addWidget(sectionLabel(tr("TIMING & SIZE")));
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
    addAdjustRow(tr("Sync"),
                 [this] { return tr("%1 s").arg(player_->subtitleDelay(), 0, 'f', 1); },
                 [this] { player_->setSubtitleDelay(player_->subtitleDelay() - 0.1); },
                 [this] { player_->setSubtitleDelay(player_->subtitleDelay() + 0.1); });
    addAdjustRow(tr("Size"),
                 [this] { return QStringLiteral("%1%").arg(qRound(player_->subtitleScale() * 100)); },
                 [this] { player_->setSubtitleScale(qMax(0.2, player_->subtitleScale() - 0.1)); },
                 [this] { player_->setSubtitleScale(qMin(4.0, player_->subtitleScale() + 0.1)); });

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
                else notify(tr("No matching subtitle found on OpenSubtitles."), 5000);
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
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(item.title), 4000);
        return;
    }
    // A Steam game: hand it to the Steam client to launch (it handles install/run).
    if (item.url.startsWith(QStringLiteral("steam://")))
    {
        QDesktopServices::openUrl(QUrl(item.url));
        statusBar()->showMessage(tr("Launching “%1” via Steam…").arg(item.title), 5000);
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
    // also the PlayStation multi-disc extension, so it would otherwise be fetched as a game. openM3u()
    // re-checks the contents and still routes a genuine disc list to the emulator.
    if (isM3uRef(lower))
    {
        if (splitTarget_) { splitTarget_->openVideo(url, item.title); finishSplitOpen(); return; }
        openM3u(url, item.title);
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
        if (!book_->openBook(url, &err)) { notify(tr("Can't open book: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); clearAudioQueue();
        book_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
        stack_->setCurrentWidget(book_);
        recordDocument();
    }
    else if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
    {
        // Prefer the reflowable reader (font sizing / pagination like EPUB) for text PDFs - this is mainly a
        // book app. Fall back to the fixed page-image view for scanned PDFs that have no text layer.
        if (book_->openBook(url, &err))
        {
            player_->stop(); retro_->stop(); pdf_->persist(); comic_->persist(); clearAudioQueue();
            book_->setStreamIssueVisible(currentNextSourceCapable_);
            stack_->setCurrentWidget(book_);
            recordDocument();
        }
        else if (pdf_->openPdf(url, &err))
        {
            player_->stop(); retro_->stop(); book_->persist(); comic_->persist(); clearAudioQueue();
            pdf_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
            stack_->setCurrentWidget(pdf_);
            recordDocument();
        }
        else { notify(tr("Can't open PDF: %1").arg(err), 6000); }
    }
    else if (lower.endsWith(QStringLiteral(".cbz"))) // a downloaded/associated comic archive
    {
        if (!comic_->openComic(url, &err)) { notify(tr("Can't open comic: %1").arg(err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(comic_);
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
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
        setAudioQueue({ url }, 0); // a single-track queue; libmpv also streams http(s) audio
    }
    else if (type == QStringLiteral("game") || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr)
    {
        // Carry the catalog title/cover/id into Recent (the ROM file itself is a hashed cache name), and the
        // console/platform hint so the right emulator is picked even when the extension is shared.
        openGamePath(url, item.title, item.thumbnailUrl, item.id, item.systemHint);
    }
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); clearAudioQueue();
        // Resume + Recent are keyed by the item's stable id when it has one (a debrid/stream URL changes every
        // time it's resolved, so keying on the URL would lose your place and duplicate the Recent entry).
        const QString rkey = item.id.isEmpty() ? url : item.id;
        beginResume(rkey);
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
                statusBar()->showMessage(e, 6000); notify(e, 6000);
                return;
            }
            if (QFileInfo(partPath).size() == 0)
            {
                QFile::remove(partPath);
                mwLog(QStringLiteral("download(curl): empty (0 bytes) for \"%1\"").arg(title));
                notify(tr("Couldn't get “%1” — the source returned no data (there may be no copy).").arg(title), 8000);
                return;
            }
            QFile::remove(localPath);
            if (!QFile::rename(partPath, localPath))
            {
                mwLog(QStringLiteral("download(curl): finalise (rename) failed for \"%1\"").arg(title));
                notify(tr("Couldn't finalise the download for “%1”.").arg(title), 6000);
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
            statusBar()->showMessage(msg, 6000); notify(msg, 6000);
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
        statusBar()->showMessage(e, 6000); notify(e, 6000);
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
            statusBar()->showMessage(e, 6000);
            notify(e, 6000);
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
            statusBar()->showMessage(e, 8000);
            notify(e, 8000);
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
            statusBar()->showMessage(e, 8000);
            notify(e, 8000);
            return;
        }
        if (!writeOk)
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: save failed for \"%1\"").arg(title));
            statusBar()->showMessage(tr("Couldn't save “%1” to cache.").arg(title), 6000);
            notify(tr("Couldn't save “%1” to cache.").arg(title), 6000);
            return;
        }
        if (QFileInfo(partPath).size() == 0) // the source returned nothing (no copy / a dead link) - opening it would just fail
        {
            QFile::remove(partPath);
            mwLog(QStringLiteral("download: empty (0 bytes) for \"%1\"").arg(title));
            notify(tr("Couldn't get “%1” — the source returned no data (there may be no copy).").arg(title), 8000);
            return;
        }
        QFile::remove(localPath);
        if (!QFile::rename(partPath, localPath))
        {
            mwLog(QStringLiteral("download: finalise (rename) failed for \"%1\"").arg(title));
            statusBar()->showMessage(tr("Couldn't finalise the download for “%1”.").arg(title), 6000);
            notify(tr("Couldn't finalise the download for “%1”.").arg(title), 6000);
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
    if (pageUrls.isEmpty()) { statusBar()->showMessage(tr("No pages to read for “%1”.").arg(title), 5000); return; }

    // Cache the assembled chapter as a CBZ keyed by the chapter id, so re-opening it is instant and the
    // comic reader's path-based resume remembers your page.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/manga");
    QDir().mkpath(dir);
    const QString hash = QString::fromUtf8(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString cbzPath = dir + QStringLiteral("/") + hash + QStringLiteral(".cbz");

    auto openCbz = [this, cbzPath, title] {
        QString err;
        if (!comic_->openComic(cbzPath, &err))
        { mwLog(QStringLiteral("openImagePages: openComic failed: %1").arg(err)); notify(tr("Can't open “%1”: %2").arg(title, err), 6000); return; }
        player_->stop(); retro_->stop(); book_->persist(); pdf_->persist(); clearAudioQueue();
        stack_->setCurrentWidget(comic_);
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
            { notify(tr("Couldn't assemble “%1”.").arg(title), 6000); return; }

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
            { QFile::remove(partPath); notify(tr("Couldn't download any pages for “%1”.").arg(title), 6000); return; }
            QFile::remove(cbzPath);
            if (!QFile::rename(partPath, cbzPath))
            { mwLog(QStringLiteral("openImagePages: rename to cbz failed")); notify(tr("Couldn't save “%1”.").arg(title), 6000); return; }
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
    const bool menu = (w == home_ || w == themedHome_ || w == themedBrowse_ || w == panelPage_ || w == library_);
    bgm_->setActive(menu);
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

void MainWindow::openSettingsHub()
{
    if (!parentalUnlock(tr("Enter the parental PIN to open Settings."))) return;
    // Entering the settings area from a real page: remember it so the top-level Back returns there.
    if (stack_->currentWidget() != panelPage_) panelReturnTo_ = stack_->currentWidget();
    showPanel(tr("Settings"), [this](QVBoxLayout* v) {
        auto add = [this, v](const QString& label, std::function<void()> fn) {
            auto* b = panelRow(label);
            connect(b, &QPushButton::clicked, this, fn);
            v->addWidget(b);
        };
        add(tr("General"),            [this] { openGeneralSettings(); });
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
    { notify(tr("Couldn't start the uninstaller."), 6000); return; }
    cf.write(del.toLocal8Bit());
    cf.close();

    // Detached + minimized so it outlives us; then quit hard (skip the cloud-push-on-exit — everything's going).
    QProcess::startDetached(QStringLiteral("cmd"),
        { QStringLiteral("/c"), QStringLiteral("start"), QString(), QStringLiteral("/min"),
          QStringLiteral("cmd"), QStringLiteral("/c"), QDir::toNativeSeparators(cmdPath) });
    forceClose_ = true;
    qApp->quit();
#else
    // macOS/Linux: the app is a bundle/AppImage the user removes by deleting it. Clear our data dir + cache + quit.
    QDir(QCoreApplication::applicationDirPath()).removeRecursively();
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
    forceClose_ = true;
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

void MainWindow::openGeneralSettings()
{
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
        v->addSpacing(10);

        auto* pbHeading = new QLabel(tr("Playback"));
        pbHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(pbHeading);
        auto* autoNext = new QCheckBox(tr("Auto-play the next episode"));
        autoNext->setStyleSheet(QStringLiteral("font-size:15px;"));
        autoNext->setChecked(Settings::autoplayNextEpisode());
        connect(autoNext, &QCheckBox::toggled, this, [](bool c) { Settings::setAutoplayNextEpisode(c); });
        v->addWidget(autoNext);

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

        // --- PC game achievements (Steam): a Steam web API key shows an installed PC game's Steam achievements. ---
        v->addSpacing(10);
        auto* sHeading = new QLabel(tr("PC Game Achievements (Steam)"));
        sHeading->setStyleSheet(QStringLiteral("font-size:17px;font-weight:bold;"));
        v->addWidget(sHeading);
        auto* sNote = new QLabel(tr("Paste a Steam Web API key (steamcommunity.com/dev/apikey) to show an installed "
            "PC game's Steam achievements in the Triple theme, with the ones you've unlocked highlighted. Unlocks "
            "come from the game's Steam-emulator save."));
        sNote->setWordWrap(true); sNote->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        v->addWidget(sNote);
        auto* sKey = new QLineEdit(store().value(QStringLiteral("steam/apikey")).toString());
        sKey->setMinimumHeight(34); sKey->setEchoMode(QLineEdit::Password);
        sKey->setPlaceholderText(tr("Steam Web API key"));
        v->addWidget(sKey);
        auto* sSave = panelRow(tr("Save Steam Key"));
        connect(sSave, &QPushButton::clicked, this, [this, sKey] {
            store().setValue(QStringLiteral("steam/apikey"), sKey->text().trimmed()); store().sync();
            statusBar()->showMessage(tr("Saved Steam Web API key."), 4000);
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
        statusBar()->showMessage(ok ? tr("Saved to Google Drive.") : m, 5000);
    });
}

// ---- "Continue watching" cross-device sync ---------------------------------------------------------------
// Resume positions ("resume/<hash>/{pos,dur,ts,title}") and the per-profile recent lists ("recent/<id>/items")
// both already live in the ini; this syncs JUST those, in a small file pushed far more often than the heavy
// state bundle, and MERGES on pull (by recency) so two devices' progress combine instead of clobbering.

QByteArray MainWindow::serializeProgress() const
{
    QJsonObject resume, recent;
    for (const QString& key : store().allKeys())
    {
        if (key.startsWith(QStringLiteral("resume/")))
        {
            const QString rest = key.mid(7);       // "<hash>/<field>"
            const int slash = rest.indexOf(QLatin1Char('/'));
            if (slash <= 0) continue;
            const QString hash = rest.left(slash), field = rest.mid(slash + 1);
            QJsonObject e = resume.value(hash).toObject();
            if      (field == QStringLiteral("pos"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("dur"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("ts"))    e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("title")) e.insert(field, store().value(key).toString());
            resume.insert(hash, e);
        }
        else if (key.startsWith(QStringLiteral("recent/"))) // "recent/<profile>/items" -> the list JSON string
        {
            recent.insert(key.mid(7), store().value(key).toString());
        }
    }
    return QJsonDocument(QJsonObject{ { QStringLiteral("resume"), resume }, { QStringLiteral("recent"), recent } })
        .toJson(QJsonDocument::Compact);
}

void MainWindow::mergeProgress(const QByteArray& json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();

    // Resume: for each item, keep whichever position was saved more recently (ts). Never delete a local entry.
    const QJsonObject resume = root.value(QStringLiteral("resume")).toObject();
    for (auto it = resume.begin(); it != resume.end(); ++it)
    {
        const QJsonObject re = it.value().toObject();
        const QString prefix = QStringLiteral("resume/") + it.key() + QLatin1Char('/');
        const double localTs = store().value(prefix + QStringLiteral("ts"), 0.0).toDouble();
        const bool haveLocal = store().contains(prefix + QStringLiteral("pos"));
        if (haveLocal && re.value(QStringLiteral("ts")).toDouble() <= localTs) continue; // local is newer/equal
        if (re.contains(QStringLiteral("pos")))   store().setValue(prefix + QStringLiteral("pos"),   re.value(QStringLiteral("pos")).toDouble());
        if (re.contains(QStringLiteral("dur")))   store().setValue(prefix + QStringLiteral("dur"),   re.value(QStringLiteral("dur")).toDouble());
        if (re.contains(QStringLiteral("ts")))    store().setValue(prefix + QStringLiteral("ts"),    re.value(QStringLiteral("ts")).toDouble());
        if (re.contains(QStringLiteral("title"))) store().setValue(prefix + QStringLiteral("title"), re.value(QStringLiteral("title")).toString());
    }

    // Recent: union the local + remote lists per profile by stable identity (key, else path), keeping the newest
    // ts for each, sorted newest-first and capped.
    const QJsonObject recent = root.value(QStringLiteral("recent")).toObject();
    for (auto it = recent.begin(); it != recent.end(); ++it)
    {
        const QString localKey = QStringLiteral("recent/") + it.key();
        QHash<QString, QJsonObject> byId;
        auto ingest = [&byId](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("key")).toString().isEmpty()
                                       ? o.value(QStringLiteral("path")).toString()
                                       : o.value(QStringLiteral("key")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id) || o.value(QStringLiteral("ts")).toDouble() >= byId[id].value(QStringLiteral("ts")).toDouble())
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(localKey).toString().toUtf8()).array()); // local first
        ingest(QJsonDocument::fromJson(it.value().toString().toUtf8()).array());              // then remote
        QList<QJsonObject> merged = byId.values();
        std::sort(merged.begin(), merged.end(), [](const QJsonObject& a, const QJsonObject& b) {
            return a.value(QStringLiteral("ts")).toDouble() > b.value(QStringLiteral("ts")).toDouble();
        });
        QJsonArray out;
        for (int i = 0; i < merged.size() && i < 40; ++i) out.append(merged[i]); // cap matches RecentStore's
        store().setValue(localKey, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    }
    store().sync();
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
    progressSyncTimer_->start(15000); // debounce: push ~15s after the last position change (i.e. shortly after you stop)
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
    persistResume(); // flush the current media's playback position before anything else on exit
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
    // Resume where we left off, now that the file is loaded and its length is known. Skip if the saved spot
    // is essentially the end (treat a near-finished file as "watched" and start it fresh).
    if (!resumePath_.isEmpty() && resumeSeek_ > 1.0 && resumeSeek_ < seconds - 5.0)
        player_->setPosition(resumeSeek_);
    resumeSeek_ = 0.0; // one-shot
}

void MainWindow::onPosition(double seconds)
{
    if (!sliderDown_ && duration_ > 0.0)
        seek_->setValue(static_cast<int>(seconds / duration_ * 1000.0));
    time_->setText(fmt(seconds) + QStringLiteral(" / ") + fmt(duration_));

    audioPos_ = seconds;
    // Throttle resume writes so we're not hammering the ini every position tick.
    if (!resumePath_.isEmpty() && std::abs(seconds - lastSavedPos_) >= 5.0)
        persistResume();
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
