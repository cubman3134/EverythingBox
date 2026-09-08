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
// NO DOWNLOADS. Nothing here fetches anything. The only inputs are a file the user picks and the files in
// <data>/cores/custom. That is the whole of this increment: the "All cores" buildbot browser is separate work.
#include "MainWindow.h"
#include "FeedbackPolicy.h"   // kFeedbackLong — the error/notice duration policy

#include <QDir>
#include <QStackedWidget>
#include <QFileDialog>
#include <QFileInfo>

#include "../core/CustomCoreInstall.h"
#include "../core/CustomCores.h"
#include "../launch/GameLauncher.h"
#include "../theme2/PanelModel.h"
#include "../theme2/ThemedPanelHost.h"

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

    // The warranty, stated once and then never again (issue #98's third decision). Shown BEFORE the
    // per-core notes below so the first thing the user reads about their first custom core is whose it is.
    if (CustomCores::noticeDue(CustomCores::registry()))
    {
        notify(CustomCores::noticeText(), kFeedbackLong);
        CustomCores::acknowledgeNotice();
    }
    // Advisory, never a refusal: the core asked for something this frontend has no runtime for. It may well
    // run anyway on its own fallback path, and saying so is more use than pretending we did not notice.
    if (!rec.needs.isEmpty())
        notify(rec.needs, kFeedbackLong);
    else
        notify(tr("Loaded %1.").arg(rec.name));
    return true;
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

    if (themedPanelIsTop(tr("Custom cores")))
        themedPanelHost_->replaceTop(tr("Custom cores"), rows, onAct, onBack);
    else
        themedPanelHost_->present(tr("Custom cores"), rows, onAct, onBack);
    stack_->setCurrentWidget(themedPanelHost_);
    updateNavForPage();
    updateBackgroundMusic();
}
#else
void MainWindow::presentCustomCores() {}
#endif
