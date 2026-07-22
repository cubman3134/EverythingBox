#include "ThemedPanelHost.h"
#include "FormFactor.h"
#include "../ui/nav/NavGraph.h"
#include "../ui/nav/Osk.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QVBoxLayout>
#include <QUrl>
#include <QColor>
#include <QSet>

// ---- PanelListModel ----------------------------------------------------------------------------------------

int PanelListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

QVariant PanelListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    if (role == RowDataRole) return rows_[index.row()].toMap();
    return {};
}

QHash<int, QByteArray> PanelListModel::roleNames() const
{
    return { { RowDataRole, QByteArrayLiteral("rowData") } };
}

void PanelListModel::setRows(const QVector<PanelRow>& rows)
{
    beginResetModel();
    rows_ = rows;
    endResetModel();
}

bool PanelListModel::patchRow(const QString& rowId, const PanelRow& row)
{
    for (int i = 0; i < rows_.size(); ++i)
    {
        if (rows_[i].id == rowId)
        {
            rows_[i] = row;
            const QModelIndex mi = index(i);
            emit dataChanged(mi, mi, { RowDataRole });   // in place — no reset (Downloads live ticks)
            return true;
        }
    }
    return false;
}

// ---- ThemedPanelHost ---------------------------------------------------------------------------------------

ThemedPanelHost::ThemedPanelHost(QWidget* parent) : QWidget(parent)
{
    graph_ = new NavGraph(this);
    buildPanelNavGraph(*graph_, 0);   // counts fed live per present()
    model_ = new PanelListModel(this);
    bridge_ = new PanelBridge(model_, this);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    buildView();
    v->addWidget(view_);

    connect(graph_, &NavGraph::activated, this, &ThemedPanelHost::onGraphActivated);
    connect(graph_, &NavGraph::selectionChanged, this, &ThemedPanelHost::onSelectionChanged);
    // Defensive: back() on an empty level stack emits rootBack(). The graph levels and stack_ move in lockstep,
    // so rootBack means NO panel is presented — there is nothing to pop and no onBack to run; the correct
    // behaviour is a silent no-op (MainWindow only routes Back here while a panel is up, so this guards misuse
    // rather than a real flow — without it a stray Back at depth 0 would just fall on the floor anyway, but the
    // explicit connect documents that the host, not the caller, owns that decision).
    connect(graph_, &NavGraph::rootBack, this, [] { /* depth 0: nothing to pop — deliberate no-op */ });
}

// Record the live panelRows cursor into the TOP entry, so a nested present()'s pop can restore the user's place
// (renderTop(restore=true)). A panelBack excursion doesn't overwrite it — only a real row selection does.
void ThemedPanelHost::onSelectionChanged(const QString& zone, int index)
{
    if (zone == QStringLiteral("panelRows") && !stack_.isEmpty())
        stack_.last().lastIndex = index;
}

void ThemedPanelHost::buildView()
{
    if (view_) return;
    view_ = new QQuickWidget(this);
    view_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view_->setClearColor(QColor(QStringLiteral("#0F1216")));
    view_->setFocusPolicy(Qt::StrongFocus);
    // Context properties MUST precede setSource (they resolve as the QML loads). `nav` is the shared selection
    // model; `panel` carries the title/style/model the delegates bind.
    view_->rootContext()->setContextProperty(QStringLiteral("nav"), graph_);
    view_->rootContext()->setContextProperty(QStringLiteral("panel"), bridge_);
    // `form` (subsystem D): the panel scales its rows/fonts + insets the safe area from the form-factor tokens.
    view_->rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
    view_->setSource(QUrl(QStringLiteral("qrc:/theme2/elements/SettingsPanel.qml")));
    // The scene root, for the nav kit (same wire ThemeEngine::buildView adds to the themed home/browse — this
    // host is a SEPARATE QQuickWidget that buildView never touches, so it needs it explicitly). NavOverlay::dismiss
    // reads "mmvQuickRoot" and forceActiveFocus()es it when an overlay (the OSK the onboarding profile / settings
    // TextField rows open) closes back onto this panel. Restoring widget focus alone leaves the QQuickWidget
    // scene's active-focus item dead after the OSK's keyboard grab/release, so SettingsPanel.qml's Keys handler
    // (all D-pad nav) goes deaf — invisible on desktop (widget focus revives the scene there) but a hard input
    // wedge on Android/TV (F1), where the scene focus does not come back without the explicit kick. mmvQuickView
    // mirrors the buildView marker so ThemeEngine::rootItem() also resolves this host.
    view_->setProperty("mmvQuickView", QVariant::fromValue<QObject*>(view_));
    view_->setProperty("mmvQuickRoot", QVariant::fromValue<QObject*>(view_->rootObject()));
}

