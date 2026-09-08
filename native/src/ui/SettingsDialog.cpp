#include "SettingsDialog.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "../core/EmulationTarget.h"   // Unified Emulation Picker: engine-tagged run-targets + per-system resolution
#include "LibretroCore.h"
#include "../emu/RetroParkOptions.h"   // Task B3: RetroPark-backed systems' options via live harvest + descriptor cache
#include "../core/CustomCores.h"        // issue #98: the user-tier custom-core registry
#include "../core/CustomCoreInstall.h"  // issue #98: a file, inspected, copied in and registered

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <vector>

namespace {
// Single source of truth for "this BUILD/platform can run RetroPark" — the classic-settings twin of MainWindow's
// kRetroParkBuildAvailable and of GameLauncher's launch-time retroParkAvailable. On a build WITHOUT the runtime
// (Android TV, iOS) the per-system picker must neither OFFER nor DISPLAY RetroPark targets, because prepareCore
// degrades a backend=retropark to the underlying engine — a "(retropark)" label would misrepresent what runs.
// Passed to both emulationTargetsFor (the offered combo items) and resolveEmulationTarget (the current value).
#ifdef EB_HAVE_RETROPARK
constexpr bool kRetroParkBuildAvailable = true;
#else
constexpr bool kRetroParkBuildAvailable = false;
#endif

// Write a chosen run-target as the per-system DEFAULT: the classic twin of MainWindow's setSystemEmulationDefault
// and of applyTargetToOverride — map the engine onto the Settings trio and CLEAR the other two levers so a default
// is one self-consistent unit. The SAME per-system levers resolveEmulationTarget / prepareCore read.
void applySystemEmulationTarget(const QString& sysId, const EmulationTarget& t)
{
    switch (t.engine)
    {
        case EmuEngine::Libretro:
            Settings::setCoreFor(sysId, t.ref);
            Settings::setEmulatorFor(sysId, QString());
            Settings::setBackendFor(sysId, EmuBackend::Libretro);
            break;
        case EmuEngine::RetroPark:
            Settings::setCoreFor(sysId, QString());
            Settings::setEmulatorFor(sysId, QString());
            Settings::setBackendFor(sysId, EmuBackend::RetroPark);
            break;
        case EmuEngine::Standalone:
            Settings::setCoreFor(sysId, QString());
            Settings::setEmulatorFor(sysId, t.ref);
            Settings::setBackendFor(sysId, EmuBackend::Libretro);
            break;
    }
}

// The libretro core the "Options…" button tunes for a system, given the row's current Emulation selection. If the
// selection is a libretro target ("libretro:<core>") use that core; otherwise (Default, RetroPark, standalone) fall
// back to the system's per-system / built-in libretro core, so a core is always resolvable for a libretro system.
QString coreForSelection(const GameSystem* sys, const QString& selTargetId)
{
    if (selTargetId.startsWith(QStringLiteral("libretro:")))
        return selTargetId.mid(QStringLiteral("libretro:").size());
    if (!sys) return QString();
    QString c = Settings::coreFor(sys->id);
    if (c.isEmpty()) c = sys->cores.value(0);
    return c;
}
} // namespace

