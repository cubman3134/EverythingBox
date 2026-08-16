#include "SettingsDialog.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "../core/EmulationTarget.h"   // Unified Emulation Picker: engine-tagged run-targets + per-system resolution
#include "LibretroCore.h"
#include "../emu/RetroParkOptions.h"   // Task B3: RetroPark-backed systems' options via live harvest + descriptor cache

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
        for (const EmulationTarget& t : emulationTargetsFor(&sys, kRetroParkBuildAvailable))
            combo->addItem(t.displayName, t.id);     // userData holds the stable target id ("libretro:<core>" / "retropark" / "standalone:<id>")

        // Current selection = the resolved per-system default (no per-game override folded in), matching prepareCore.
        const EmulationTarget cur = resolveEmulationTarget(
            &sys, LaunchOpts::Override{}, Settings::coreFor(sys.id), Settings::emulatorFor(sys.id),
            Settings::backendFor(sys.id), kRetroParkBuildAvailable);
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
        for (const EmulationTarget& t : emulationTargetsFor(&sys, kRetroParkBuildAvailable))
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
    const QString titleName = (core.isEmpty() && titleSys) ? titleSys->name : core;
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
