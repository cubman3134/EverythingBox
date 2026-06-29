#include <QApplication>
#include "core/AppPaths.h"
#include <QIcon>
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

int main(int argc, char** argv)
{
    // libmpv requires the C numeric locale, otherwise option/number parsing breaks. Set it before Qt.
    std::setlocale(LC_NUMERIC, "C");

    QApplication app(argc, argv);
    capLogAtStartup();                      // trim a runaway log before we start appending to it
    qInstallMessageHandler(appLogHandler);  // no console (GUI app) -> send all diagnostics to the log file
    QApplication::setApplicationName(QStringLiteral("My Media Vault"));
    QApplication::setApplicationDisplayName(QStringLiteral("My Media Vault"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    // Comfortable, remote/touch-friendly base sizing for generic controls (dialogs, lists, inputs). Views
    // that set their own styles (Home chrome, settings panels) keep theirs; this just enlarges the rest.
    app.setStyleSheet(QStringLiteral(
        "QPushButton{min-height:30px;padding:8px 16px;font-size:14px;}"
        "QLineEdit,QComboBox,QAbstractSpinBox{min-height:30px;padding:5px 10px;font-size:14px;}"
        "QCheckBox,QRadioButton{font-size:14px;spacing:8px;}"
        "QCheckBox::indicator,QRadioButton::indicator{width:20px;height:20px;}"
        "QListWidget::item,QListView::item{min-height:34px;}"
        "QScrollBar:vertical{width:14px;}QScrollBar:horizontal{height:14px;}"));

    migrateLegacySettings(); // carry over the old goliath.ini before any setting is read
    cloudPullAtStartup();    // then pull a newer cloud snapshot (if signed in) before loading state

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
    window.resize(1280, 760);
    window.show();
    window.raise();
    window.activateWindow(); // foreground + keyboard focus so arrow keys work without a click first
    return app.exec();
}
