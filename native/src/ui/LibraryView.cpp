#include "LibraryView.h"
#include "AddonSettingsDialog.h"
#include "RegistryBrowser.h"
#include "../addons/AddonManager.h"

#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>

LibraryView::LibraryView(AddonManager* mgr, QWidget* parent) : QWidget(parent), mgr_(mgr)
{
    auto* v = new QVBoxLayout(this);

    auto* tools = new QHBoxLayout();
    auto* homeBtn = new QPushButton(tr("‹ Home"), this);
    connect(homeBtn, &QPushButton::clicked, this, &LibraryView::homeRequested);
    tools->addWidget(homeBtn);
    auto* browse = new QPushButton(tr("Browse Add-ons…"), this);
    auto* install = new QPushButton(tr("Install Addon…"), this);
    auto* configure = new QPushButton(tr("Configure…"), this);
    auto* reload = new QPushButton(tr("Reload"), this);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search the selected source…"));
    auto* searchBtn = new QPushButton(tr("Search"), this);
    connect(browse, &QPushButton::clicked, this, &LibraryView::browseAddons);
    connect(install, &QPushButton::clicked, this, &LibraryView::installAddon);
    connect(configure, &QPushButton::clicked, this, &LibraryView::configureAddon);
    connect(reload, &QPushButton::clicked, this, &LibraryView::reloadAddons);
    connect(searchBtn, &QPushButton::clicked, this, &LibraryView::doSearch);
    connect(search_, &QLineEdit::returnPressed, this, &LibraryView::doSearch);
    tools->addWidget(browse);
    tools->addWidget(install);
    tools->addWidget(configure);
    tools->addWidget(reload);
    tools->addWidget(search_, 1);
    tools->addWidget(searchBtn);
    v->addLayout(tools);

    auto* split = new QSplitter(Qt::Horizontal, this);
    sourceList_ = new QListWidget(split);
    itemList_ = new QListWidget(split);
    split->addWidget(sourceList_);
    split->addWidget(itemList_);
    split->setStretchFactor(1, 1);
    split->setSizes({ 240, 800 });
    v->addWidget(split, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    v->addWidget(status_);

    connect(sourceList_, &QListWidget::currentRowChanged, this, &LibraryView::onSourceChanged);
    connect(sourceList_, &QListWidget::itemChanged, this, &LibraryView::onSourceCheckChanged);
    connect(itemList_, &QListWidget::itemActivated, this, &LibraryView::onItemActivated);
    connect(mgr_, &AddonManager::catalogReady, this, &LibraryView::onCatalogReady);

    refreshSources();
}

void LibraryView::refreshSources()
{
    populating_ = true;
    sourceList_->clear();
    sourceRefs_.clear();
    itemList_->clear();
    currentItems_.clear();

    // All media sources are listed; the checkbox enables/disables each one (persisted).
    for (LoadedAddon* s : mgr_->sources())
    {
        const QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
        auto* item = new QListWidgetItem(name, sourceList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(mgr_->isEnabled(s->manifest.id) ? Qt::Checked : Qt::Unchecked);
        sourceRefs_.push_back(s);
    }
    populating_ = false;

    if (sourceList_->count() > 0)
        sourceList_->setCurrentRow(0);
    else
        status_->setText(tr("No media-source addons installed. Use “Install Addon…” to add one."));
}

void LibraryView::onSourceChanged()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size()) return;

    LoadedAddon* s = sourceRefs_[row];
    if (!mgr_->isEnabled(s->manifest.id))
    {
        itemList_->clear();
        currentItems_.clear();
        pendingReqId_ = -1; // cancel any in-flight result for this pane
        status_->setText(tr("“%1” is disabled — check its box to enable it.")
                             .arg(s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name));
        return;
    }
    status_->setText(tr("Loading…"));
    pendingReqId_ = mgr_->requestCatalog(s, QString(), QString(), 1);
}

void LibraryView::onSourceCheckChanged(QListWidgetItem* item)
{
    if (populating_ || !item) return;
    const int row = sourceList_->row(item);
    if (row < 0 || row >= sourceRefs_.size()) return;
    mgr_->setEnabled(sourceRefs_[row]->manifest.id, item->checkState() == Qt::Checked);
    if (row == sourceList_->currentRow())
        onSourceChanged(); // reflect the new state in the item pane immediately
}

void LibraryView::doSearch()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size()) return;
    const QString q = search_->text().trimmed();
    status_->setText(tr("Loading…"));
    pendingReqId_ = q.isEmpty() ? mgr_->requestCatalog(sourceRefs_[row], QString(), QString(), 1)
                                : mgr_->requestSearch(sourceRefs_[row], q);
}

void LibraryView::onCatalogReady(int requestId, const MediaCatalog& cat)
{
    if (requestId != pendingReqId_) return; // superseded
    showCatalog(cat);
}

void LibraryView::showCatalog(const MediaCatalog& cat)
{
    currentItems_ = cat.items;
    itemList_->clear();
    for (const MediaItem& it : currentItems_)
    {
        QString label = it.title;
        if (!it.subtitle.isEmpty()) label += QStringLiteral("\n    ") + it.subtitle;
        auto* w = new QListWidgetItem(label, itemList_);
        if (!it.type.isEmpty()) w->setToolTip(it.type + QStringLiteral(": ") + it.url);
    }
    status_->setText(cat.title.isEmpty()
                         ? tr("%1 item(s)").arg(currentItems_.size())
                         : tr("%1 — %2 item(s)").arg(cat.title).arg(currentItems_.size()));
}

void LibraryView::onItemActivated()
{
    const int row = itemList_->currentRow();
    if (row >= 0 && row < currentItems_.size())
        emit openItem(currentItems_[row]);
}

void LibraryView::installAddon()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Install Addon"), QString(), tr("Addon packages (*.addon *.zip);;All files (*.*)"));
    if (f.isEmpty()) return;
    QString err;
    if (!mgr_->installPackage(f, &err))
    {
        QMessageBox::warning(this, tr("Install failed"), err);
        return;
    }
    refreshSources();
    status_->setText(tr("Addon installed."));
}

void LibraryView::configureAddon()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size())
    {
        status_->setText(tr("Select a source to configure."));
        return;
    }
    AddonSettingsDialog dlg(sourceRefs_[row]->manifest, this);
    if (dlg.exec() == QDialog::Accepted)
        onSourceChanged(); // re-fetch the catalog so changed config (e.g. an API key) takes effect now
}

void LibraryView::reloadAddons()
{
    mgr_->reload();
    refreshSources();
}

void LibraryView::browseAddons()
{
    RegistryBrowser dlg(RegistryBrowser::Addons, mgr_, this);
    dlg.exec();
    if (dlg.installedSomething()) refreshSources(); // show newly installed add-ons
}