QWidget* ThemedPanelHost::quickWidget() const { return view_; }

void ThemedPanelHost::setStyle(const QVariantMap& style)
{
    style_ = style;
    bridge_->setStyle(style);
    // Match the QQuickWidget backdrop to the panel background so a resize never flashes the default dark.
    const QString bg = style.value(QStringLiteral("background")).toString();
    if (view_ && !bg.isEmpty()) view_->setClearColor(QColor(bg));
}

bool ThemedPanelHost::selectable(const PanelRow& r)
{
    // Separator/Info are non-interactive status rows the cursor skips (dividers). Action/Toggle/Choice/TextField/
    // LogView are activatable (LogView enters scroll mode — Task 3). Progress rows ARE activatable (Task 4): a
    // Downloads job is a Progress row and activating it opens the per-job action chooser (Pause/Resume/…). A
    // pure-status Progress row (no handler) simply fires an ignored onActivate — benign; Downloads is the only
    // consumer today. Disabled rows are never selectable.
    switch (r.kind)
    {
    case PanelRow::Separator:
    case PanelRow::Info:
        return false;
    default:
        return r.enabled;
    }
}

int ThemedPanelHost::firstSelectableRow(const QVector<PanelRow>& rows) const
{
    for (int i = 0; i < rows.size(); ++i)
        if (selectable(rows[i])) return i;
    return 0;
}

void ThemedPanelHost::present(const QString& title, const QVector<PanelRow>& rows,
                              std::function<void(const QString&, const QString&)> onActivate,
                              std::function<void()> onBack)
{
    stack_.append({ title, rows, std::move(onActivate), std::move(onBack) });
    graph_->pushLevel(QStringLiteral("panel:") + title, [this] { onLevelPopped(); });
    renderTop();
    if (view_) view_->setFocus();
}

void ThemedPanelHost::renderTop(bool restore)
{
    if (stack_.isEmpty()) return;
    const Entry& e = stack_.last();
    // Capture the target cursor NOW, before any graph mutation. Critical for pop-restore: setZoneCount below
    // SHRINKS the active zone when the popped child had more rows than this parent, which snaps the stale child
    // index into the new count and emits selectionChanged — onSelectionChanged then writes that snapped value
    // into THIS entry's lastIndex (the child is already gone, so stack_.last() is us). Reading e.lastIndex after
    // that point would restore the clamped child cursor, not the row the user actually left the parent on. (The
    // probe's 18(d) child was smaller than its parent, so the shrink never fired — this only surfaces live with
    // a child panel longer than its parent, e.g. General(27 rows) popped back to the hub(13).) probe_navqml
    // §18(e) drives THIS host (larger child, interior remembered row) to pin exactly this capture-before-mutate
    // ordering — moving the capture below the setZoneCount/select block turns that assertion red.
    const int target = restore ? e.lastIndex : firstSelectableRow(e.rows);
    // Divider set: the indices the cursor must skip (Separator/Info — Progress rows are SELECTABLE since the
    // Downloads panel activates them for the per-job menu; see selectable()), so a set-index snaps to the
    // nearest activatable row and along-axis stepping hops over them (NavGraph owns what QML's seekSelectable did).
    QSet<int> dividers;
    for (int i = 0; i < e.rows.size(); ++i)
        if (!selectable(e.rows[i])) dividers.insert(i);
    model_->setRows(e.rows);
    graph_->setZoneCount(QStringLiteral("panelRows"), e.rows.size());
    graph_->setUnselectable(QStringLiteral("panelRows"), dividers);
    bridge_->setTitle(e.title);
    // Fresh present(): land on the first selectable row. Pop-restore: land on the entry's REMEMBERED row (the
    // user's place before the nested drill) — select() clamps + divider-snaps it, so a shrunk row list is safe.
    graph_->select(QStringLiteral("panelRows"), target);
}

