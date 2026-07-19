#include "ThemedPanelHost.h"
#include "../ui/nav/NavGraph.h"
#include "../ui/nav/Osk.h"

#include <QQuickWidget>
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
    view_->setSource(QUrl(QStringLiteral("qrc:/theme2/elements/SettingsPanel.qml")));
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
    // Separator/Info/Progress are non-interactive status rows the cursor skips (dividers). Action/Toggle/
    // Choice/TextField/LogView are activatable (LogView enters scroll mode — Task 3).
    switch (r.kind)
    {
    case PanelRow::Separator:
    case PanelRow::Info:
    case PanelRow::Progress:
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
    // Divider set: the indices the cursor must skip (Separator/Info/Progress), so a set-index snaps to the
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
    graph_->select(QStringLiteral("panelRows"),
                   restore ? e.lastIndex : firstSelectableRow(e.rows));
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

void ThemedPanelHost::onGraphActivated(const QString& zone, int index)
{
    if (zone == QStringLiteral("panelBack")) { graph_->back(); return; }
    if (zone != QStringLiteral("panelRows") || stack_.isEmpty()) return;

    Entry& e = stack_.last();
    if (index < 0 || index >= e.rows.size()) return;
    PanelRow& r = e.rows[index];
    if (!selectable(r)) return;

    switch (r.kind)
    {
    case PanelRow::Action:
        if (e.onActivate) e.onActivate(r.id, QString());
        break;
    case PanelRow::Toggle:
        r.checked = !r.checked;
        model_->patchRow(r.id, r);
        if (e.onActivate) e.onActivate(r.id, r.checked ? QStringLiteral("1") : QStringLiteral("0"));
        break;
    case PanelRow::Choice:
    {
        // externalEdit-style cycle: advance to the next option (wrap), commit, fire onActivate with the pick.
        if (!r.options.isEmpty())
        {
            int cur = r.options.indexOf(r.value);
            if (cur < 0) cur = 0;
            r.value = r.options[(cur + 1) % r.options.size()];
            model_->patchRow(r.id, r);
            if (e.onActivate) e.onActivate(r.id, r.value);
        }
        break;
    }
    case PanelRow::TextField:
    {
        // externalEdit contract: the HOST runs the editor. Mirror the OSK on THIS graph so Back inside it closes
        // the OSK only (and its close revives the panel's cursor through the graph's one handler).
        const QString t = Osk::getText(r.label, r.value, QLineEdit::Normal, window(), graph_);
        if (!t.isNull())
        {
            r.value = t;
            model_->patchRow(r.id, r);
            if (e.onActivate) e.onActivate(r.id, t);
        }
        break;
    }
    case PanelRow::LogView:      // scroll mode (Task 3) — no commit dispatch yet
    case PanelRow::Info:
    case PanelRow::Progress:
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