// Per-system EMULATION picker (Unified Emulation Picker, Task 5) — classic twin of the themed presentEmulatorCorePicker.
// One combo per system whose items are "Default" (clear to the system built-in) + every engine-tagged run-target
// emulationTargetsFor(sys) enumerates; current selection = the resolved per-system target. On Save the row writes the
// per-system trio (setCoreFor/setEmulatorFor/setBackendFor). Libretro systems keep the per-core "Options…" editor.
SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Emulator Settings — Emulation per System"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    // Page 0: the emulation-per-system list. The per-core options editor is pushed as page 1 on demand
    // (in-place, no popup window) and removed when the user leaves it.
    auto* mainPage = new QWidget(stack_);
    stack_->addWidget(mainPage);
    auto* v = new QVBoxLayout(mainPage);

    auto* intro = new QLabel(tr("Choose how each system runs — a libretro core, a standalone emulator, or RetroPark — "
                                "and tune per-core options. Controller and keyboard mapping is in “Input Mapping…”."), mainPage);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto* form = new QFormLayout();

    for (const auto& sys : SystemCatalog::systems())
    {
        auto* combo = new QComboBox(this);
        combo->addItem(tr("Default"), QString());   // item 0: clear to the system built-in (userData "")
        for (const EmulationTarget& t : emulationTargetsFor(&sys, kRetroParkBuildAvailable, kStandaloneBuildAvailable))
            combo->addItem(t.displayName, t.id);     // userData holds the stable target id ("libretro:<core>" / "retropark" / "standalone:<id>")

        // Current selection = the resolved per-system default (no per-game override folded in), matching prepareCore.
        const EmulationTarget cur = resolveEmulationTarget(
            &sys, LaunchOpts::Override{}, Settings::coreFor(sys.id), Settings::emulatorFor(sys.id),
            Settings::backendFor(sys.id), kRetroParkBuildAvailable, kStandaloneBuildAvailable);
        const int idx = combo->findData(cur.id);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);

        combos_.insert(sys.id, combo);

        // emulation dropdown + (libretro systems only) an "Options…" button that edits the selected core's settings.
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(combo, 1);
        // Libretro systems expose per-core options; a PRESENTING RetroPark system (gc) whose per-system default
        // resolves to RetroPark ALSO does (internal resolution / aspect, from the descriptor cache B1 writes on
        // first play — Task B2). A plain standalone system on its external emulator has no such surface.
        const bool retroParkPresentingOpts =
            (cur.engine == EmuEngine::RetroPark) && retroParkSystemIsPresenting(sys.id);
        if (sys.externalEmulator.isEmpty() || retroParkPresentingOpts)
        {
            auto* optBtn = new QPushButton(tr("Options…"), row);
            const QString sid = sys.id;
            connect(optBtn, &QPushButton::clicked, this, [this, sid] { editOptions(sid); });
            h->addWidget(optBtn);
        }
        form->addRow(sys.name, row);
    }
    v->addLayout(form);

    auto* note = new QLabel(
        tr("The selected engine is used automatically when you open a matching game — no prompt. A libretro core "
           "that isn't installed downloads from the libretro buildbot on first use."),
        this);
    note->setWordWrap(true);
    v->addWidget(note);

    // Issue #98, the classic twin of the themed panel's "Custom cores..." row. A core the user supplies shows
    // up in the combos above (candidateCoresFor appends it after every catalogue core, so no default moves);
    // THIS is where it is loaded, listed and removed. Without this button the escape hatch would exist on the
    // themed layout only.
    auto* customBtn = new QPushButton(tr("Custom cores..."), mainPage);
    customBtn->setToolTip(tr("Run a libretro core EverythingBox doesn't ship - one you built or downloaded yourself."));
    connect(customBtn, &QPushButton::clicked, this, &SettingsDialog::editCustomCores);
    v->addWidget(customBtn, 0, Qt::AlignLeft);

    status_ = new QLabel(mainPage);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color:#c0392b;"));
    status_->hide();
    v->addWidget(status_);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, mainPage);
    connect(box, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(box);
}

void SettingsDialog::save()
{
    for (const auto& sys : SystemCatalog::systems())
    {
        QComboBox* c = combos_.value(sys.id);
        if (!c) continue;
        const QString targetId = c->currentData().toString();
        if (targetId.isEmpty())                      // "Default" -> clear the per-system levers to the built-in
        {
            Settings::setCoreFor(sys.id, QString());
            Settings::setEmulatorFor(sys.id, QString());
            Settings::setBackendFor(sys.id, EmuBackend::Libretro);
            continue;
        }
        for (const EmulationTarget& t : emulationTargetsFor(&sys, kRetroParkBuildAvailable, kStandaloneBuildAvailable))
            if (t.id == targetId) { applySystemEmulationTarget(sys.id, t); break; }
    }
    accept();
}

