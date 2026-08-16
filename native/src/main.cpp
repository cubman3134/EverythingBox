#include <QApplication>
#include "core/AppBrand.h"
#ifdef EB_HAVE_QML
#include "theme2/MpvPreview.h"
#include <QQuickWindow>
#include <QtQml>
#endif
#include "core/AppPaths.h"
#include "core/AssetBootstrap.h"
#include "core/BrandMigration.h" // rebrand T3: per-step, resumable move of an existing install onto this brand
#include "core/SafeAreaInsets.h"
#include <QIcon>
#include <QScreen>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QEventLoop>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QDateTime>
#include <QMutex>
#include <clocale>
#include "ui/MainWindow.h"
#include "ui/ProfileDialog.h"
#include "core/ProfilePasscode.h"   // mustShowPicker — the pure always-ask-at-launch decision (#30)
#include "core/ProfileStore.h"
#include "core/CloudSync.h"
#include "core/Settings.h"
#include "core/ConsumptionStats.h" // mdsync T3: fold legacy accumulators into this device's namespace at startup
#include "core/PlayStats.h"
#include "core/SaveMeta.h"     // save-sync T4: the one-time stray core-save sweep
#include "core/PerfTrace.h"
#include "core/CrashReport.h"  // issue #28: first-chance AV reporter, installed before the GUI comes up
#include "core/UiTestServer.h" // issue #172: the UI-test channel listens BEFORE the startup work, not after

// App version (keep in sync with project(VERSION ...) in native/CMakeLists.txt).
static constexpr const char* kAppVersion = "0.5.393";

// Path of the single diagnostic log (shared with the stream/manga resolution tracing). The Settings ▸ Debug
// viewer reads this file.
static QString logPath() { return AppPaths::dataDir() + QStringLiteral("/stream_debug.log"); }

// Route Qt diagnostics (qDebug/qInfo/qWarning/qCritical, plus internal Qt/library messages) to the log file.
// As a GUI-subsystem app there is no console, so this is the only place errors are recorded. Thread-safe.
static void appLogHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    static QMutex mtx; QMutexLocker lock(&mtx);
    const char* lvl = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
                    : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
    QFile f(logPath());
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  [")
                 + QString::fromLatin1(lvl) + QStringLiteral("] ") + msg + QStringLiteral("\n")).toUtf8());
}

// Keep the log from growing without bound: if it's over ~1 MB at startup, drop it and start fresh.
static void capLogAtStartup()
{
    const QFileInfo fi(logPath());
    if (fi.exists() && fi.size() > 1024 * 1024) QFile::remove(logPath());
}

// If the configured sync backend is usable (Drive signed in, or a self-hosted server URL set), ALWAYS pull the
// latest state bundle BEFORE the app reads any settings, so every session starts from the cloud's
// profiles/favorites/addons/themes (the exit push saved them last time). Best-effort with a timeout so a
// slow/absent network never hangs startup.
static void cloudPullAtStartup()
{
    // Backend-aware usability gate (Increment C): construct the configured backend and ask IT whether it can be
    // used — Drive = a refresh token, the self-hosted server = a URL is set. The old CloudSync::isConfigured()
    // pre-check was Drive-OAuth-specific and wrongly skipped the startup pull for a server-backend user (who has
    // no OAuth client at all); cloud.isSignedIn() already answers "usable" for whichever backend is selected.
    CloudSync cloud;
    if (!cloud.isSignedIn()) return;
    QEventLoop loop;
    QTimer::singleShot(8000, &loop, &QEventLoop::quit); // never hang startup on a slow/absent network
    cloud.checkStatus([&cloud, &loop](const CloudSync::Status& st) {
        // A failed file-query lands here as !hasRemote and is harmless: we only decline to pull (no seed, no push,
        // nothing written), so the findFile blindness cannot destroy data on this path. Behavior left unchanged.
        if (!st.reached || !st.hasRemote) { loop.quit(); return; }
        cloud.applyRemote(st.fileId, st.modifiedIso, st.remoteHash, [&loop](bool) { loop.quit(); }); // always take the cloud
    });
    loop.exec();
}

