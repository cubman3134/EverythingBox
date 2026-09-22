// Custom cores (issue #98), the MainWindow half — a SEPARATE translation unit that defines MainWindow's #98
// members, for MainWindowPlayOn.cpp's reason: MainWindow.cpp is the busiest merge surface in the repository
// and this feature is self-contained, so it costs that file two short insertions instead of two hundred lines.
//
// THE SURFACE. Settings ▸ Emulator Settings ▸ "Custom cores…" opens a panel that is, deliberately, almost
// nothing: where custom cores live, a button to load one, a button to pick up anything dropped in that folder,
// and then one block per registered core — what it calls itself, what it opens, where its file is, a Run entry
// if it needs no content, and Remove. Everything else about a custom core is reached through the surfaces that
// already exist: it appears in the per-system Emulation picker and in a game's own Emulation row, because
// candidateCoresFor() puts it there.
//
// WHAT THE USER IS TOLD, AND WHEN. Once, on the first core that loads: the warranty (CustomCores::noticeText).
// Never again — a second core, a re-load, a restart, all silent. If the core asked for something we do not
// provide, that sentence is shown too, as information: the load is not refused for it. And if the file cannot
// be hosted at all, the sentence from CoreInspect is shown and nothing is written.
//
// THE "ALL CORES" BROWSER (#98 increments 2-3) hangs off this panel: every core on the libretro buildbot that
// the catalogue doesn't already ship, searchable, with Install — and Update for a core it installed whose index
// date has moved on. All of the deciding lives in BuildbotIndex (what is listed, what is refused, what is an
// update) and BuildbotInstall (the host rule, the download, inspect-before-register, the atomic swap); this
// file only lists rows and says what happened. An installed core is announced through the SAME announceCustomCore
// a hand-loaded one is, so the one-time notice and the capability sentence cannot diverge between the two.
#include "MainWindow.h"
#include "FeedbackPolicy.h"   // kFeedbackLong — the error/notice duration policy

#include <QDir>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>
#include <QFileDialog>
#include <QFileInfo>

#include "../core/BuildbotIndex.h"
#include "../core/BuildbotInstall.h"
#include "../core/CustomCoreInstall.h"
#include "../core/CustomCores.h"
#include "../launch/GameLauncher.h"
#include "../theme2/PanelModel.h"
#include "../theme2/ThemedPanelHost.h"
#include "nav/NavGraph.h"

// Load one file as a custom core and say what happened. Returns true when it registered. THE one place the
// one-time notice fires, so the themed panel, the folder scan and any later caller cannot each grow their own
// idea of when the user has been told.
bool MainWindow::registerCustomCore(const QString& file)
{
    CustomCore rec;
    QString err;
    if (!CustomCoreInstall::loadFromFile(file, &rec, &err))
    {
        notify(err.isEmpty() ? tr("Couldn't load %1 as a core.").arg(QFileInfo(file).fileName()) : err,
               kFeedbackLong);
        return false;
    }

    announceCustomCore(rec, tr("Loaded %1.").arg(rec.name));
    return true;
}

// What the user is told about a core that has just been registered — by hand or from the "All cores" browser.
void MainWindow::announceCustomCore(const CustomCore& rec, const QString& done)
{
    // ONE message, because the notifier is ONE label: two notify() calls in a row show only the second, which is
    // how the warranty used to vanish under "Loaded X." the instant it appeared (found live on the #98 browser).
    QStringList said;
    // The warranty, stated once and then never again (issue #98's third decision). FIRST, so the first thing the
    // user reads about their first custom core is whose it is.
    const bool warranty = CustomCores::noticeDue(CustomCores::registry());
    if (warranty)
    {
        said << CustomCores::noticeText();
        CustomCores::acknowledgeNotice();
    }
    // Advisory, never a refusal: the core asked for something this frontend has no runtime for. It may well
    // run anyway on its own fallback path, and saying so is more use than pretending we did not notice.
    said << (rec.needs.isEmpty() ? done : rec.needs);
    if (warranty || !rec.needs.isEmpty())
        notify(said.join(QStringLiteral(" ")), kFeedbackLong);
    else
        notify(done);
}

// Settings ▸ … ▸ Custom cores ▸ "Load a core file…". A native file dialog, the documented exception to the
// nav-kit rule (the same one the ROMs folder, the add-on installer and the user-emulator binary picker take).
void MainWindow::loadCustomCoreFromPicker()
{
    const QString sfx = CustomCoreInstall::librarySuffix();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose a libretro core"), QString(),
        tr("libretro cores (*%1);;All files (*)").arg(sfx));
    if (file.isEmpty()) return;
    if (registerCustomCore(file) && themedHomeEnabled() && themedPanelHost_)
        presentCustomCores();     // rebuild so the new core's block is on screen
}

