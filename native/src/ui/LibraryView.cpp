#include "LibraryView.h"
#include "AddonSettingsDialog.h"
#include "RegistryBrowser.h"
#include "../addons/AddonManager.h"
#include "../core/RomLibrary.h"

#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>

LibraryView::LibraryView(AddonManager* mgr, QWidget* parent) : QWidget(parent), mgr_(mgr)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    // Page 0: the library UI. Browse / Configure / Add-by-URL / Remove are pushed as sub-pages in place.
    auto* mainPage = new QWidget(stack_);
    stack_->addWidget(mainPage);
    auto* v = new QVBoxLayout(mainPage);

    auto* tools = new QHBoxLayout();
    auto* homeBtn = new QPushButton(tr("‹ Home"), mainPage);
    connect(homeBtn, &QPushButton::clicked, this, &LibraryView::homeRequested);
    tools->addWidget(homeBtn);
    auto* browse = new QPushButton(tr("Browse Add-ons…"), this);
    auto* install = new QPushButton(tr("Install Addon…"), this);
    auto* addUrl = new QPushButton(tr("Add by URL…"), this);
    auto* configure = new QPushButton(tr("Configure…"), this);
    auto* removeBtn = new QPushButton(tr("Remove"), this);
    auto* reload = new QPushButton(tr("Reload"), this);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search the selected source…"));
    auto* searchBtn = new QPushButton(tr("Search"), this);
    connect(browse, &QPushButton::clicked, this, &LibraryView::browseAddons);
    connect(install, &QPushButton::clicked, this, &LibraryView::installAddon);
    connect(addUrl, &QPushButton::clicked, this, &LibraryView::addByUrl);
    connect(configure, &QPushButton::clicked, this, &LibraryView::configureAddon);
    connect(removeBtn, &QPushButton::clicked, this, &LibraryView::removeSelected);
    connect(reload, &QPushButton::clicked, this, &LibraryView::reloadAddons);
    connect(searchBtn, &QPushButton::clicked, this, &LibraryView::doSearch);
    connect(search_, &QLineEdit::returnPressed, this, &LibraryView::doSearch);
    // A remote (HTTP) addon's manifest fetch is async; report its outcome + refresh the list.
    connect(mgr_, &AddonManager::remoteSourceResult, this, [this](bool ok, const QString& msg) {
        status_->setText(msg);
        if (ok) refreshSources();
    });
    tools->addWidget(browse);
    tools->addWidget(install);
    tools->addWidget(addUrl);
    tools->addWidget(configure);
    tools->addWidget(removeBtn);
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

void LibraryView::pushPage(QWidget* page)
{
    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}

void LibraryView::popPage(QWidget* page)
{
    stack_->setCurrentIndex(0); // back to the library list
    stack_->removeWidget(page);
    page->deleteLater();
}

void LibraryView::showDialogPage(QDialog* dlg, const std::function<void(int)>& onFinished)
{
    dlg->setWindowFlags(Qt::Widget); // render inline instead of as a separate window
    // Queued: the handler removes/deletes the dialog, so it must run after QDialog::done() returns.
    connect(dlg, &QDialog::finished, this, [this, dlg, onFinished](int result) {
        onFinished(result);
        popPage(dlg);
    }, Qt::QueuedConnection);
    pushPage(dlg);
}

void LibraryView::refreshSources()
{
    populating_ = true;
    sourceList_->clear();
    sourceRefs_.clear();
    itemList_->clear();
    currentItems_.clear();
    localMode_ = false;

    // First entry: the local ROM library (RetroBat / ES-DE layout). Not an addon, so its ref is null and it
    // has no enable/disable checkbox; selecting it drills systems -> games (see onSourceChanged).
    { auto* local = new QListWidgetItem(tr("🎮  Local ROMs"), sourceList_); local->setToolTip(RomLibrary::root());
      sourceRefs_.push_back(nullptr); }

    // All media sources are listed; the checkbox enables/disables each one (persisted).
    for (LoadedAddon* s : mgr_->sources())
    {
        QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
        if (s->transport == LoadedAddon::RemoteHttp) name += tr("  (remote)"); // distinguish URL-based sources
        auto* item = new QListWidgetItem(name, sourceList_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(mgr_->isEnabled(s->manifest.id) ? Qt::Checked : Qt::Unchecked);
        sourceRefs_.push_back(s);
    }
    populating_ = false;

    // Default to the first add-on (row 0 is the synthetic "Local ROMs" entry); selecting Local ROMs is what
    // creates/scans the ROM tree, so we don't do that work unless the user asks for it.
    if (sourceList_->count() > 1)
        sourceList_->setCurrentRow(1);
    else
        sourceList_->setCurrentRow(0); // only Local ROMs is present
}

void LibraryView::onSourceChanged()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size()) return;

    LoadedAddon* s = sourceRefs_[row];
    if (!s) { pendingReqId_ = -1; showLocalSystems(); return; } // the synthetic "Local ROMs" source
    localMode_ = false;
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
    if (row < 0 || row >= sourceRefs_.size() || !sourceRefs_[row]) return; // skip the local-ROMs pseudo-source
    mgr_->setEnabled(sourceRefs_[row]->manifest.id, item->checkState() == Qt::Checked);
    if (row == sourceList_->currentRow())
        onSourceChanged(); // reflect the new state in the item pane immediately
}