// Move an install created under the PREVIOUS brand onto the current one (see core/BrandMigration.h). Must run
// before any setting is read: Settings::store() holds a function-local static QSettings, so the first read
// snapshots whatever file is on disk at that moment — if that happens before the ini is copied into place, the
// whole session runs on an empty settings file and then writes it back. Also before the startup cloud pull, so
// the pull resolves the renamed Drive folder rather than seeding a fresh one beside it.
//
// The local steps are synchronous; only the Drive half is async, so the event loop below runs only when there
// is a network round-trip to wait for, and gives up after the same 8s budget as cloudPullAtStartup — an
// unreachable Drive leaves its flags unset and the migration simply resumes next launch.
static void brandMigrationAtStartup()
{
    QEventLoop loop;
    bool finished = false;
    BrandMigration::run([&loop, &finished](bool) { finished = true; loop.quit(); });
    if (finished) return;                                  // resolved synchronously — nothing to wait for
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);    // never hang startup on a slow/absent network
    loop.exec();
}

// Ends the startup.firstpaint span on the main window's first real Paint event — its true first on-screen
// frame. This is deliberately distinct from startup.total's zero-timer end: a singleShot(0) can fire BEFORE
// the window actually paints if the GUI thread is about to block on synchronous work, so a paint-based span
// is the honest guard against a regression where startup work stalls the first paint (e.g. a slow audio /
// device open landing on the GUI thread). Installed only under EB_PERF, so normal runs pay nothing; it
// removes itself and self-destructs once the first paint fires.
class FirstPaintProbe : public QObject
{
public:
    explicit FirstPaintProbe(QWidget* win) : win_(win) {}
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Paint)
            if (auto* w = qobject_cast<QWidget*>(o); w && w->window() == win_)
            {
                PerfTrace::end(QStringLiteral("startup.firstpaint"));
                qApp->removeEventFilter(this);
                deleteLater();
            }
        return false;
    }
private:
    QWidget* win_;
};

