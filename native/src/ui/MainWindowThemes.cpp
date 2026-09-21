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
#include "../core/ProfileStore.h"        // the preview writes the per-PROFILE theme choice, like the picker
#include "../core/ThemeChoice.h"         // …and ThemeChoice owns that key, both halves of it
#include "../core/ThemeRegistry.h"
#include "../core/ThemeShots.h"          // the screenshot fetch/cache, shared with the classic browser
#include "../theme2/ThemeEngine.h"
#include "../theme2/ThemedPanelHost.h"   // PanelRow — the row this patches after a removal

#include <QDir>
#include <QNetworkAccessManager>   // the shared docNam_ the screenshot fetch borrows
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

// ---- Preview: applying an installed theme, and putting the old one back (issue #91) --------------------
//
// "Preview" means APPLY. There is no lighter thing it could mean: a theme is a folder of QML and JSON that
// the engine renders the product with, so the only way to show what one looks like is to make it the theme
// — which is exactly what the picker's live preview does (ThemeChoice::setForProfile, then the surface
// re-renders). What makes it a preview rather than a choice is that leaving restores the previous value.
//
// WHY THE RAW STORED VALUE IS WHAT IS SAVED, not currentThemeFolder(): Appearance shows a RESOLVED folder
// while nothing at all may be stored (ThemeChoice::resolve falls back to the shipped theme). Writing that
// resolution back on the way out would persist a choice the user never made — onto a key that SYNCS to
// their other devices — and would silently answer the forced first-run pick for them. An empty restore is
// written back as empty, which is the same fact as unset everywhere this key is read (forProfile returns
// "" for both, and needsPick then stays true).
void MainWindow::beginThemePreview(const QString& folder)
{
    if (folder.isEmpty()) return;
    const QString profile = ProfileStore::currentId();
    // FIRST preview only: a second Preview press (another theme, or the same one again) must not overwrite
    // the saved value with the theme the first press applied, or leaving would restore a preview.
    if (themePreviewFolder_.isEmpty()) themePreviewRestore_ = ThemeChoice::forProfile(profile);
    themePreviewFolder_ = folder;
    ThemeChoice::setForProfile(profile, folder);

    // Make it visible where the user is standing. On the themed surface the panel itself is theme-rendered,
    // so re-resolving its style block IS the preview landing (the same mechanism apply-on-select uses on
    // Appearance); the full theme — home, browse, detail — lands the way it always does, when the surface is
    // left and showHomeScreen rebuilds from the saved key. On the classic surface the Appearance panel's
    // live preview box is rebuilt by its own re-render, which its caller drives.
    if (themedPanelHost_) themedPanelHost_->setStyle(settingsPanelStyle());
}

void MainWindow::endThemePreview()
{
    if (themePreviewFolder_.isEmpty()) return;             // nothing is being previewed — not an error
    const QString previewed = themePreviewFolder_;
    const QString restore   = themePreviewRestore_;
    themePreviewFolder_.clear();
    themePreviewRestore_.clear();

    const QString profile = ProfileStore::currentId();
    // The one question worth asking on the way out, and ThemeRegistry answers it so it is pinned rather
    // than inferred: is what is stored now still what the preview applied? If the user walked into Theme…
    // while previewing and CHOSE something, that is a decision — restoring over it would silently undo it.
    if (!ThemeRegistry::previewShouldRestore(previewed, ThemeChoice::forProfile(profile))) return;
    ThemeChoice::setForProfile(profile, restore);
    if (themedPanelHost_) themedPanelHost_->setStyle(settingsPanelStyle());
}

// ---- Screenshots: one thumbnail per row (issue #91) ---------------------------------------------------
//
// WHICH url may be fetched is ThemeRegistry's answer (https, the index's own host or one the user added, at
// most four per entry, anything else dropped without a word). WHAT may then be cached and drawn is
// ThemeShots' (2 MB held to as the bytes arrive, and the product's one magic-byte picture rule). Neither
// question is answered here, and neither gallery answers it either — this is the single place the two halves
// are joined, so the classic browser and the themed panel cannot end up with different bounds.
//
// The ROW gets the FIRST admitted screenshot. The rest of the array is what a detail view would show; there
// is no detail view on either gallery today, and fetching pictures nothing can display would spend a TV's
// connection on nothing.
//
// `then` does NOT fire when there is no picture: a row with no thumbnail is the row this product already
// had, and every caller's "patch the row" path would otherwise have to re-derive that it should do nothing.
void MainWindow::fetchThemeShot(const ThemeRegistry::Entry& entry, const QString& indexUrl,
                                std::function<void(const QString&)> then)
{
    if (!then) return;
    const QStringList urls = ThemeRegistry::screenshotUrls(indexUrl, entry, themeExtraRegistryUrls());
    if (urls.isEmpty()) return;
    const QString folder = entry.folder();
    if (folder.isEmpty()) return;
    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    ThemeShots::fetch(docNam_, folder, urls.first(), [then](const QString& path) {
        if (!path.isEmpty()) then(path);
    });
}