void LibraryView::doSearch()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size()) return;
    const QString q = search_->text().trimmed();

    // Local ROMs: filter the whole library by title (empty query -> back to the systems list).
    if (!sourceRefs_[row])
    {
        if (q.isEmpty()) { showLocalSystems(); return; }
        if (localGroups_.isEmpty()) localGroups_ = RomLibrary::scan();
        MediaCatalog cat;
        cat.title = tr("Search “%1”").arg(q);
        for (const RomLibrary::SystemGroup& g : localGroups_)
            for (const RomLibrary::Rom& r : g.roms)
                if (r.title.contains(q, Qt::CaseInsensitive))
                {
                    MediaItem it;
                    it.type = QStringLiteral("game");
                    it.url = r.path; it.title = r.title; it.subtitle = r.systemName;
                    it.systemHint = r.systemId; it.id = QStringLiteral("romlib:") + r.path;
                    cat.items.push_back(it);
                }
        localMode_ = true; localLevel_ = 1; // results are games -> Enter launches
        showCatalog(cat);
        status_->setText(tr("%n match(es).", "", cat.items.size()));
        return;
    }

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
    if (row < 0 || row >= currentItems_.size()) return;
    const MediaItem it = currentItems_[row];
    if (localMode_)
    {
        if (it.type == QStringLiteral("__back__")) { showLocalSystems(); return; } // "‹ All systems"
        if (localLevel_ == 0) { showLocalGames(it.id); return; }                    // a system row -> its games
        emit openItem(it);                                                          // a game -> launch
        return;
    }
    emit openItem(it);
}

// Scan the ROM library and list the systems that have games (RetroBat / ES-DE <root>/<system>/ layout). The
// tree is created first so it's ready to drop ROMs into even on a fresh install.
void LibraryView::showLocalSystems()
{
    localMode_ = true; localLevel_ = 0; localSystemId_.clear();
    RomLibrary::ensureStructure();
    localGroups_ = RomLibrary::scan();

    MediaCatalog cat;
    cat.title = tr("Local ROMs");
    for (const RomLibrary::SystemGroup& g : localGroups_)
    {
        MediaItem it;
        it.type = QStringLiteral("platform");
        it.id = g.systemId;                 // onItemActivated drills into this system
        it.title = g.systemName;
        it.subtitle = tr("%n game(s)", "", g.roms.size());
        it.expandable = true;
        cat.items.push_back(it);
    }
    showCatalog(cat);
    if (localGroups_.isEmpty())
        status_->setText(tr("No ROMs found under %1 — drop games into its system sub-folders "
                            "(RetroBat / ES-DE layout), then Reload.").arg(RomLibrary::root()));
    else
        status_->setText(tr("Local ROMs — %n system(s). Open one to see its games.", "", localGroups_.size()));
}

void LibraryView::showLocalGames(const QString& systemId)
{
    localMode_ = true; localLevel_ = 1; localSystemId_ = systemId;
    const RomLibrary::SystemGroup* grp = nullptr;
    for (const RomLibrary::SystemGroup& g : localGroups_) if (g.systemId == systemId) { grp = &g; break; }

    MediaCatalog cat;
    MediaItem back; back.type = QStringLiteral("__back__"); back.title = tr("‹ All systems");
    cat.items.push_back(back);
    if (grp)
    {
        cat.title = grp->systemName;
        for (const RomLibrary::Rom& r : grp->roms)
        {
            MediaItem it;
            it.type = QStringLiteral("game");
            it.url = r.path;
            it.title = r.title;
            it.systemHint = r.systemId;                        // routes to the right core/emulator on launch
            it.id = QStringLiteral("romlib:") + r.path;         // stable key for Recent / play stats
            cat.items.push_back(it);
        }
    }
    showCatalog(cat);
    status_->setText(grp ? tr("%1 — %n game(s). Enter to play.", "", grp->roms.size()).arg(grp->systemName)
                         : tr("No games."));
}

