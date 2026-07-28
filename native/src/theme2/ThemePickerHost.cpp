#include "ThemePickerHost.h"
#include "FormFactor.h"
#include "ThemeEngine.h"
#include "../core/SafeAreaInsets.h"
#include "../ui/nav/NavGraph.h"

#include <QColor>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QUrl>

// ---- PickerBridge ------------------------------------------------------------------------------------------

void PickerBridge::set(const QString& t, const QStringList& n, bool must)
{
    title_ = t;
    names_ = n;
    mustChoose_ = must;
    emit changed();
}

void PickerBridge::setStyleMap(const QVariantMap& s)
{
    style_ = s;
    emit changed();
}

// ---- ThemePickerHost ---------------------------------------------------------------------------------------

ThemePickerHost::ThemePickerHost(QWidget* parent) : QWidget(parent)
{
    graph_ = new NavGraph(this);
    // THE SINGLE ZONE. `themeRows` is the only zone this surface registers — the live preview is deliberately
    // NOT a zone (see rebuildPreview). A second zone here, or a focusable preview, would let the cursor walk
    // into a view it cannot leave. The two SELF edges are containment pins (NavGraph::addEdge): the zone is
    // Vertical, so Left/Right are its cross axis and would otherwise fall through to geometric crossing onto
    // nothing; Up/Down step the list and, at its ends, find no zone above or below — a contained no-op.
    // Registered at count 0; present() feeds the live count (setZoneCount), the same count-gating discipline
    // ThemedPanelHost uses. This shape is not shared with a probe, so it is spelled here rather than in
    // NavThemeGraph.h (that header exists to stop app/probe drift; there is no probe to drift from).
    graph_->registerZone(QStringLiteral("themeRows"), 0, 0, 0, Qt::Vertical);
    graph_->setDefaultZone(QStringLiteral("themeRows"));
    graph_->addEdge(QStringLiteral("themeRows"), Qt::Key_Left,  QStringLiteral("themeRows"));
    graph_->addEdge(QStringLiteral("themeRows"), Qt::Key_Right, QStringLiteral("themeRows"));

    bridge_ = new PickerBridge(this);

    // The chrome view, built exactly as ThemedPanelHost::buildView does. NO QVBoxLayout here (the one place
    // this host must diverge): the preview is a SIBLING widget raised over this one, so both are positioned by
    // hand in resizeEvent/layoutPreview — a layout would fight the overlay for view_'s geometry.
    view_ = new QQuickWidget(this);
    view_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view_->setClearColor(QColor(QStringLiteral("#0F1216")));
    view_->setFocusPolicy(Qt::StrongFocus);
    // Context properties MUST precede setSource (they resolve as the QML loads). `nav` is the selection model;
    // `picker` carries the title/style/names/mustChoose the surface binds.
    view_->rootContext()->setContextProperty(QStringLiteral("nav"), graph_);
    view_->rootContext()->setContextProperty(QStringLiteral("picker"), bridge_);
    view_->rootContext()->setContextProperty(QStringLiteral("safeArea"), &SafeAreaBridge::instance());
    view_->rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
    view_->setSource(QUrl(QStringLiteral("qrc:/theme2/ThemePicker.qml")));
    // The scene-root markers the nav kit reads (NavOverlay::dismiss force-focuses "mmvQuickRoot" when an
    // overlay closes back onto a host; ThemeEngine::rootItem resolves "mmvQuickView"). Same wire as
    // ThemedPanelHost — this is a SEPARATE QQuickWidget buildView never touches.
    view_->setProperty("mmvQuickView", QVariant::fromValue<QObject*>(view_));
    view_->setProperty("mmvQuickRoot", QVariant::fromValue<QObject*>(view_->rootObject()));
    view_->setGeometry(rect());

    // Selection change -> the preview follows the cursor. rebuildPreview no-ops when the folder is unchanged.
    connect(graph_, &NavGraph::selectionChanged, this, [this](const QString& zone, int) {
        if (zone == QStringLiteral("themeRows")) rebuildPreview();
    });
    // Enter on a row commits the FOLDER (never the display name — themeDisplayName returns "Name — Author",
    // which does not round-trip). Dispatch through a BY-VALUE copy of the handler: onPicked_ may re-present
    // (or tear down) this surface, which would otherwise destroy the closure while it runs.
    connect(graph_, &NavGraph::activated, this, [this](const QString& zone, int index) {
        if (zone != QStringLiteral("themeRows")) return;
        const QString folder = folders_.value(index);
        if (folder.isEmpty()) return;
        const std::function<void(const QString&)> fn = onPicked_;
        if (fn) fn(folder);
    });
    // No levels are pushed by this host, so every Back bottoms out here: the caller owns what leaving means
    // (Appearance returns to the panel; the forced first-run step wires a quit-confirm).
    connect(graph_, &NavGraph::rootBack, this, [this] {
        const std::function<void()> fn = onBack_;
        if (fn) fn();
    });
    // The QML slot pushes its settled geometry at us (see ThemePicker.qml) — never polled.
    connect(bridge_, &PickerBridge::slotChanged, this, &ThemePickerHost::layoutPreview);
}