void SettingsDialog::editOptions(const QString& systemId)
{
    QComboBox* combo = combos_.value(systemId);
    if (!combo) return;
    const QString selId = combo->currentData().toString();

    // Task B3 (classic twin of MainWindow::editCoreOptions): when the row's current selection is the RetroPark
    // backend, the options come from the RetroPark runtime, not a headless native libretro load — a late-declaring
    // DRIVEN core (fceumm/NES) sees nothing until content loads. A DRIVEN system live-harvests first, then falls
    // back to the descriptor cache RetroParkView writes on first play (B4). A PRESENTING system (gc, Task B2) is a
    // VULKAN core the D3D11 harvest rejects — and a full headless Dolphin boot is far too heavy just to read a
    // static list — so it sources ONLY from that cache. A no-retropark build can't select "retropark", so this
    // branch is dead there.
    const bool retroParkSource = (selId == QStringLiteral("retropark"));
    const bool retroParkPresenting = retroParkSource && retroParkSystemIsPresenting(systemId);

    // A PRESENTING RetroPark system (gc) runs no libretro core, so coreForSelection returns empty (coreFor/cores[0]
    // both empty). Key its options under the stable systemId instead of the empty string — this matches
    // RetroParkView::openGame's final fallback EXACTLY, so B1's launch-apply, B5's in-game menu and this editor share
    // one keyspace (gc), and two presenting systems never collide on opt//*. Every other selection keeps its
    // resolvable libretro core; a native system with no core still bails below.
    QString core = coreForSelection(SystemCatalog::byId(systemId), selId);
    if (core.isEmpty() && retroParkPresenting) core = systemId;
    if (core.isEmpty() && !retroParkPresenting)
    { status_->setText(tr("No libretro core to configure for this system.")); status_->show(); return; }
    status_->hide(); // clear any previous error

    std::vector<CoreOption> opts;
    if (retroParkSource)
    {
        // DRIVEN systems live-harvest first: shim dir per system, mirroring RetroParkView's load path — N64 ->
        // libretro_shim_n64, every other driven system (today NES) -> libretro_shim. harvest() is a no-op returning
        // {} on a no-retropark build. PRESENTING gc SKIPS the harvest (Vulkan core, D3D11 runtime rejects it) and
        // reads the descriptor cache directly.
        if (!retroParkPresenting)
        {
            const QString subdir = (systemId == QStringLiteral("n64"))
                ? QStringLiteral("libretro_shim_n64") : QStringLiteral("libretro_shim");
            opts = RetroParkOptions::harvest(CoreManager::coresDir() + QStringLiteral("/") + subdir);
        }
        if (opts.empty())
            opts = RetroParkOptions::parse(Settings::coreOptionDescriptors(core).toUtf8());   // cached on first play
    }
    else
    {
        // Make sure the core is present (download on first use), then load it headlessly to read its options.
        // Progress + failures show inline in the status line (no popup).
        QString dlErr;
        const QString corePath = CoreManager::ensureCore(core, &dlErr, [this, core](int pct) {
            status_->setText(tr("Downloading core ‘%1’… %2%").arg(core).arg(pct));
            status_->setStyleSheet(QStringLiteral("color:#555;"));
            status_->show();
        });
        if (corePath.isEmpty())
        {
            status_->setStyleSheet(QStringLiteral("color:#c0392b;"));
            status_->setText(dlErr.isEmpty() ? tr("Couldn't download core ‘%1’.").arg(core) : dlErr);
            status_->show();
            return;
        }
        status_->hide(); // clear the progress line on success

        LibretroCore tmp;
        std::string err;
        if (!tmp.loadCore(corePath.toStdString(), &err))
        {
            status_->setText(tr("Couldn't load core ‘%1’: %2").arg(core, QString::fromStdString(err)));
            status_->show();
            return;
        }
        opts = tmp.options(); // copy out before unloading
        tmp.unload();
    }

    // Build the options editor as an in-place page (no popup window).
    auto* page = new QWidget(stack_);
    auto* outer = new QVBoxLayout(page);

    auto* header = new QHBoxLayout();
    auto* back = new QPushButton(tr("‹ Back"), page);
    // Title uses the core name, or the system name for a PRESENTING system whose core name is empty (gc) so the
    // header never reads " — Core Options". Persistence still uses the (possibly empty) `core`.
    const GameSystem* titleSys = SystemCatalog::byId(systemId);
    const QString titleName = (retroParkPresenting && titleSys) ? titleSys->name : core;
    auto* title = new QLabel(tr("<b>%1 — Core Options</b>").arg(titleName), page);
    header->addWidget(back);
    header->addSpacing(8);
    header->addWidget(title, 1);
    outer->addLayout(header);

    // Cores can expose dozens of options, so make the list scrollable.
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget;
    auto* form = new QFormLayout(inner);

    QHash<QString, QComboBox*> optCombos;
    if (opts.empty())
    {
        form->addRow(new QLabel(retroParkSource
            ? tr("Launch this system once to configure its options.")
            : tr("This core doesn't expose any configurable options.")));
    }
    else for (const CoreOption& o : opts)
    {
        const QString key = QString::fromStdString(o.key);
        auto* c = new QComboBox(inner);
        for (const auto& vp : o.values)
            c->addItem(QString::fromStdString(vp.second), QString::fromStdString(vp.first)); // label, value
        QString cur = Settings::optionValue(core, key);
        if (cur.isEmpty())
            cur = QString::fromStdString(o.defaultValue);
        const int idx = c->findData(cur);
        if (idx >= 0)
            c->setCurrentIndex(idx);
        if (!o.info.empty())
            c->setToolTip(QString::fromStdString(o.info));
        optCombos.insert(key, c);
        form->addRow(QString::fromStdString(o.desc), c);
    }
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    auto* note = new QLabel(tr("Changes take effect the next time you open a game with this core."), page);
    note->setWordWrap(true);
    outer->addWidget(note);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, page);
    outer->addWidget(box);

    auto leave = [this, page] {
        stack_->setCurrentIndex(0);   // back to the emulation list
        stack_->removeWidget(page);
        page->deleteLater();
    };
    connect(box, &QDialogButtonBox::accepted, this, [this, core, optCombos, leave] {
        for (auto it = optCombos.constBegin(); it != optCombos.constEnd(); ++it)
            Settings::setOptionValue(core, it.key(), it.value()->currentData().toString());
        leave();
    });
    connect(box, &QDialogButtonBox::rejected, this, leave);
    connect(back, &QPushButton::clicked, this, leave);

    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}

