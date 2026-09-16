// "Configure on website…" (issue #80), the MainWindow half — its own translation unit, off MainWindow.cpp, for
// the same reason as MainWindowPlayOn.cpp (#143/#186): a feature's members should not cost every other branch
// a conflict in the busiest file in the repository.
//
// One entry point for every place the action appears: the "needs to be configured" guidance row on the home
// (both layouts reach it through HomeView::activateItem), the themed Add-ons detail panel, and the classic
// Add-ons screen. It derives {base}/configure (StremioTranslate::configureUrlFor, pinned by probe_stremio),
// hands it to the browser through the existing openAuthPage path, and opens the waiting card
// (AddonConfigurePanel) — which installs through addRemoteSource, so a new link for the same add-on REPLACES
// the installed entry in place (AddonManager::planRemoteAdd, pinned by probe_addon).
#include "MainWindow.h"

#include <QDateTime>
#include <QFile>
#include <QUrl>

#include "../addons/AddonManager.h"
#include "../addons/StremioTranslate.h"
#include "../core/AppPaths.h"
#include "../core/LogSafeText.h"
#include "../theme2/FormFactor.h"
#include "AddonConfigurePanel.h"
#include "LibraryView.h"
#ifdef EB_HAVE_QML
#include "../theme2/ThemedPanelHost.h"
#endif

namespace {
void configureLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}
} // namespace

void MainWindow::openAddonConfigure(const QString& sourceId)
{
    if (!addons_) return;
    LoadedAddon* s = addons_->sourceById(sourceId);
    if (!s || s->transport != LoadedAddon::RemoteHttp) { notify(tr("That add-on can't be configured on a website.")); return; }
    const QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
    const QString page = StremioTranslate::configureUrlFor(s->baseUrl);
    if (page.isEmpty()) { notify(tr("%1 has no website to configure it on.").arg(name)); return; }

    // The page URL can carry the add-on's current options (a configured Torrentio's base holds them, debrid
    // keys included), so the log names the host and the page — never the path in between or a query.
    configureLog(QStringLiteral("addon configure: open requested for %1 -> %2").arg(sourceId, LogSafeText::url(page)));
    // EB_UITEST_NO_BROWSER: a UI-test drive proves the open was REQUESTED (the line above) without putting a
    // browser window on the desktop of the machine running the drive. Honoured only with the test channel on.
    const bool suppress = qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_UITEST_NO_BROWSER");
    if (!suppress) openAuthPage(page);   // does nothing on a TV (no browser); the card still explains the flow

#ifdef EB_HAVE_QML
    const bool themed = themedHomeEnabled() && themedPanelHost_;
#else
    const bool themed = false;
#endif
    new AddonConfigurePanel(addons_.get(), name, [this, themed, name] {
        // Installed (added, or replaced in place). Refresh the Add-ons surface the user came from, if any.
#ifdef EB_HAVE_QML
        if (themed && themedPanelIsTop(name))            // the add-on's detail panel: back to a rebuilt root
        {
            themedPanelHost_->handleBack();
            openLibrary();
            return;
        }
        if (themed && themedPanelIsTop(tr("Add-ons"))) { openLibrary(); return; }
#else
        Q_UNUSED(themed); Q_UNUSED(name);
#endif
        if (library_) library_->refreshSources();
    }, this);
}
