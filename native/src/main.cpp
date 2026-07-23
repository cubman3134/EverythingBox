#include <QApplication>
#ifdef MMV_HAVE_QML
#include "theme2/MpvPreview.h"
#include <QQuickWindow>
#include <QtQml>
#endif
#include "core/AppPaths.h"
#include "core/AssetBootstrap.h"
#include <QIcon>
#include <QScreen>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QMetaType>
#include <QEventLoop>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QDateTime>
#include <QMutex>
#include <clocale>
#include "ui/MainWindow.h"
#include "ui/ProfileDialog.h"
#include "core/ProfileStore.h"
#include "core/CloudSync.h"
#include "core/Settings.h"
#include "core/ConsumptionStats.h" // mdsync T3: fold legacy accumulators into this device's namespace at startup
#include "core/PlayStats.h"
#include "core/PerfTrace.h"

// App version (keep in sync with project(VERSION ...) in native/CMakeLists.txt).
static constexpr const char* kAppVersion = "0.4.19";

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

// If signed in to Google Drive, ALWAYS pull the latest state bundle BEFORE the app reads any settings, so
// every session starts from the cloud's profiles/favorites/addons/themes (the exit push saved them last
// time). Best-effort with a timeout so a slow/absent network never hangs startup.
static void cloudPullAtStartup()
{
    if (!CloudSync::isConfigured()) return;
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

// One-time migration from the old "Goliath" naming. If the old goliath.ini exists and the new
// mymediavault.ini does not, copy it across and rewrite the renamed addon ids (com.goliath.* ->
// com.mymediavault.*) in both keys and values, so existing profiles, API keys and favourites carry over.
// Idempotent: once mymediavault.ini exists this is skipped.
static void migrateLegacySettings()
{
    const QString dir = AppPaths::dataDir();
    const QString oldIni = dir + QStringLiteral("/goliath.ini");
    const QString newIni = dir + QStringLiteral("/mymediavault.ini");
    if (QFile::exists(newIni) || !QFile::exists(oldIni)) return;
    if (!QFile::copy(oldIni, newIni)) return;

    QSettings s(newIni, QSettings::IniFormat);
    const QString oldNs = QStringLiteral("com.goliath.");
    const QString newNs = QStringLiteral("com.mymediavault.");
    const QStringList keys = s.allKeys();
    for (const QString& k : keys)
    {
        QVariant v = s.value(k);
        // Rewrite the addon namespace inside string values too (e.g. a favourite's stored addonId).
        if (v.typeId() == QMetaType::QString)
        {
            QString sv = v.toString();
            if (sv.contains(oldNs)) { sv.replace(oldNs, newNs); v = sv; }
        }
        if (k.contains(oldNs))
        {
            QString nk = k; nk.replace(oldNs, newNs);
            s.setValue(nk, v);
            s.remove(k);
        }
        else if (v.typeId() == QMetaType::QString && v.toString() != s.value(k).toString())
        {
            s.setValue(k, v);
        }
    }
    s.sync();
}

// Ends the startup.firstpaint span on the main window's first real Paint event — its true first on-screen
// frame. This is deliberately distinct from startup.total's zero-timer end: a singleShot(0) can fire BEFORE
// the window actually paints if the GUI thread is about to block on synchronous work, so a paint-based span
// is the honest guard against a regression where startup work stalls the first paint (e.g. a slow audio /
// device open landing on the GUI thread). Installed only under MMV_PERF, so normal runs pay nothing; it
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

#ifdef MMV_HAVE_QML
    // The themed home is a QQuickView embedded via createWindowContainer (see ThemeEngine), rendered with
    // Qt Quick's software backend. The app also drives libmpv through a QOpenGLWidget, and a GPU-accelerated
    // QQuickWidget sharing GL with it renders blank; the software QQuickView avoids the GL path entirely.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    // The themed Video element's real-playback path: a libmpv software-render item themes create as MMV
    // MpvPreview (Video.qml instantiates it at runtime, guarded, when a playable clip exists).
    qmlRegisterType<MpvPreview>("MMV", 1, 0, "MpvPreview");
#endif

    QApplication app(argc, argv);
    capLogAtStartup();                      // trim a runaway log before we start appending to it
    qInstallMessageHandler(appLogHandler);  // no console (GUI app) -> send all diagnostics to the log file
    QApplication::setApplicationName(QStringLiteral("My Media Vault"));
    QApplication::setApplicationDisplayName(QStringLiteral("My Media Vault"));
    QApplication::setApplicationVersion(QString::fromLatin1(kAppVersion));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

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
    // no-op UNLESS MMV_TEST_BOOTSTRAP_SRC points at a source dir — the env override makes the whole pipeline
    // desktop-verifiable without an Android toolchain (see probe_bootstrap).
#if defined(Q_OS_ANDROID)
    AssetBootstrap::run(QStringLiteral("assets:/mmv"), AppPaths::dataDir(),
                        QString::fromLatin1(kAppVersion));
#elif defined(Q_OS_IOS)
    // iOS: the stock themes2/ + addons are staged at the bundle root as mmv/ (see the if(IOS) CMake block);
    // extract them into the writable data dir exactly like the Android assets:/mmv flow.
    AssetBootstrap::run(QCoreApplication::applicationDirPath() + QStringLiteral("/mmv"),
                        AppPaths::dataDir(), QString::fromLatin1(kAppVersion));
#else
    if (qEnvironmentVariableIsSet("MMV_TEST_BOOTSTRAP_SRC"))
        AssetBootstrap::run(qEnvironmentVariable("MMV_TEST_BOOTSTRAP_SRC"), AppPaths::dataDir(),
                            QString::fromLatin1(kAppVersion));
#endif

    migrateLegacySettings(); // carry over the old goliath.ini before any setting is read
    cloudPullAtStartup();    // then pull a newer cloud snapshot (if signed in) before loading state
    ProfileStore::migrateIcons(); // one-time: repair legacy mojibake-corrupted profile icons on disk
    ConsumptionStats::migrate();  // one-time: fold pre-upgrade un-namespaced stats into this device's namespace
    PlayStats::migrate();         // one-time: same for per-game playtime (before any CloudMerge serialize)

    // A profile must be active before the app is usable. One profile -> use it. With none or several, the
    // picker is shown inline once the window is up (chooseProfile); set a provisional current first so the
    // shell can build, then the inline picker confirms/creates the real choice.
    const QVector<Profile> profiles = ProfileStore::list();
    const bool chooseProfile = (profiles.size() != 1);
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
    window.setWindowTitle(QStringLiteral("My Media Vault"));
#ifdef Q_OS_IOS
    // A phone screen is far narrower than the desktop layout's aggregate minimum width, and a fullscreen
    // window can never shrink below its layout minimum — override it so fullscreen clamps to the real
    // screen (an explicit minimum takes precedence over the layout-derived one).
    window.setMinimumSize(1, 1);
    if (QScreen* s = QGuiApplication::primaryScreen()) window.resize(s->geometry().size());
#else
    window.resize(1280, 760);                              // the size we restore to when leaving full screen
#endif
    // startup.firstpaint spans show() -> the window's first real paint (ends via FirstPaintProbe). Only armed
    // under MMV_PERF. It is the honest complement to startup.total's zero-timer end below.
    if (PerfTrace::enabled())
    {
        PerfTrace::begin(QStringLiteral("startup.firstpaint"));
        qApp->installEventFilter(new FirstPaintProbe(&window));
    }
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
