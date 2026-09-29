// Cloud Sync ▸ What syncs (issue #27, increments 1 and 2): the MainWindow members that list what Cloud Sync
// carries, in the user's words, and let this device switch most of it off.
//
// The categories, their rules and the switches all live in core (SyncCategories.h, CloudSync, CloudMerge) and
// are pinned by probe_cloudmerge section 46. This file is only the page, on both layouts:
//   * one row per category, in SyncCategories' order;
//   * a switch on the seven a user may turn off; Accounts & sign-ins, Other settings and the device-local
//     settings are listed with what happens to them and no switch (the owner decides the credential policy;
//     "other settings" carries the profile list; device-local never syncs);
//   * the line "These choices apply to this device only.", because they do: the switches live under cloud/.
// Both builders read the same list (syncCategoryRowsFor below), so neither layout can show a category the other
// does not. Nav kit only: a themed PanelRow list and a classic panel of checkboxes, no dialog of any kind.
#include "MainWindow.h"
#include "MainWindowInternal.h"   // panelRow, mwLog

#include <QCheckBox>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../core/CloudMerge.h"
#include "../core/CloudSync.h"
#include "../core/SyncCategories.h"
#ifdef EB_HAVE_QML
#include "../theme2/PanelModel.h"
#include "../theme2/ThemedPanelHost.h"
#endif

namespace {

// One category's row, as both builders draw it.
struct SyncCategoryRow
{
    synccat::Category cat;
    QString id;          // "sync.cat:<id>" — stable; the uitest drive and the themed dispatch both key on it
    QString label;       // the plain-language name
    bool switchable;     // a switch (on/off) rather than a statement
    bool on;             // the switch's state (switchable rows only)
    QString statement;   // what happens to it (the rows without a switch)
};

// The statements first, then the switches — on BOTH layouts, so the two read the same. The order is forced by the
// themed panel: its cursor skips Info rows and scrolls only to keep the CURRENT row in view, so an Info row after
// the last switch would never be scrolled onto the screen of a TV or a desktop.
QVector<SyncCategoryRow> syncCategoryRowsFor()
{
    QVector<SyncCategoryRow> rows;
    for (int pass = 0; pass < 2; ++pass)
    for (synccat::Category c : synccat::kListed)
    {
        if (synccat::hasToggle(c) != (pass == 1)) continue;
        SyncCategoryRow r;
        r.cat = c;
        r.id = QStringLiteral("sync.cat:") + QLatin1String(synccat::id(c));
        r.label = synccat::displayName(c);
        r.switchable = synccat::hasToggle(c);
        r.on = CloudSync::categoryEnabled(c);
        switch (c)
        {
            case synccat::Category::Accounts:
                r.statement = MainWindow::tr("synced (not yet configurable)"); break;
            case synccat::Category::Other:
                r.statement = MainWindow::tr("synced"); break;
            case synccat::Category::DeviceLocal:
                r.statement = MainWindow::tr("never synced"); break;
            default: break;
        }
        rows << r;
    }
    return rows;
}

QString syncCategoriesDeviceNote() { return MainWindow::tr("These choices apply to this device only."); }

// Two lengths of one statement: the themed Info row has a single line for its value (a longer one is elided),
// the classic label wraps.
QString syncCategoriesOffNote(bool brief)
{
    return brief ? MainWindow::tr("This device stops sending and taking it. Nothing is deleted.")
                 : MainWindow::tr("Off means this device stops sending and taking that category; turning it back "
                                  "on takes your other devices' values for it. Nothing is "
                                  "deleted, here or on your other devices.");
}

} // namespace

// Apply one switch from either layout, through the ONE core entry point (CloudMerge::switchCategory), which does
// both documents. It starts no pull and no push (#27 review, finding 1): off relays the category, and on adopts
// the relayed copy for that category alone, so what this device uploads is the same the moment after the switch as
// the moment before. The one follow-up is the merge document's ordinary debounce, because turning a category back
// on can add this device's own rows to the union — the same push any local edit to those stores arms.
void MainWindow::setSyncCategoryFromUi(synccat::Category c, bool on)
{
    if (CloudSync::categoryEnabled(c) == on) return;
    CloudMerge::switchCategory(c, on);
    mwLog(QStringLiteral("cloud sync: category %1 switched %2 on this device")
              .arg(QLatin1String(synccat::id(c)), on ? QStringLiteral("on") : QStringLiteral("off")));
    if (on) scheduleProgressSync();   // no-op when signed out
}