void LibraryView::installAddon()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Install Addon"), QString(), tr("Addon packages (*.addon *.zip);;All files (*.*)"));
    if (f.isEmpty()) return;
    QString err;
    if (!mgr_->installPackage(f, &err))
    {
        status_->setText(tr("Install failed: %1").arg(err));
        return;
    }
    refreshSources();
    status_->setText(tr("Addon installed."));
}

void LibraryView::addByUrl()
{
    // Inline form (no popup) for the addon URL.
    auto* page = new QWidget(stack_);
    auto* pv = new QVBoxLayout(page);
    pv->addWidget(new QLabel(tr("<b>Add add-on by URL</b>"), page));
    pv->addWidget(new QLabel(tr("Addon URL (its manifest.json or base URL):"), page));
    auto* edit = new QLineEdit(page);
    edit->setPlaceholderText(tr("https://…/manifest.json"));
    pv->addWidget(edit);
    pv->addStretch(1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, page);
    pv->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, [this, edit, page] {
        const QString url = edit->text().trimmed();
        if (!url.isEmpty()) { status_->setText(tr("Fetching addon…")); mgr_->addRemoteSource(url); } // async
        popPage(page);
    });
    connect(box, &QDialogButtonBox::rejected, this, [this, page] { popPage(page); });
    pushPage(page);
    edit->setFocus();
}

void LibraryView::removeSelected()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size() || !sourceRefs_[row]) { status_->setText(tr("Select an add-on to remove.")); return; }
    LoadedAddon* s = sourceRefs_[row];
    const QString name = s->manifest.name.isEmpty() ? s->manifest.id : s->manifest.name;
    const bool remote = (s->transport == LoadedAddon::RemoteHttp);

    // Inline confirm page (no popup).
    auto* page = new QWidget(stack_);
    auto* pv = new QVBoxLayout(page);
    pv->addWidget(new QLabel(tr("<b>Remove add-on</b>"), page));
    auto* msg = new QLabel(remote ? tr("Remove the remote source “%1”? (Only the saved URL is removed.)").arg(name)
                                  : tr("Remove the addon “%1” and delete its files?").arg(name), page);
    msg->setWordWrap(true);
    pv->addWidget(msg);
    pv->addStretch(1);
    auto* box = new QDialogButtonBox(page);
    auto* confirm = box->addButton(tr("Remove"), QDialogButtonBox::DestructiveRole);
    box->addButton(QDialogButtonBox::Cancel);
    pv->addWidget(box);
    connect(confirm, &QPushButton::clicked, this, [this, s, name, remote, page] {
        const bool ok = remote ? mgr_->removeRemoteSource(s->baseUrl) : mgr_->removeAddon(s->manifest.id);
        if (ok) { refreshSources(); status_->setText(tr("Removed “%1”.").arg(name)); }
        else      status_->setText(tr("Couldn't remove “%1”.").arg(name));
        popPage(page);
    });
    connect(box, &QDialogButtonBox::rejected, this, [this, page] { popPage(page); });
    pushPage(page);
}

void LibraryView::configureAddon()
{
    const int row = sourceList_->currentRow();
    if (row < 0 || row >= sourceRefs_.size() || !sourceRefs_[row])
    {
        status_->setText(tr("Select an add-on to configure."));
        return;
    }
    auto* dlg = new AddonSettingsDialog(sourceRefs_[row]->manifest, this);
    showDialogPage(dlg, [this](int result) {
        if (result == QDialog::Accepted)
            onSourceChanged(); // re-fetch the catalog so changed config (e.g. an API key) takes effect now
    });
}

void LibraryView::reloadAddons()
{
    mgr_->reload();
    refreshSources();
}

void LibraryView::browseAddons()
{
    auto* dlg = new RegistryBrowser(RegistryBrowser::Addons, mgr_, this);
    showDialogPage(dlg, [this, dlg](int) {
        if (dlg->installedSomething()) refreshSources(); // show newly installed add-ons
    });
}