// ---- Issue #98: the classic custom-core page ------------------------------------------------------------
// The same content as the themed panel, in the classic idiom: where the cores live, a button to load one, a
// button to pick up anything dropped in that folder, and a group box per registered core with what it opens,
// where its file is, a Run button when it needs no content, and Remove. A transient page pushed onto the same
// stack the per-core options editor uses, so nothing here opens a window.
// One place every custom-core message goes: the page's own line AND page 0's, so it is legible whether the
// user stays on the custom-core page or backs out to the emulation list.
void SettingsDialog::sayCustomCore(const QString& text)
{
    if (customStatus_) { customStatus_->setText(text); customStatus_->setVisible(!text.isEmpty()); }
    if (status_)       { status_->setText(text);       status_->setVisible(!text.isEmpty()); }
}

void SettingsDialog::loadCustomCorePicked()
{
    const QString sfx = CustomCoreInstall::librarySuffix();
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Choose a libretro core"), QString(),
        tr("libretro cores (*%1);;All files (*)").arg(sfx));
    if (file.isEmpty()) return;

    CustomCore rec;
    QString err;
    if (!CustomCoreInstall::loadFromFile(file, &rec, &err))
    {
        sayCustomCore(err.isEmpty() ? tr("Couldn't load %1 as a core.").arg(QFileInfo(file).fileName()) : err);
        return;
    }
    // The warranty, once. The themed builder fires the same pair through MainWindow::notify; the RULE lives in
    // CustomCores (noticeDue / acknowledgeNotice), so the two surfaces cannot disagree about whether the user
    // has already been told.
    QStringList said;
    if (CustomCores::noticeDue(CustomCores::registry()))
    {
        said << CustomCores::noticeText();
        CustomCores::acknowledgeNotice();
    }
    if (!rec.needs.isEmpty()) said << rec.needs;          // advisory, never a refusal
    if (said.isEmpty()) said << tr("Loaded %1.").arg(rec.name);
    const QString message = said.join(QStringLiteral(" "));
    editCustomCores();   // rebuild the page so the new core is listed (this recreates customStatus_)
    sayCustomCore(message);
}