void MainWindow::openCloudSyncCategories()
{
    if (!cloud_) cloud_ = std::make_unique<CloudSync>(this);
    const bool signedIn = cloud_->isSignedIn();
    const QVector<SyncCategoryRow> cats = syncCategoryRowsFor();
#ifdef EB_HAVE_QML
    // Themed: a nested level on the Cloud Sync panel (Back pops to it). Switches are Toggle rows; the rest are
    // Info rows whose value says what happens to them.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        themedPanelHost_->setStyle(settingsPanelStyle());
        QVector<PanelRow> rows;
        // Every Info row goes ABOVE the switches (see syncCategoryRowsFor): the cursor never lands on one, so one
        // below the last switch would never scroll into view.
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("sync.device"); r.label = tr("Scope");
          r.value = syncCategoriesDeviceNote(); rows << r; }
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("sync.offnote"); r.label = tr("Off");
          r.value = syncCategoriesOffNote(/*brief=*/true); rows << r; }
        if (!signedIn)
        { PanelRow r; r.kind = PanelRow::Info; r.id = QStringLiteral("sync.signedout"); r.label = tr("Status");
          r.value = tr("Not signed in — nothing syncs yet. These choices apply once you are."); rows << r; }
        for (const SyncCategoryRow& c : cats)
        {
            PanelRow r; r.id = c.id; r.label = c.label;
            if (c.switchable) { r.kind = PanelRow::Toggle; r.checked = c.on; }
            else              { r.kind = PanelRow::Info;   r.value = c.statement; }
            rows << r;
        }

        auto onAct = [this](const QString& id, const QString& val) {
            for (synccat::Category c : synccat::kListed)
                if (synccat::hasToggle(c) && id == QStringLiteral("sync.cat:") + QLatin1String(synccat::id(c)))
                { setSyncCategoryFromUi(c, val == QStringLiteral("1")); return; }
        };
        auto onBack = [this] { openCloudSync(); };   // defensive root onBack (nested: a pop renders Cloud Sync)
        if (themedPanelIsTop(tr("What syncs")))
            themedPanelHost_->replaceTop(tr("What syncs"), rows, onAct, onBack, /*keepCursor=*/true);
        else
            themedPanelHost_->present(tr("What syncs"), rows, onAct, onBack);
        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }
#endif
    showPanel(tr("What syncs"), [this, cats, signedIn](QVBoxLayout* v) {
        auto* intro = new QLabel(tr("<b>What Cloud Sync carries</b><br>%1").arg(syncCategoriesDeviceNote().toHtmlEscaped()));
        intro->setWordWrap(true); intro->setStyleSheet(QStringLiteral("font-size:14px;"));
        v->addWidget(intro);
        auto* off = new QLabel(syncCategoriesOffNote(/*brief=*/false));
        off->setWordWrap(true); off->setStyleSheet(QStringLiteral("font-size:13px;color:#888;"));
        v->addWidget(off);
        if (!signedIn)
        {
            auto* so = new QLabel(tr("Not signed in — nothing syncs yet. These choices apply once you are."));
            so->setWordWrap(true); so->setStyleSheet(QStringLiteral("font-size:13px;color:#888;"));
            v->addWidget(so);
        }
        for (const SyncCategoryRow& c : cats)
        {
            if (c.switchable)
            {
                // "&&": a button label's lone '&' is a mnemonic marker, and every name here has one in it.
                auto* cb = new QCheckBox(QString(c.label).replace(QLatin1Char('&'), QStringLiteral("&&")));
                cb->setObjectName(c.id);
                cb->setChecked(c.on);
                const synccat::Category cat = c.cat;
                connect(cb, &QCheckBox::toggled, this, [this, cat](bool on) { setSyncCategoryFromUi(cat, on); });
                v->addWidget(cb);
            }
            else
            {
                auto* l = new QLabel(tr("%1: %2").arg(c.label, c.statement));
                l->setObjectName(c.id);
                l->setWordWrap(true); l->setStyleSheet(QStringLiteral("font-size:14px;padding:4px 0;"));
                v->addWidget(l);
            }
        }
    }, [this] { openCloudSync(); });
}