void ThemedPanelHost::replaceTop(const QString& title, const QVector<PanelRow>& rows,
                                 std::function<void(const QString&, const QString&)> onActivate,
                                 std::function<void()> onBack)
{
    if (stack_.isEmpty()) { present(title, rows, std::move(onActivate), std::move(onBack)); return; }
    Entry& e = stack_.last();
    e.title = title;
    e.rows = rows;
    e.onActivate = std::move(onActivate);
    e.onBack = std::move(onBack);
    e.lastIndex = 0;              // a rebuilt row set invalidates the remembered cursor
    renderTop();                 // same level (no pushLevel) — lands on the first selectable row
    if (view_) view_->setFocus();
}

void ThemedPanelHost::updateRow(const QString& rowId, const PanelRow& row)
{
    // Patch EVERY stack entry that carries this row id — a BACKGROUNDED panel (a parent under a nested child)
    // must keep receiving live updates (Downloads progress ticks while a child dialog is up), so its pop-restore
    // renders fresh data. The model_ only mirrors the TOP entry, so it is patched (in place, no reset) exactly
    // when the top holds the row; a backgrounded patch reaches the screen at renderTop on pop.
    for (Entry& e : stack_)
        for (PanelRow& r : e.rows)
            if (r.id == rowId) { r = row; break; }
    model_->patchRow(rowId, row);   // no-op (false) when the row isn't in the top panel's model
}

void ThemedPanelHost::reset()
{
    // Hard clear: drop every stacked panel + its graph level, running NO onBack (a fresh root presentation or a
    // leave to Home owns the navigation). popLevelSilent keeps the graph's level mirror in lockstep.
    while (!stack_.isEmpty())
    {
        stack_.removeLast();
        graph_->popLevelSilent();
    }
}

void ThemedPanelHost::handleBack() { graph_->back(); }   // pop one level -> onLevelPopped (parent, or root exit)

void ThemedPanelHost::onLevelPopped()
{
    if (stack_.isEmpty()) return;
    const Entry gone = stack_.takeLast();
    if (!stack_.isEmpty())
        renderTop(/*restore=*/true);   // back to the parent panel, cursor on the row the user left it at
    else if (gone.onBack)
        gone.onBack();             // the last panel was dismissed — the root onBack leaves the host
}

bool ThemedPanelHost::overlayAbove() const
{
    // Every panel present() pushes exactly one graph level, so the graph's panel-levels always equal stack_.size();
    // an OSK / NavMenu mirrors itself as an EXTRA "overlay" level (Osk::getText / NavOverlay::setNavGraph). Any
    // excess depth is therefore an overlay sitting above the top panel.
    return graph_ && graph_->levelDepth() > stack_.size();
}