void SettingsDialog::editCustomCores()
{
    // Rebuilt in place: a load or a removal re-enters this function, and the transient page is replaced.
    while (stack_->count() > 1)
    {
        QWidget* old = stack_->widget(1);
        stack_->removeWidget(old);
        old->deleteLater();
    }

    auto* page = new QWidget(stack_);
    auto* outer = new QVBoxLayout(page);

    auto* back = new QPushButton(tr("Back"), page);
    outer->addWidget(back, 0, Qt::AlignLeft);

    auto* intro = new QLabel(
        tr("Cores you supply yourself. EverythingBox doesn't curate these: one can crash, misbehave or corrupt "
           "a save, and that is between you and the core. A custom core never replaces the core we would have "
           "chosen for you - pick it for a system above, or for a single game from that game's Emulation row."),
        page);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* folder = new QLabel(tr("Folder: %1").arg(QDir::toNativeSeparators(CustomCores::customDir())), page);
    folder->setWordWrap(true);
    folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(folder);

    auto* buttons = new QHBoxLayout();
    auto* loadBtn = new QPushButton(tr("Load a core file..."), page);
    connect(loadBtn, &QPushButton::clicked, this, &SettingsDialog::loadCustomCorePicked);
    buttons->addWidget(loadBtn);
    auto* scanBtn = new QPushButton(tr("Pick up cores dropped in that folder"), page);
    connect(scanBtn, &QPushButton::clicked, this, [this] {
        const QStringList found = CustomCoreInstall::unregisteredInCustomDir();
        if (found.isEmpty()) { sayCustomCore(tr("Nothing new in that folder.")); return; }
        int ok = 0;
        QString lastErr;
        for (const QString& f : found)
        {
            QString err;
            if (CustomCoreInstall::loadFromFile(f, nullptr, &err)) ++ok; else lastErr = err;
        }
        QString message;
        if (ok > 0 && CustomCores::noticeDue(CustomCores::registry()))
        {
            message = CustomCores::noticeText();
            CustomCores::acknowledgeNotice();
        }
        else
            message = ok > 0 ? tr("Loaded %1 core(s).").arg(ok)
                             : (lastErr.isEmpty() ? tr("Nothing in that folder loaded as a core.") : lastErr);
        editCustomCores();
        sayCustomCore(message);
    });
    buttons->addWidget(scanBtn);
    buttons->addStretch(1);
    outer->addLayout(buttons);

    customStatus_ = new QLabel(page);
    customStatus_->setWordWrap(true);
    customStatus_->hide();
    outer->addWidget(customStatus_);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget(scroll);
    auto* list = new QVBoxLayout(inner);

    const QList<CustomCore> cores = CustomCores::all();
    if (cores.isEmpty())
        list->addWidget(new QLabel(tr("No custom cores loaded."), inner));
    for (const CustomCore& c : cores)
    {
        auto* box = new QGroupBox(c.version.isEmpty() ? c.name : (c.name + QStringLiteral(" ") + c.version), inner);
        auto* bv = new QVBoxLayout(box);
        auto* opens = new QLabel(
            c.extensions.isEmpty()
                ? (c.supportsNoGame ? tr("Opens: nothing - it runs on its own")
                                    : tr("Opens: it doesn't say"))
                : tr("Opens: .%1").arg(c.extensions.join(QStringLiteral(", ."))), box);
        opens->setWordWrap(true);
        bv->addWidget(opens);
        auto* file = new QLabel(tr("File: %1").arg(QDir::toNativeSeparators(c.path)), box);
        file->setWordWrap(true);
        file->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bv->addWidget(file);
        if (!c.needs.isEmpty())
        {
            auto* needs = new QLabel(c.needs, box);
            needs->setWordWrap(true);
            bv->addWidget(needs);
        }
        auto* row = new QHBoxLayout();
        // A supports_no_game core has no content to be opened from, so this Run button is its ONLY way in.
        // The dialog does not own a launcher, so it ASKS: MainWindow wires runCustomCoreRequested to
        // GameLauncher::runCoreWithoutContent when it constructs the dialog.
        if (c.supportsNoGame)
        {
            auto* runBtn = new QPushButton(tr("Run"), box);
            const QString ref = CustomCores::refFor(c.id);
            const QString name = c.name;
            connect(runBtn, &QPushButton::clicked, this, [this, ref, name] {
                emit runCustomCoreRequested(ref, name);
                accept();
            });
            row->addWidget(runBtn);
        }
        else
            bv->addWidget(new QLabel(tr("To use it: pick it for a system above, or for one game from that "
                                        "game's Emulation row."), box));
        auto* rmBtn = new QPushButton(tr("Remove"), box);
        const QString cid = c.id;
        connect(rmBtn, &QPushButton::clicked, this, [this, cid] {
            // The REGISTRATION, not the file: the copy stays in the folder and the scan can pick it up again.
            // A settings row must never destroy something on disk.
            CustomCores::remove(cid);
            editCustomCores();
            sayCustomCore(tr("Removed."));
        });
        row->addWidget(rmBtn);
        row->addStretch(1);
        bv->addLayout(row);
        list->addWidget(box);
    }
    list->addStretch(1);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    connect(back, &QPushButton::clicked, this, [this] { stack_->setCurrentIndex(0); });

    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}