QWidget* ThemePickerHost::quickWidget() const { return view_; }

void ThemePickerHost::setStyle(const QVariantMap& style)
{
    bridge_->setStyleMap(style);
    // Match the QQuickWidget backdrop to the surface background so a resize never flashes the default dark.
    const QString bg = style.value(QStringLiteral("background")).toString();
    if (view_ && !bg.isEmpty()) view_->setClearColor(QColor(bg));
}

void ThemePickerHost::present(const QString& title, const QString& currentFolder, bool mustChoose,
                              std::function<void(const QString& folder)> onPicked,
                              std::function<void()> onBack)
{
    titleText_ = title;
    onPicked_ = std::move(onPicked);
    onBack_ = std::move(onBack);

    // folders_ is the AUTHORITATIVE list; the bridge's display names are built from it in the same pass, so the
    // two stay index-aligned. A display name ("Name — Author") must never be mapped back to a folder.
    folders_ = ThemeEngine::availableThemes();
    QStringList names;
    names.reserve(folders_.size());
    for (const QString& folder : folders_)
        names << ThemeEngine::themeDisplayName(folder);
    bridge_->set(title, names, mustChoose);

    graph_->setZoneCount(QStringLiteral("themeRows"), folders_.size());
    int idx = folders_.indexOf(currentFolder);
    if (idx < 0) idx = 0;                       // absent/empty current -> the first installed theme
    graph_->select(QStringLiteral("themeRows"), idx);
    // select() is silent when idx is already the live index (a re-present onto the same row), so force the
    // preview: the theme LIST may have changed under us even when the cursor did not.
    previewFolder_.clear();
    rebuildPreview();
    if (view_) view_->setFocus();
}

QVariantList ThemePickerHost::previewItems()
{
    // The four inherent categories, so an XMB/Triple theme shows its cross and a Grid-style theme shows
    // tiles. Matches what the classic Appearance preview has always used as its empty-library fallback.
    QVariantList items;
    for (const char* n : { "Video", "Games", "Audio", "Reading" })
        items << QVariantMap{ { QStringLiteral("title"), QString::fromLatin1(n) },
                              { QStringLiteral("accent"), QStringLiteral("#3E8E7E") } };
    return items;
}

void ThemePickerHost::rebuildPreview()
{
    const int idx = (graph_ && graph_->zone() == QStringLiteral("themeRows")) ? graph_->index() : 0;
    const QString folder = folders_.value(idx);
    if (folder.isEmpty()) return;
    if (preview_ && folder == previewFolder_) return;   // the cursor moved but the theme did not

    if (preview_) { preview_->deleteLater(); preview_ = nullptr; }
    previewFolder_.clear();

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("EverythingBox"));
    const QVariantList items = previewItems();
    QWidget* w = ThemeEngine::buildView(ThemeEngine::themesRoot() + QStringLiteral("/") + folder,
                                        items, system, this);
    preview_ = qobject_cast<QQuickWidget*>(w);
    if (!preview_) { if (w) w->deleteLater(); return; }

    // THE constraint: the preview must never take the cursor. NoFocus on the widget, and it is registered
    // in no nav zone, so arrows/Enter always reach the list.
    preview_->setFocusPolicy(Qt::NoFocus);
    preview_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (QQuickItem* r = ThemeEngine::rootItem(preview_))   // feed categories too, so XMB shows its cross
        { r->setProperty("categories", items); r->setProperty("catIndex", 0); }
    previewFolder_ = folder;
    preview_->show();
    layoutPreview();
}

void ThemePickerHost::layoutPreview()
{
    if (!preview_ || !view_) return;
    QQuickItem* root = view_->rootObject();
    if (!root) return;
    // The QML exposes the slot's geometry (pushed at us via PickerBridge::slotMoved whenever it settles);
    // mirror it onto the overlaid widget. The QQuickWidget fills this host's rect, so scene coords ARE our
    // child coords.
    const int x = int(root->property("previewX").toReal());
    const int y = int(root->property("previewY").toReal());
    const int w = int(root->property("previewW").toReal());
    const int h = int(root->property("previewH").toReal());
    if (w > 0 && h > 0) { preview_->setGeometry(x, y, w, h); preview_->raise(); }
}

void ThemePickerHost::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (view_) view_->setGeometry(rect());
    layoutPreview();   // the slot moved with the chrome (the QML also pushes, whichever lands first)
}
