#include <QApplication>
#include <QIcon>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QMetaType>
#include <QEventLoop>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <clocale>
#include "ui/MainWindow.h"
#include "ui/ProfileDialog.h"
#include "core/ProfileStore.h"
#include "core/CloudSync.h"

// If signed in to Google Drive, pull a newer state bundle BEFORE the app reads any settings, so the
// session starts from the latest synced profiles/favorites/addons/themes. Best-effort with a timeout.
static void cloudPullAtStartup()
{
    if (!CloudSync::isConfigured()) return;
    CloudSync cloud;
    if (!cloud.isSignedIn()) return;
    QEventLoop loop;
    QTimer::singleShot(8000, &loop, &QEventLoop::quit); // never hang startup on a slow/absent network
    cloud.checkStatus([&cloud, &loop](const CloudSync::Status& st) {
        if (!st.reached || !st.hasRemote || !st.remoteChanged) { loop.quit(); return; }
        if (st.localChanged)
        {
            // Both sides changed since the last sync -> let the user decide.
            QMessageBox box(QMessageBox::Warning, QObject::tr("Sync conflict"),
                QObject::tr("Your Google Drive has newer changes from another device, and this device also has "
                            "unsynced changes.\n\nUse the cloud's data (replace this device), or keep this device "
                            "(it will overwrite the cloud later)?"));
            QPushButton* useCloud = box.addButton(QObject::tr("Use cloud data"), QMessageBox::AcceptRole);
            box.addButton(QObject::tr("Keep this device"), QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == useCloud)
                cloud.applyRemote(st.fileId, st.modifiedIso, [&loop](bool) { loop.quit(); });
            else
                loop.quit(); // keep local; the exit push will overwrite the cloud
        }
        else
        {
            cloud.applyRemote(st.fileId, st.modifiedIso, [&loop](bool) { loop.quit(); }); // remote-only change
        }
    });
    loop.exec();
}

// One-time migration from the old "Goliath" naming. If the old goliath.ini exists and the new
// mymediavault.ini does not, copy it across and rewrite the renamed addon ids (com.goliath.* ->
// com.mymediavault.*) in both keys and values, so existing profiles, API keys and favourites carry over.
// Idempotent: once mymediavault.ini exists this is skipped.
static void migrateLegacySettings()
{
    const QString dir = QCoreApplication::applicationDirPath();
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
    QApplication::setApplicationName(QStringLiteral("My Media Vault"));
    QApplication::setApplicationDisplayName(QStringLiteral("My Media Vault"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    migrateLegacySettings(); // carry over the old goliath.ini before any setting is read
    cloudPullAtStartup();    // then pull a newer cloud snapshot (if signed in) before loading state

    // A profile must be active before the app opens. One profile -> use it; otherwise (none, or several)
    // the user picks an existing profile or creates one. Refusing to choose at startup exits the app.
    const QVector<Profile> profiles = ProfileStore::list();
    if (profiles.size() == 1)
    {
        ProfileStore::setCurrent(profiles.first().id);
    }
    else
    {
        ProfileDialog dlg(/*mustChoose*/ true);
        if (dlg.exec() != QDialog::Accepted || dlg.selectedId().isEmpty())
            return 0; // no profile chosen -> don't start
        ProfileStore::setCurrent(dlg.selectedId());
    }

    MainWindow window;
    window.setWindowTitle(QStringLiteral("My Media Vault"));
    window.resize(1280, 760);
    window.show();
    window.raise();
    window.activateWindow(); // foreground + keyboard focus so arrow keys work without a click first
    return app.exec();
}