int main(int argc, char** argv)
{
    PerfTrace::begin(QStringLiteral("startup.total")); // ends after the first paint (zero-timer below)

    // libmpv requires the C numeric locale, otherwise option/number parsing breaks. Set it before Qt.
    std::setlocale(LC_NUMERIC, "C");

#ifdef Q_OS_IOS
    // iOS: flush the widget backingstore through Metal. The default raster flush goes through OpenGL ES,
    // which fails in the simulator (and EAGL is deprecated on device) — the app ran fine but the screen
    // stayed black. Must be set before the QApplication is constructed.
    qputenv("QT_WIDGETS_RHI", "1");
    qputenv("QT_WIDGETS_RHI_BACKEND", "metal");
#endif

#ifdef EB_HAVE_QML
    // The themed home is a QQuickView embedded via createWindowContainer (see ThemeEngine), rendered with
    // Qt Quick's software backend. The app also drives libmpv through a QOpenGLWidget, and a GPU-accelerated
    // QQuickWidget sharing GL with it renders blank; the software QQuickView avoids the GL path entirely.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    // The themed Video element's real-playback path: a libmpv software-render item themes create as EB
    // MpvPreview (Video.qml instantiates it at runtime, guarded, when a playable clip exists).
    qmlRegisterType<MpvPreview>("EB", 1, 0, "MpvPreview");
#endif

    QApplication app(argc, argv);
    // AGAIN, after the QApplication: on Unix its constructor re-applies the environment's locale
    // (setlocale(LC_ALL, "")), clobbering the early call above — and libmpv REFUSES to create a
    // context under a non-C LC_NUMERIC ("Non-C locale detected", then mpv_create returns null).
    // Only reproducible when launched from a terminal with LANG set; Finder launches carry no locale.
    // This placement is Qt's documented remedy (QCoreApplication "Locale Settings").
    std::setlocale(LC_NUMERIC, "C");
    capLogAtStartup();                      // trim a runaway log before we start appending to it
    qInstallMessageHandler(appLogHandler);  // no console (GUI app) -> send all diagnostics to the log file
    // WARNING: setApplicationName is NOT cosmetic — CHANGING THIS VALUE MOVES THE MOBILE DATA DIRECTORY.
    // On Android/iOS AppPaths::dataDir() resolves through QStandardPaths::AppDataLocation, which
    // incorporates applicationName (on iOS, ~/Library/Application Support/<applicationName>). Renaming it
    // as part of a prose sweep would silently strand every mobile user's ini, saves, states and addons at
    // the old path — a wipe with no migration and no error. So it stays on the LEGACY spaced form (a
    // "lookup that tolerates the legacy name until migration is confirmed") until the brand migration
    // owns the mobile path move as an explicit, migrated step. The DISPLAY name below is pure chrome and
    // carries no path meaning, so it flips to the new brand now.
    //
    // Rebrand T3 CONSIDERED migrating this and deliberately did not. Renaming it is not a rename — it is a
    // recursive move of the whole mobile data directory (ini, saves, states, addons, themes), it cannot be
    // exercised by a desktop headless probe (AppPaths::dataDir() only branches under Q_OS_ANDROID/Q_OS_IOS),
    // and it cannot reuse BrandMigration's flag mechanism unchanged, because those flags live in the ini
    // INSIDE the directory being moved. It is a device-tested step of its own; see core/BrandMigration.h.
    // Until it lands, this pin IS the tolerance, and it must not be flipped by a prose sweep.
    QApplication::setApplicationName(QString::fromLatin1(AppBrand::Legacy::kDisplayName));
    QApplication::setApplicationDisplayName(QString::fromLatin1(AppBrand::kDisplayName));
    QApplication::setApplicationVersion(QString::fromLatin1(kAppVersion));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    // Issue #28: a ~1-in-6 access violation inside QQuickRepeater::clear() that five reproduction
    // campaigns failed to trigger. The diagnosis came entirely from ONE full WER dump — and all five
    // dumps were later evicted by WER's LocalDumps rotation, with no copies. So the next occurrence
    // records itself: a first-chance vectored handler writes the faulting module+offset, the bad
    // address and the registers into crash_report.log, beside stream_debug.log, in a file we own and
    // nothing rotates out. It returns EXCEPTION_CONTINUE_SEARCH, so WER still writes its dump and
    // nothing else about the process changes.
    //
    // Installed HERE, not earlier: the log path is resolved once, now, so the handler itself never
    // touches AppPaths, QString or any allocator — and AppPaths::dataDir() needs the QApplication (and
    // on mobile the application NAME, set just above) to already exist. Still long before any window.
    CrashReport::install((AppPaths::dataDir() + QStringLiteral("/crash_report.log")).toUtf8().constData());

    // Issue #172: bring the UI-test channel up HERE — before the asset bootstrap, the brand migration, the
    // cloud pull, and the whole MainWindow ctor. It used to be created ~400 lines into that ctor, which meant
    // any stall on the way there left no pipe at all: the harness saw a connect failure, which is exactly what
    // it sees when the app was never launched with EB_UITEST, and a live verification became "nothing
    // happened, presumably fine". Now the channel exists from the first moments and answers `not-ready` until
    // the window binds its hooks, so a stuck startup is a specific answer instead of silence.
    //
    // IniPhase::NotSettled: the Settings ▸ Debug toggle cannot be read yet. Settings::store() holds a
    // function-local static QSettings that snapshots the ini on its FIRST read, and the brand migration below
    // has to copy the ini into place before that happens (see the note on brandMigrationAtStartup) — reading
    // it here would run the whole session off an empty settings file. The toggle gets its own
    // ensureListening() further down, once the ini is settled.
    //
    // This line used to be `if (UiTestServer::wantedFromEnvOrArgs()) UiTestServer::ensureListening();` and was
    // safe only because wanted()'s `||` happened to evaluate the env/args half first — a startup invariant
    // living in another file's operand order (issue #177). Stating the phase moves the guard inside
    // ensureListening(), where it cannot be forgotten: the argument has no default, so a call site that says
    // nothing does not compile.
    UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled);

    // Comfortable, remote/touch-friendly base sizing for generic controls (dialogs, lists, inputs). Views
    // that set their own styles (Home chrome, settings panels) keep theirs; this just enlarges the rest.
    // The :focus rules are the app-wide SELECTION HIGHLIGHT: stylesheet-styled controls suppress the
    // native focus rectangle, so any widget without its own :focus rule (e.g. the profile picker's
    // buttons) looked completely unselected while focused — "the selection disappeared" when arrowing
    // onto it. Screens with their own :focus styles (panel rows, overlays, the esc menu) win over these.
    app.setStyleSheet(QStringLiteral(
        "QPushButton{min-height:30px;padding:8px 16px;font-size:14px;}"
        "QPushButton:focus{background:#2D6CDF;color:#fff;border:2px solid #5B8CFF;border-radius:6px;}"
        "QLineEdit,QComboBox,QAbstractSpinBox{min-height:30px;padding:5px 10px;font-size:14px;}"
        // Focused = SELECTED: an outline around the box (you navigated to it, you're not typing yet).
        "QLineEdit:focus,QComboBox:focus,QAbstractSpinBox:focus{border:2px solid #5B8CFF;border-radius:4px;}"
        // EDITING (a live cursor, set by NavTextField): a brighter, filled look so it's clearly distinct
        // from the plain selection outline.
        "QLineEdit[mmvEditing=\"true\"]{background:#0d0f14;border:2px solid #8FB2FF;border-radius:4px;}"
        // A scrollable text view (the Debug log) gets the same two-state outline: SELECTED shows a border,
        // INTERACTING (scroll mode) shows the brighter one.
        "QPlainTextEdit:focus,QTextEdit:focus{border:2px solid #5B8CFF;}"
        "QPlainTextEdit[mmvEditing=\"true\"],QTextEdit[mmvEditing=\"true\"]{border:2px solid #8FB2FF;}"
        "QCheckBox,QRadioButton{font-size:14px;spacing:8px;}"
        "QCheckBox:focus,QRadioButton:focus{color:#2D6CDF;font-weight:bold;}"
        "QCheckBox::indicator,QRadioButton::indicator{width:20px;height:20px;}"
        "QSlider:focus{background:rgba(91,140,255,0.20);border-radius:4px;}"
        "QListWidget::item,QListView::item{min-height:34px;}"
        "QScrollBar:vertical{width:14px;}QScrollBar:horizontal{height:14px;}"));

    // First-run asset extraction (D2 Task 2). A fresh Android install boots into an empty AppPaths::dataDir()
    // with the stock themes2/ + first-party addons/ only inside the read-only APK, so extract them before
    // AddonManager/ThemeEngine (built by MainWindow below) read those dirs off disk. On desktop this is a
    // no-op UNLESS EB_TEST_BOOTSTRAP_SRC points at a source dir — the env override makes the whole pipeline
    // desktop-verifiable without an Android toolchain (see probe_bootstrap).