#ifdef EB_HAVE_QML
// The themed panel. A hub GRANDCHILD (Settings ▸ Emulator Settings ▸ here), so Back returns to the core
// picker, matching the nesting the rest of the settings tree uses.
void MainWindow::presentCustomCores()
{
    themedPanelHost_->setStyle(settingsPanelStyle());

    const QList<CustomCore> cores = CustomCores::all();
    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.intro");
      r.label = tr("Cores you supply"); r.enabled = false;
      r.value = cores.isEmpty() ? tr("None loaded — EverythingBox uses its own for every system")
                                : tr("%n loaded", "", int(cores.size()));
      rows << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.folder");
      r.label = tr("Folder"); r.value = QDir::toNativeSeparators(CustomCores::customDir());
      r.enabled = false; rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.load");
      r.label = tr("Load a core file…"); rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.scan");
      r.label = tr("Pick up cores dropped in that folder"); rows << r; }
    { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.browse");
      r.label = tr("All cores (libretro buildbot)…"); rows << r; }

    for (const CustomCore& c : cores)
    {
        { PanelRow r; r.kind = PanelRow::Separator; r.id = QStringLiteral("cc.sep:") + c.id;
          r.label = c.version.isEmpty() ? c.name : (c.name + QStringLiteral(" ") + c.version); rows << r; }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.opens:") + c.id;
          r.label = tr("Opens"); r.enabled = false;
          r.value = c.extensions.isEmpty()
                        ? (c.supportsNoGame ? tr("Nothing — it runs on its own") : tr("It doesn't say"))
                        : QStringLiteral(".") + c.extensions.join(QStringLiteral(", ."));
          rows << r; }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.file:") + c.id;
          r.label = tr("File"); r.value = QDir::toNativeSeparators(c.path); r.enabled = false; rows << r; }
        if (c.source == CustomCores::sourceBuildbot())
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.from:") + c.id;
          r.label = tr("From"); r.value = tr("the libretro buildbot, %1").arg(c.sourceDate); r.enabled = false;
          rows << r; }
        if (!c.needs.isEmpty())
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.needs:") + c.id;
          r.label = tr("Note"); r.value = c.needs; r.enabled = false; rows << r; }
        // A supports_no_game core has no content to be opened from, so this Run entry is its ONLY way in
        // (issue #98's second decision). A content-requiring core deliberately gets none: it is reached by
        // opening a game and choosing it on that game's Emulation row.
        if (c.supportsNoGame)
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.run:") + c.id;
          r.label = tr("Run %1").arg(c.name); rows << r; }
        else
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("cc.how:") + c.id;
          r.label = tr("To use it"); r.enabled = false;
          r.value = tr("Pick it under Emulation, for one game or for a whole system"); rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("cc.remove:") + c.id;
          r.label = tr("Remove %1").arg(c.name); r.destructive = true; rows << r; }
    }

    auto onAct = [this](const QString& id, const QString&) {
        if (id == QStringLiteral("cc.load")) { loadCustomCoreFromPicker(); return; }
        if (id == QStringLiteral("cc.browse")) { presentAllCores(true); return; }
        if (id == QStringLiteral("cc.scan"))
        {
            const QStringList found = CustomCoreInstall::unregisteredInCustomDir();
            if (found.isEmpty()) { notify(tr("Nothing new in that folder.")); return; }
            int ok = 0;
            for (const QString& f : found) if (registerCustomCore(f)) ++ok;
            if (ok == 0) notify(tr("Nothing in that folder loaded as a core."), kFeedbackLong);
            presentCustomCores();
            return;
        }
        if (id.startsWith(QStringLiteral("cc.run:")))
        {
            const CustomCore* c = CustomCores::byRef(CustomCores::refFor(id.mid(7)));
            if (c && launcher_) launcher_->runCoreWithoutContent(CustomCores::refFor(c->id), c->name);
            return;
        }
        if (id.startsWith(QStringLiteral("cc.remove:")))
        {
            // Removing the REGISTRATION, not the file: the copy stays in the folder, so the scan above can
            // pick it up again. Nothing on disk is destroyed by a settings row.
            const QString cid = id.mid(10);
            if (CustomCores::remove(cid)) notify(tr("Removed."));
            presentCustomCores();
            return;
        }
    };
    auto onBack = [this] { presentEmulatorCorePicker(); };

    // An in-place rebuild keeps the cursor where it was (a load, a scan, a removal, or a core the All cores
    // browser just installed) rather than throwing it back to the first row.
    if (themedPanelIsTop(tr("Custom cores")))
        themedPanelHost_->replaceTop(tr("Custom cores"), rows, onAct, onBack, /*keepCursor=*/true);
    else
        themedPanelHost_->present(tr("Custom cores"), rows, onAct, onBack);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}

