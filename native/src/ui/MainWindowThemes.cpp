// MainWindowThemes — the theme browser's members beyond "install" (issue #91).
//
// A FEATURE TU, for the reason MainWindowPlayOn.cpp and MainWindowOpenFail.cpp already are ones:
// MainWindow.cpp is the file every concurrent branch collides in, and a feature that grows four members
// should not grow them there. Only the declarations are in MainWindow.h.
//
// What is here is everything BOTH Appearance surfaces have to agree about and that is not already in
// ThemeRegistry: which registries are configured, which themes ship inside the app, and what removing one
// means. The classic surface (RegistryBrowser) asks ThemeRegistry the same three questions through its own
// copies of the ini/paths lookups, so the ANSWERS are shared even though the lookups are not — the thing
// that must not fork is the rule, and the rule lives in core.
#include "MainWindow.h"

#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/ThemeRegistry.h"
#include "../theme2/ThemeEngine.h"
#include "../theme2/ThemedPanelHost.h"   // PanelRow — the row this patches after a removal

#include <QDir>
#include <QSettings>
#include <QString>
#include <QStringList>

// The built-in theme registry. Spelled here as well as in MainWindow.cpp's gallery panel because this is
// where the EXTRAS list is read, and a function that returns "the registries" while omitting the built-in
// one is a trap for the next caller. Both spellings are the same string, and probe coverage of the host
// rule does not depend on which one a call site used.
static QString builtInThemeIndexUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json");
}

// The registries the USER added — the `registry/themesExtras` ini key the classic browser writes, which is
// deliberately the same list the decorations gallery reads (one community registry format, not two).
//
// This is the list the download host rule takes: an entry may point its archive at the index's own host, or
// at a host the user chose by adding that registry. Nowhere else.
QStringList MainWindow::themeExtraRegistryUrls() const
{
    QSettings iniStore(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    QStringList out;
    for (const QString& u : iniStore.value(QStringLiteral("registry/themesExtras")).toStringList())
        if (!u.trimmed().isEmpty() && !out.contains(u.trimmed())) out << u.trimmed();
    return out;
}

QStringList MainWindow::themeRegistryUrls() const
{
    QStringList out;
    out << builtInThemeIndexUrl();
    for (const QString& u : themeExtraRegistryUrls())
        if (!out.contains(u)) out << u;
    return out;
}

// The themes that ship inside the app. ThemeRegistry reads them from the manifest staged beside the themes
// themselves; this is only the path lookup, kept off both surfaces so neither can point it somewhere else.
QStringList MainWindow::bundledThemeFolders() const
{
    return ThemeRegistry::bundledFolders(ThemeEngine::themesRoot());
}

// Remove an installed theme. Every refusal — a bundled theme, a name that is not one plain segment, anything
// that resolves outside the themes root, a folder that holds no theme.json — is ThemeRegistry's, so this
// surface and the classic one refuse the same set and say the same words.
//
// The ROW is patched rather than the panel rebuilt, exactly as removeDecorationPack does it: presenting the
// gallery again would push a second panel level (presentThemeRegistry starts with present(), not
// replaceTop), and Back would then walk out through a stale copy of the list.
void MainWindow::removeInstalledTheme(ThemeRegistry::Entry entry, const QString& rowId)
{
    // An install holds nested event loops and this panel stays live under them, so a Remove row is still
    // activatable from inside one. Deleting the folder an install is writing into is not a race worth
    // having; refuse and say so, in the same words the install path refuses a second install.
    if (themeInstallBusy_)
    {
        updatePanelInfo(QStringLiteral("treg.status"),
                        tr("An install is running — it has to finish first."));
        return;
    }

    const QString label = entry.name.isEmpty() ? entry.folder() : entry.name;
    QString err;
    if (!ThemeRegistry::removeInstalled(ThemeEngine::themesRoot(), entry.folder(), bundledThemeFolders(),
                                        &err))
    {
        updatePanelInfo(QStringLiteral("treg.status"), err);
        return;
    }

    // Back to what an uninstalled entry's row says. The picker reads the directory live, so the theme is out
    // of it the next time Theme… is opened; if the removed theme was the one being rendered, ThemeEngine
    // falls back on its own the next time it resolves.
    PanelRow r;
    r.kind  = PanelRow::Action;
    r.id    = rowId;
    r.label = label;
    QString sub = entry.author.isEmpty() ? QString() : tr("by %1").arg(entry.author);
    if (!entry.version.isEmpty())
        sub += (sub.isEmpty() ? QString() : QStringLiteral(" · ")) + entry.version;
    r.value   = sub.isEmpty() ? tr("Install") : sub;
    r.enabled = true;
    themedPanelHost_->updateRow(rowId, r);
    updatePanelInfo(QStringLiteral("treg.status"), tr("Removed \"%1\".").arg(label));
}