#if defined(Q_OS_ANDROID)
    AssetBootstrap::run(QStringLiteral("assets:/eb"), AppPaths::dataDir(),
                        QString::fromLatin1(kAppVersion));
#elif defined(Q_OS_IOS)
    // iOS: the stock themes2/ + addons are staged at the bundle root as eb/ (see the if(IOS) CMake block);
    // extract them into the writable data dir exactly like the Android assets:/eb flow.
    AssetBootstrap::run(QCoreApplication::applicationDirPath() + QStringLiteral("/eb"),
                        AppPaths::dataDir(), QString::fromLatin1(kAppVersion));
#else
    if (qEnvironmentVariableIsSet("EB_TEST_BOOTSTRAP_SRC"))
        AssetBootstrap::run(qEnvironmentVariable("EB_TEST_BOOTSTRAP_SRC"), AppPaths::dataDir(),
                            QString::fromLatin1(kAppVersion));
#endif
    // The DISK half of the XMB -> Triple theme rename (roadmap #57), deliberately OUTSIDE the platform #if
    // above: the duplicate "Triple" row is an UPGRADE artefact, and a desktop upgrade never runs
    // AssetBootstrap::run at all (it overlays a new themes2/ into the data dir instead), so folding this into
    // run() would fix Android/iOS and leave every desktop install showing the theme twice. Runs before any
    // theme is read — MainWindow's ctor is what first calls ThemeEngine::availableThemes().
    AssetBootstrap::retireRenamedTheme(AppPaths::dataDir());

    BrandMigration::migrateGoliathIni(AppPaths::dataDir()); // carry over the oldest ini before any read
    brandMigrationAtStartup(); // then move that install onto the CURRENT brand — still before any read
    // The ini is settled, so the Settings ▸ Debug toggle is now safe to read. No-op if the env/argument pass
    // above already brought the channel up. Deliberately BEFORE the cloud pull: that step runs an event loop
    // on a network round-trip, and it is precisely the kind of startup work whose stall must stay drivable.
    UiTestServer::ensureListening(UiTestServer::IniPhase::Settled);
    cloudPullAtStartup();    // then pull a newer cloud snapshot (if signed in) before loading state
    ProfileStore::migrateIcons(); // one-time: repair legacy mojibake-corrupted profile icons on disk
    ConsumptionStats::migrate();  // one-time: fold pre-upgrade un-namespaced stats into this device's namespace
    PlayStats::migrate();         // one-time: same for per-game playtime (before any CloudMerge serialize)
    SaveMeta::sweepStrays();      // one-time: core save files left loose in the app dir move into saves/,
                                  // before any core runs — after this, saveDir points cores there anyway

    // A profile must be active before the app is usable, and — since issue #30 — the app ALWAYS asks which
    // one, the way a streaming app does. The old rule was `profiles.size() != 1`: a one-profile install (which
    // every install becomes the moment its first profile exists) jumped straight in, so the picker was
    // effectively a multi-user-only screen and a passcode on that profile would have been unreachable.
    //
    // The decision now lives in ProfilePasscode::mustShowPicker, pure and probe-pinned, because it has three
    // inputs that must compose in exactly one way: the count, the user's opt-out preference, and whether that
    // single profile is passcode-protected (which overrides the preference — skipping the picker would skip
    // the only surface that asks for the code). The provisional setCurrent below is unchanged: the picker is
    // shown inline once the window is up, pre-home, so a provisional current only lets the shell build.
    const QVector<Profile> profiles = ProfileStore::list();
    const bool chooseProfile = ProfilePasscode::mustShowPicker(
        int(profiles.size()), Settings::skipProfilePickerWhenSingle(),
        profiles.size() == 1 && !profiles.first().passHash.isEmpty());
    if (profiles.size() == 1)
    {
        ProfileStore::setCurrent(profiles.first().id);
    }
    else if (!profiles.isEmpty())
    {
        bool valid = false;
        const QString cur = ProfileStore::currentId();
        for (const Profile& p : profiles) if (p.id == cur) { valid = true; break; }
        if (!valid) ProfileStore::setCurrent(profiles.first().id); // provisional until the user picks
    }

    MainWindow window(chooseProfile);
    window.setWindowTitle(QString::fromLatin1(AppBrand::kDisplayName)); // chrome only — no path meaning