namespace {

// The browser's session state. File-static rather than MainWindow members for MainWindowPlayOn.cpp's reason (this
// feature stays off MainWindow.h's busiest lines); there is one MainWindow, and the state is only a cache of the
// last index read plus what the user typed.
struct AllCoresState
{
    bool    loading = false;
    bool    loaded  = false;
    QString error;
    BuildbotIndex::Parsed parsed;
    QString query;
    QSet<QString> busy;          // core names with an install/update in flight
    // The Custom cores panel UNDER this one lists the registered cores, and a nested panel's Back re-renders its
    // parent from the rows it was built with. So an install marks the parent stale, and the first time the
    // parent is back on top it is rebuilt (see watchParent below).
    bool parentStale = false;
    QPointer<NavGraph> watched;  // the panel graph the level watcher is connected to
};
AllCoresState& allCores()
{
    static AllCoresState s;
    return s;
}

// The most rows one page lists. The real index is a few hundred cores; past this the page asks for a search.
constexpr int kAllCoresShown = 300;

} // namespace

// The "All cores" panel: a search field, then every browsable entry (available, or installed as a custom core)
// with its date and state. A grandchild of the Custom cores panel, so Back returns there.
void MainWindow::presentAllCores(bool refetch)
{
    AllCoresState& st = allCores();

    // Rebuild the Custom cores panel when Back lands on it after an install. levelsChanged fires from inside the
    // graph's back() — itself inside a QML emission — so the slot only SCHEDULES the rebuild for the next turn
    // (a queued call, never a nested loop: the #28 rule). Connected once per graph.
    if (NavGraph* g = themedPanelHost_->navGraph(); g && st.watched != g)
    {
        st.watched = g;
        connect(g, &NavGraph::levelsChanged, this, [this](int) {
            if (!allCores().parentStale) return;
            QTimer::singleShot(0, this, [this] {
                if (allCores().parentStale && themedPanelIsTop(tr("Custom cores")))
                {
                    allCores().parentStale = false;
                    presentCustomCores();
                }
            });
        });
    }
    if ((refetch || !st.loaded) && !st.loading)
    {
        st.loading = true;
        st.error.clear();
        BuildbotInstall::fetchIndex(BuildbotInstall::effectivePolicy(), this,
            [this](const BuildbotIndex::Parsed& parsed, const QString& error) {
                AllCoresState& s = allCores();
                s.loading = false;
                s.loaded  = error.isEmpty();
                s.error   = error;
                if (error.isEmpty()) s.parsed = parsed;
                if (themedPanelIsTop(tr("All cores"))) presentAllCores(false);
            });
    }

    themedPanelHost_->setStyle(settingsPanelStyle());
    const BuildbotInstall::FetchPolicy pol = BuildbotInstall::effectivePolicy();
    QVector<PanelRow> rows;
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.from"); r.enabled = false;
      r.label = tr("From"); r.value = pol.base.host(); rows << r; }
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.what"); r.enabled = false;
      r.label = tr("What this is");
      r.value = tr("Cores EverythingBox doesn't ship. One you install here is a custom core: not curated by us.");
      rows << r; }
    { PanelRow r; r.kind = PanelRow::TextField; r.id = QStringLiteral("ac.search"); r.label = tr("Search");
      r.value = st.query; rows << r; }

    if (st.loading && !st.loaded)
    { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.loading"); r.enabled = false;
      r.label = tr("Reading the core list…"); rows << r; }
    if (!st.error.isEmpty())
    {
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.error"); r.enabled = false;
          r.label = tr("Couldn't read it"); r.value = st.error; rows << r; }
        { PanelRow r; r.kind = PanelRow::Action; r.id = QStringLiteral("ac.retry"); r.label = tr("Try again");
          rows << r; }
    }

    if (st.loaded)
    {
        const QList<BuildbotIndex::Row> classified = BuildbotIndex::classify(
            st.parsed.entries, BuildbotIndex::catalogueCoreNames(), CustomCores::all());
        const QList<BuildbotIndex::Row> shown = BuildbotIndex::browsable(classified, st.query);
        int installed = 0, updates = 0;
        for (const BuildbotIndex::Row& row : shown)
        {
            if (row.status == BuildbotIndex::Status::InstalledCustom) ++installed;
            if (row.updateAvailable) ++updates;
        }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.count"); r.enabled = false;
          r.label = st.query.isEmpty() ? tr("On the buildbot") : tr("Matching “%1”").arg(st.query);
          r.value = (shown.size() == 1 ? tr("1 core") : tr("%1 cores").arg(shown.size()))
                    + tr(", %1 installed, %2 with an update").arg(installed).arg(updates);
          rows << r; }
        const int skipped = st.parsed.malformed + st.parsed.refused + st.parsed.dropped;
        if (skipped > 0)
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.skipped"); r.enabled = false;
          r.label = tr("Not listed");
          r.value = tr("%1 lines of the list couldn't be read or named a file EverythingBox won't install")
                        .arg(skipped);
          rows << r; }

        int n = 0;
        for (const BuildbotIndex::Row& row : shown)
        {
            if (n++ >= kAllCoresShown) break;
            const QString date = row.entry.date.toString(QStringLiteral("yyyy-MM-dd"));
            PanelRow r;
            r.kind  = PanelRow::Action;
            r.id    = QStringLiteral("ac.core:") + row.entry.name;
            r.label = row.entry.name;
            if (st.busy.contains(row.entry.name))
                r.value = tr("Installing…");
            else if (row.updateAvailable)
                r.value = tr("Update available · %1").arg(date);
            else if (row.status == BuildbotIndex::Status::InstalledCustom)
                r.value = row.fromBuildbot ? tr("Installed · %1").arg(date) : tr("Installed (loaded by hand)");
            else
                r.value = tr("Install · %1").arg(date);
            rows << r;
        }
        if (shown.size() > kAllCoresShown)
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("ac.more"); r.enabled = false;
          r.label = tr("Showing %1 of %2").arg(kAllCoresShown).arg(shown.size());
          r.value = tr("Search to narrow it down"); rows << r; }
    }

    auto onAct = [this](const QString& id, const QString& value) {
        if (id == QStringLiteral("ac.search")) { allCores().query = value.trimmed(); presentAllCores(false); return; }
        if (id == QStringLiteral("ac.retry"))  { presentAllCores(true); return; }
        if (id.startsWith(QStringLiteral("ac.core:"))) { installFromAllCores(id.mid(8)); return; }
    };
    auto onBack = [this] { presentCustomCores(); };

    if (themedPanelIsTop(tr("All cores")))
        themedPanelHost_->replaceTop(tr("All cores"), rows, onAct, onBack);
    else
        themedPanelHost_->present(tr("All cores"), rows, onAct, onBack);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}