void ThemedPanelHost::onGraphActivated(const QString& zone, int index)
{
    if (zone == QStringLiteral("panelBack")) { graph_->back(); return; }
    if (zone != QStringLiteral("panelRows") || stack_.isEmpty()) return;

    Entry& e = stack_.last();
    if (index < 0 || index >= e.rows.size()) return;
    PanelRow& r = e.rows[index];
    if (!selectable(r)) return;

    // RE-ENTRANCY SAFETY (host-local). An onActivate body may synchronously — or, under a blocking OSK / QFileDialog
    // nested loop, ASYNCHRONOUSLY — rebuild THIS panel via present()/replaceTop(). Shipping actors: openLibrary's
    // lib.install / lib.reload branches call openLibrary() (→ replaceTop) then keep running (setAddonsStatus); the
    // Emulator "Change folder…" flow takes an async replaceTop under its QFileDialog; RetroAchievements' async
    // loginResult → openRetroAchievements() → replaceTop can fire while a TextField OSK is open ("Signing in…").
    // replaceTop move-assigns e.onActivate (destroying the closure WHILE it executes — UB) and reassigns e.rows
    // (dangling `r`, and any reference held across the OSK's nested loop — the UAF the review found). Defences:
    //   * dispatch through a BY-VALUE copy of the handler — the entry's copy is then free to be replaced mid-call;
    //   * snapshot the row id, and never touch `r` / `e` after the handler (or after the OSK loop) returns;
    //   * TextField: after the OSK, RE-LOCATE the row by id in the CURRENT top entry (the panel may have been
    //     rebuilt) before committing — if it's gone, drop the commit safely.
    const ActivateFn fn = e.onActivate;
    const QString rowId = r.id;

    switch (r.kind)
    {
    case PanelRow::Action:
    case PanelRow::Progress:   // a Downloads job row: activation opens the per-job action chooser (host-driven)
        if (fn) fn(rowId, QString());   // NB: `r`/`e` may be dangling after this — do not touch them below
        break;
    case PanelRow::Toggle:
    {
        r.checked = !r.checked;
        const bool on = r.checked;      // read the new state BEFORE dispatch (fn may reassign e.rows)
        model_->patchRow(rowId, r);
        if (fn) fn(rowId, on ? QStringLiteral("1") : QStringLiteral("0"));
        break;
    }
    case PanelRow::Choice:
    {
        // externalEdit-style cycle: advance to the next option (wrap), commit, fire onActivate with the pick.
        if (!r.options.isEmpty())
        {
            int cur = r.options.indexOf(r.value);
            if (cur < 0) cur = 0;
            r.value = r.options[(cur + 1) % r.options.size()];
            const QString picked = r.value;   // snapshot before dispatch
            model_->patchRow(rowId, r);
            if (fn) fn(rowId, picked);
        }
        break;
    }
    case PanelRow::TextField:
    {
        // externalEdit contract: the HOST runs the editor in a BLOCKING nested loop (Osk::getText). Mirror the OSK
        // on THIS graph so Back inside it closes the OSK only (and its close revives the panel's cursor through the
        // graph's one handler). A masked row (credentials) masks during EDITING too — the OSK honors the echo mode.
        // Snapshot the edit inputs first: a REFERENCE into e.rows must NOT survive the loop (an async replaceTop can
        // free this row's buffer mid-edit — the UAF the review found), so `r` is untouched from here on.
        const QString label = r.label, initial = r.value;
        const QLineEdit::EchoMode echo = r.masked ? QLineEdit::Password : QLineEdit::Normal;
        const QString t = Osk::getText(label, initial, echo, window(), graph_);
        if (!t.isNull())
        {
            // Re-locate the row by id in the CURRENT top entry (the panel may have been rebuilt during the OSK).
            // If it's gone, drop the commit AND the dispatch — never write through a stale row / fire a stale edit.
            bool committed = false;
            if (!stack_.isEmpty())
                for (PanelRow& er : stack_.last().rows)
                    if (er.id == rowId) { er.value = t; model_->patchRow(rowId, er); committed = true; break; }
            if (committed && fn) fn(rowId, t);   // fire the handler captured when the edit began
        }
        break;
    }
    case PanelRow::LogView:      // scroll mode (Task 3) — no commit dispatch yet
    case PanelRow::Info:
    case PanelRow::Separator:
        break;
    }
}

QString ThemedPanelHost::panelTitle() const { return stack_.isEmpty() ? QString() : stack_.last().title; }
int ThemedPanelHost::levelDepth() const { return stack_.size(); }

QString ThemedPanelHost::focusedRowLabel() const
{
    if (graph_->zone() == QStringLiteral("panelBack")) return QStringLiteral("‹ Back");
    if (stack_.isEmpty()) return QString();
    const QVector<PanelRow>& rows = stack_.last().rows;
    const int i = graph_->index();
    return (i >= 0 && i < rows.size()) ? rows[i].label : QString();
}