#ifdef Q_OS_IOS
    // A phone screen is far narrower than the desktop layout's aggregate minimum width, and a fullscreen
    // window can never shrink below its layout minimum — override it so fullscreen clamps to the real
    // screen (an explicit minimum takes precedence over the layout-derived one).
    window.setMinimumSize(1, 1);
    if (QScreen* s = QGuiApplication::primaryScreen()) window.resize(s->geometry().size());
#else
    window.resize(1280, 760);                              // the size we restore to when leaving full screen
    // Test-only seam (parity with EB_TEST_SCREEN_MM): pin the window to a phone/tablet size, so the
    // mobile layout can be exercised with the uitest channel on a desktop host (where the window could
    // otherwise never go below the desktop layout's minimum). Never active in production.
    if (qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_TEST_WINDOW"))
    {
        const QStringList wh = qEnvironmentVariable("EB_TEST_WINDOW").split(QLatin1Char('x'));
        if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0)
        {
            window.setMinimumSize(1, 1);
            window.resize(wh[0].toInt(), wh[1].toInt());
        }
    }
#endif
    // startup.firstpaint spans show() -> the window's first real paint (ends via FirstPaintProbe). Only armed
    // under EB_PERF. It is the honest complement to startup.total's zero-timer end below.
    if (PerfTrace::enabled())
    {
        PerfTrace::begin(QStringLiteral("startup.firstpaint"));
        qApp->installEventFilter(new FirstPaintProbe(&window));
    }
