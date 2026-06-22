#include "SettingsDialog.h"
#include "ControllerRemapDialog.h"
#include "../core/SystemCatalog.h"
#include "../core/Settings.h"
#include "../core/CoreManager.h"
#include "LibretroCore.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QWidget>
#include <QScrollArea>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <vector>

SettingsDialog::SettingsDialog(Gamepad* pad, Keymap* keys, QWidget* parent)
    : QDialog(parent), pad_(pad), keys_(keys)
{
    setWindowTitle(tr("Settings — Cores per System"));

    auto* v = new QVBoxLayout(this);

    auto* controllerBtn = new QPushButton(tr("Input Mapping (Controller + Keyboard)…"), this);
    connect(controllerBtn, &QPushButton::clicked, this, &SettingsDialog::editControllerMapping);
    v->addWidget(controllerBtn);

    auto* form = new QFormLayout();

    for (const auto& sys : SystemCatalog::systems())
    {
        auto* combo = new QComboBox(this);
        for (const QString& core : sys.cores)
        {
            const QString label = CoreManager::isInstalled(core) ? core + tr("  (installed)") : core;
            combo->addItem(label, core); // userData holds the bare core name
        }
        QString chosen = Settings::coreFor(sys.id);
        if (chosen.isEmpty())
            chosen = sys.cores.value(0);
        const int idx = combo->findData(chosen);
        if (idx >= 0)
            combo->setCurrentIndex(idx);

        combos_.insert(sys.id, combo);

        // core dropdown + an "Options…" button that edits that core's own settings.
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(combo, 1);
        auto* optBtn = new QPushButton(tr("Options…"), row);
        const QString sid = sys.id;
        connect(optBtn, &QPushButton::clicked, this, [this, sid] { editOptions(sid); });
        h->addWidget(optBtn);
        form->addRow(sys.name, row);
    }
    v->addLayout(form);

    auto* note = new QLabel(
        tr("The selected core is used automatically when you open a matching game — no prompt. "
           "If it isn't installed, it downloads from the libretro buildbot on first use."),
        this);
    note->setWordWrap(true);
    v->addWidget(note);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(box);
}

void SettingsDialog::save()
{
    for (const auto& sys : SystemCatalog::systems())
        if (QComboBox* c = combos_.value(sys.id))
            Settings::setCoreFor(sys.id, c->currentData().toString());
    accept();
}

void SettingsDialog::editControllerMapping()
{
    ControllerRemapDialog dlg(pad_, keys_, this);
    dlg.exec();
}

void SettingsDialog::editOptions(const QString& systemId)
{
    QComboBox* combo = combos_.value(systemId);
    if (!combo) return;
    const QString core = combo->currentData().toString();

    // Make sure the core is present (download on first use), then load it headlessly to read its options.
    const QString corePath = CoreManager::ensureCore(core, this);
    if (corePath.isEmpty()) return; // cancelled / failed (already reported)

    LibretroCore tmp;
    std::string err;
    if (!tmp.loadCore(corePath.toStdString(), &err))
    {
        QMessageBox::warning(this, tr("Can't read options"),
                             tr("Couldn't load core '%1':\n%2").arg(core, QString::fromStdString(err)));
        return;
    }
    const std::vector<CoreOption> opts = tmp.options(); // copy out before unloading
    tmp.unload();

    if (opts.empty())
    {
        QMessageBox::information(this, tr("No options"),
                                 tr("The core '%1' doesn't expose any configurable options.").arg(core));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("%1 — Core Options").arg(core));
    auto* outer = new QVBoxLayout(&dlg);

    // Cores can expose dozens of options, so make the list scrollable.
    auto* scroll = new QScrollArea(&dlg);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget;
    auto* form = new QFormLayout(inner);

    QHash<QString, QComboBox*> optCombos;
    for (const CoreOption& o : opts)
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

    auto* note = new QLabel(tr("Changes take effect the next time you open a game with this core."), &dlg);
    note->setWordWrap(true);
    outer->addWidget(note);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(box);
    dlg.resize(480, 560);

    if (dlg.exec() == QDialog::Accepted)
        for (auto it = optCombos.constBegin(); it != optCombos.constEnd(); ++it)
            Settings::setOptionValue(core, it.key(), it.value()->currentData().toString());
}