// One browser row activated. Available -> install; "Update available" -> re-fetch and swap; anything else says
// why there is nothing to do. The work is async (the page stays live); the outcome is announced, and the panel
// rebuilt if it is still the one on screen.
void MainWindow::installFromAllCores(const QString& coreName)
{
    AllCoresState& st = allCores();
    if (st.busy.contains(coreName)) { notify(tr("%1 is already being installed.").arg(coreName)); return; }

    const QList<BuildbotIndex::Row> classified = BuildbotIndex::classify(
        st.parsed.entries, BuildbotIndex::catalogueCoreNames(), CustomCores::all());
    const BuildbotIndex::Row* row = nullptr;
    for (const BuildbotIndex::Row& r : classified)
        if (r.entry.name == coreName) { row = &r; break; }
    if (!row || row->status == BuildbotIndex::Status::Catalogue) return;   // never offered, so never installed here
    if (row->status == BuildbotIndex::Status::InstalledCustom && !row->updateAvailable)
    {
        notify(row->fromBuildbot
                   ? tr("%1 is up to date.").arg(coreName)
                   : tr("You loaded %1 by hand, so EverythingBox doesn't update it. Load the new file to update it.")
                         .arg(coreName),
               kFeedbackLong);
        return;
    }

    const bool updating = row->updateAvailable;
    const BuildbotIndex::Entry entry = row->entry;
    st.busy.insert(coreName);
    notify(updating ? tr("Updating %1…").arg(coreName) : tr("Installing %1…").arg(coreName));
    if (themedPanelIsTop(tr("All cores"))) presentAllCores(false);

    BuildbotInstall::install(BuildbotInstall::effectivePolicy(), entry, this, {},
        [this, coreName, updating](bool ok, const CustomCore& rec, const QString& error) {
            allCores().busy.remove(coreName);
            if (ok) allCores().parentStale = true;
            if (ok)
                announceCustomCore(rec, updating ? tr("Updated %1.").arg(rec.name) : tr("Installed %1.").arg(rec.name));
            else
                notify(error.isEmpty() ? tr("Couldn't install %1.").arg(coreName) : error, kFeedbackLong);
            if (themedPanelIsTop(tr("All cores"))) presentAllCores(false);
        });
}
#else
void MainWindow::presentCustomCores() {}
void MainWindow::presentAllCores(bool) {}
void MainWindow::installFromAllCores(const QString&) {}
#endif