#ifdef Q_OS_IOS
    // Edge-to-edge: don't let Qt inset the window OR any widget in the chain at the safe areas (that
    // reads as the app "cutting off" at the notch/home bar, unlike native apps). Every widget applies
    // the margins independently, so clear the attribute on the whole existing tree; the themed surface
    // fills the physical screen and chrome pads itself from the `safeArea` bridge instead.
    window.setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    for (QWidget* w : window.findChildren<QWidget*>())
        w->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    // Belt & braces: the dark background still paints anywhere the content doesn't.
    {
        QPalette pal = window.palette();
        pal.setColor(QPalette::Window, QColor(0x0F, 0x12, 0x16)); // themes' default `background`
        window.setPalette(pal);
        window.setAutoFillBackground(true);
    }
    // Media-app audio session: play through the silent switch (UI sounds + video/music audio).
    mmvConfigureAudioSession();
    // The platform reports zero insets until the window is actually up — refresh once shown (and again
    // shortly after: the first layout pass can land before UIKit publishes them).
    QTimer::singleShot(0,    [] { SafeAreaBridge::instance().refresh(); });
    QTimer::singleShot(500,  [] { SafeAreaBridge::instance().refresh(); });
    QTimer::singleShot(2000, [] { SafeAreaBridge::instance().refresh(); });
#endif
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // Mobile has no windowed mode: the app is always fullscreen. On Android, showFullScreen() also drives Qt 6.8's
    // QtActivityDelegate into sticky-immersive — it maps the top-level Qt::WindowFullScreen state onto the
    // Android WindowInsetsController (BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE, API 30+) / the legacy
    // SYSTEM_UI_FLAG_IMMERSIVE_STICKY, so the status + navigation bars stay hidden over video and the
    // emulator and auto-re-hide after a swipe, with NO hand-rolled JNI and no custom manifest theme. See
    // .superpowers/sdd/d2-task-3-report.md for the investigation.
    window.showFullScreen();
#else
    if (Settings::startFullscreen()) window.showFullScreen();
    else                             window.show();
#endif
    // A zero-timer fires after the event loop's first pass (first paint), so startup.total spans launch->visible.
    QTimer::singleShot(0, [] { PerfTrace::end(QStringLiteral("startup.total")); });
    window.raise();
    window.activateWindow(); // foreground + keyboard focus so arrow keys work without a click first
    return app.exec();
}
