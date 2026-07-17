#include "SettingsDialog.h"
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
#include <QStackedWidget>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <vector>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Emulator Settings — Cores per System"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    // Page 0: the cores-per-system list. The per-core options editor is pushed as page 1 on demand
    // (in-place, no popup window) and removed when the user leaves it.
    auto* mainPage = new QWidget(stack_);
    stack_->addWidget(mainPage);
    auto* v = new QVBoxLayout(mainPage);

    auto* intro = new QLabel(tr("Choose the libretro core for each system, and tune per-core options. "
                                "Controller and keyboard mapping is in “Input Mapping…”."), mainPage);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto* form = new QFormLayout();

    for (const auto& sys : SystemCatalog::systems())
    {
        if (!sys.externalEmulator.isEmpty()) continue; // standalone emulators have no libretro core to pick
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
        if (sys.externalEmulator.isEmpty())
            if (QComboBox* c = combos_.value(sys.id))
                Settings::setCoreFor(sys.id, c->currentData().toString());
    accept();
}

void SettingsDialog::editOptions(const QString& systemId)
{
    QComboBox* combo = combos_.value(systemId);
    if (!combo) return;
    const QString core = combo->currentData().toString();
    status_->hide(); // clear any previous error

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
    const std::vector<CoreOption> opts = tmp.options(); // copy out before unloading
    tmp.unload();

    // Build the options editor as an in-place page (no popup window).
    auto* page = new QWidget(stack_);
    auto* outer = new QVBoxLayout(page);

    auto* header = new QHBoxLayout();
    auto* back = new QPushButton(tr("‹ Back"), page);
    auto* title = new QLabel(tr("<b>%1 — Core Options</b>").arg(core), page);
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
        form->addRow(new QLabel(tr("This core doesn't expose any configurable options.")));
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
        stack_->setCurrentIndex(0);   // back to the cores list
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
