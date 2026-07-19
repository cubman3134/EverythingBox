// ThemedPanelHost — the themed-mode analogue of MainWindow::showPanel. Where the classic settings area stuffs
// QWidgets into a scroll layout, the themed area renders a flat QVector<PanelRow> (PanelModel.h) through ONE
// full-size QQuickWidget (SettingsPanel.qml) driven by the Nav Contract — arrow/controller navigable, styled
// from the active theme's `settingsPanel` block. It is a persistent stack page (like ReaderChromeHost).
//
// It owns:
//   * a NavGraph (buildPanelNavGraph — the ONE shared shape probe_navqml §18 asserts) exposed to the QML as
//     the `nav` context property: a Vertical `panelRows` zone (one index per row) + a `panelBack` header zone;
//   * a PanelListModel (QAbstractListModel) the ListView binds — updateRow patches ONE row IN PLACE
//     (dataChanged), never a full reset, so the Downloads panel's live progress ticks don't reset the list;
//   * a PanelBridge (`panel` context property) carrying the title + resolved style + the model;
//   * a STACK of panels: present() pushes an entry + a graph LEVEL ("panel:<title>"); a nested present() stacks
//     another; Back pops one (the level's onPop re-renders the parent panel, or — at the root — runs that
//     panel's onBack to leave the host). This mirrors classic's per-panel onBack chain.
//
// Row activation is host-driven in C++ (onGraphActivated): the single `panelRows` zone cannot host per-row
// sub-zones, so the externalEdit CONTRACT is honoured at the row level — Enter on a TextField row runs
// Osk::getText (mirrored on this graph so Back closes the OSK only); Toggle flips; Choice cycles to the next
// option — each then fires the caller's onActivate(rowId, newValue). Info/Separator/Progress are dividers the
// cursor skips.
#pragma once
#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QWidget>
#include <functional>

#include "../ui/nav/NavThemeGraph.h"
#include "PanelModel.h"

class NavGraph;
class QQuickWidget;

// The ListView's model: one role ("rowData") returning the row's QVariantMap, so updateRow can emit
// dataChanged for a single row (in-place) instead of resetting the whole list.
class PanelListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit PanelListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(const QVector<PanelRow>& rows);   // full swap (a new panel) — begin/endResetModel
    bool patchRow(const QString& rowId, const PanelRow& row); // in-place — dataChanged on that one row
    const QVector<PanelRow>& rows() const { return rows_; }

    static constexpr int RowDataRole = Qt::UserRole + 1;
private:
    QVector<PanelRow> rows_;
};

// The `panel` context property the QML reads: title + resolved style block + the ListView model.
class PanelBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QVariantMap style READ style NOTIFY changed)
    Q_PROPERTY(QObject* model READ model CONSTANT)
public:
    PanelBridge(PanelListModel* model, QObject* parent = nullptr) : QObject(parent), model_(model) {}
    QString title() const { return title_; }
    QVariantMap style() const { return style_; }
    QObject* model() const { return model_; }
    void setTitle(const QString& t) { if (t != title_) { title_ = t; emit changed(); } }
    void setStyle(const QVariantMap& s) { style_ = s; emit changed(); }
signals:
    void changed();
private:
    PanelListModel* model_ = nullptr;
    QString title_;
    QVariantMap style_;
};

class ThemedPanelHost : public QWidget
{
    Q_OBJECT
public:
    using ActivateFn = std::function<void(const QString& rowId, const QString& newValue)>;
    using BackFn = std::function<void()>;

    explicit ThemedPanelHost(QWidget* parent = nullptr);

    NavGraph* navGraph() const { return graph_; }   // for MainWindow::updateNavForPage (presence marker)
    QWidget*  quickWidget() const;                  // the QQuickWidget key events are delivered to

    // The active theme's resolved `settingsPanel` styling block (colors/accent; empty -> QML hard fallbacks).
    // Set before present(); applied to every rendered panel until changed.
    void setStyle(const QVariantMap& style);

    // showPanel-themed analogue: present(title, rows, onActivate, onBack).
    // onActivate(rowId, newValue) — Toggle flips deliver "0"/"1"; Choice delivers the picked option;
    // TextField delivers committed text (via ThemedTextField externalEdit -> host runs Osk); Action delivers "".
    void present(const QString& title, const QVector<PanelRow>& rows,
                 std::function<void(const QString& rowId, const QString& newValue)> onActivate,
                 std::function<void()> onBack);
    void updateRow(const QString& rowId, const PanelRow& row); // in-place (Progress/Info live updates)

    // Discard every stacked panel + graph level WITHOUT running any onBack — for the host's root entry point
    // (openSettingsHub) to start a fresh presentation, and for a hard leave (Home). Idempotent.
    void reset();

    // Key/Back arbitration for when this host is the current stack page. handleBack pops one panel level.
    void handleBack();

    // UI-test snapshot helpers (the QQuickWidget focus is opaque).
    QString panelTitle() const;
    int levelDepth() const;
    QString focusedRowLabel() const;   // the label Enter would act on right now ("‹ Back" on the header zone)

private:
    struct Entry { QString title; QVector<PanelRow> rows; ActivateFn onActivate; BackFn onBack;
                   int lastIndex = 0; };   // the panelRows cursor while this panel was live (pop-restore)

    void buildView();
    // (re)populate the model/title/zone-count from the top entry + land the cursor: the first selectable row on
    // a fresh present(), the entry's REMEMBERED row on a pop-restore (the user's place survives a nested drill).
    void renderTop(bool restore = false);
    void onLevelPopped();             // a "panel:" level was popped: re-render the parent, or leave at the root
    void onGraphActivated(const QString& zone, int index);
    void onSelectionChanged(const QString& zone, int index);   // record the top entry's panelRows cursor
    static bool selectable(const PanelRow& r);   // Separator/Info/Progress are dividers the cursor skips
    int firstSelectableRow(const QVector<PanelRow>& rows) const;

    NavGraph*       graph_ = nullptr;
    PanelListModel* model_ = nullptr;
    PanelBridge*    bridge_ = nullptr;
    QQuickWidget*   view_ = nullptr;
    QVariantMap     style_;
    QVector<Entry>  stack_;            // the presented panels, innermost last (parallel to the graph levels)
};
