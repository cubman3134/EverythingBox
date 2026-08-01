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
#include <QShowEvent>
#include <QTimer>
#include <QUrl>

// ---- PickerBridge ------------------------------------------------------------------------------------------

void PickerBridge::set(const QString& t, const QStringList& n, const QStringList& kinds,
                       const QStringList& notes, bool must)
{
    title_ = t;
    names_ = n;
    fitKinds_ = kinds;
    fitNotes_ = notes;
    mustChoose_ = must;
    emit changed();
}

void PickerBridge::setStyleMap(const QVariantMap& s)
{
    style_ = s;
    emit changed();
}

// The QML-facing spelling of a verdict. Kinds, not colours: the QML resolves these against the theme's own
// settingsPanel palette, so a light theme's warning is its own warning colour and not a hardcoded red.
static QString fitKindName(ThemeFormFactors::Fit f)
{
    switch (f)
    {
        case ThemeFormFactors::Fit::Supported:   return QStringLiteral("supported");
        case ThemeFormFactors::Fit::Unsupported: return QStringLiteral("unsupported");
        case ThemeFormFactors::Fit::Undeclared:  return QStringLiteral("undeclared");
    }
    return QStringLiteral("undeclared");
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
    // The QML slot pushes its settled geometry at us (see ThemePicker.qml) — never polled. Connected BEFORE
    // setSource: the slot reports once from Component.onCompleted, which fires DURING setSource, so a connect
    // made after it would drop that first report. (It is inert today only because preview_ is still null then;
    // that is luck, not design — the wire has to exist before the scene that fires it.)
    connect(bridge_, &PickerBridge::slotChanged, this, &ThemePickerHost::layoutPreview);
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
        if (!fn) return;                    // already fired (or never armed): a disarmed surface answers nothing
        // DISARM BEFORE DISPATCH, never after. `fn` is a by-value copy so the closure survives its own handler
        // re-presenting (or tearing down) this surface — that part is unchanged. But clearing AFTER the call would
        // wipe the callbacks a re-entrant present() had just installed, leaving the new presentation dead. Clearing
        // first also makes a second Enter (or a Back arriving after a pick) a no-op, so the caller's continuation
        // cannot run twice off one presentation.
        onPicked_ = nullptr; onBack_ = nullptr;
        fn(folder);
    });
    // No levels are pushed by this host, so every Back bottoms out here: the caller owns what leaving means
    // (Appearance returns to the panel; the forced first-run step wires a quit-confirm or accepts the default).
    connect(graph_, &NavGraph::rootBack, this, [this] {
        const std::function<void()> fn = onBack_;
        if (!fn) return;
        onPicked_ = nullptr; onBack_ = nullptr;   // same disarm-before-dispatch rule as the pick path above
        fn();
    });
}

QWidget* ThemePickerHost::quickWidget() const { return view_; }

QString ThemePickerHost::focusedRowLabel() const
{
    // The DISPLAY name, not the folder: this mirrors what the row actually reads on screen, which is what a test
    // asserting "the Lumen row is selected" wants. Rebuilt from the folder (the authoritative list) rather than
    // cached, so it can never drift from what present() fed the bridge.
    if (!graph_ || graph_->zone() != QStringLiteral("themeRows")) return QString();
    const QString folder = folders_.value(graph_->index());
    return folder.isEmpty() ? QString() : ThemeEngine::themeDisplayName(folder);
}

QString ThemePickerHost::focusedRowFitNote() const
{
    if (!graph_ || graph_->zone() != QStringLiteral("themeRows")) return QString();
    const QString folder = folders_.value(graph_->index());
    // Re-derived from the folder rather than read out of the bridge, for the same reason focusedRowLabel is:
    // folders_ is the authoritative list, and a cached copy is a copy that can drift from what is on screen.
    return folder.isEmpty() ? QString()
                            : ThemeFormFactors::shortNote(ThemeEngine::themeFormFactorFit(folder));
}

void ThemePickerHost::setStyle(const QVariantMap& style)
{
    bridge_->setStyleMap(style);
    // Match the QQuickWidget backdrop to the surface background so a resize never flashes the default dark.
    const QString bg = style.value(QStringLiteral("background")).toString();
    if (view_ && !bg.isEmpty()) view_->setClearColor(QColor(bg));
}

bool ThemePickerHost::present(const QString& title, const QString& currentFolder, bool mustChoose,
                              std::function<void(const QString& folder)> onPicked,
                              std::function<void()> onBack)
{
    // THE INSTALL GUARD, held HERE so every caller inherits it. ThemeEngine::availableThemes() pads an empty
    // result with ThemeChoice::kFallbackTheme (ThemeEngine.cpp) — a NAME, not a folder on disk — and
    // ThemeEngine.h tells callers who need an actually-loadable theme to ask hasInstalledTheme() instead. Left
    // unchecked this surface would show one row over an all-black preview and hand onPicked_ a folder that does
    // not exist, persisting an unloadable theme for that profile.
    //
    // REFUSE rather than render an empty state: an empty state here is a dead screen. This surface's only
    // actions are "highlight a theme" and "commit the highlight", and with nothing installed there is nothing
    // to do on it — worse in the forced first-run mode, where Back is wired to a quit-confirm and the user
    // would be pinned between an empty list and quitting. The caller has the recovery paths (the classic
    // non-themed UI, an import flow, an error), so hand the decision back to it.
    //
    // Refuse BEFORE touching any member: a refused present must not disturb a presentation already live, and
    // must never re-arm onPicked_/folders_ with the padded list. Callers (Task 4) MUST check the return and
    // fall through when it is false — nothing is shown and no callback will ever fire.
    if (!ThemeEngine::hasInstalledTheme()) return false;

    titleText_ = title;
    onPicked_ = std::move(onPicked);
    onBack_ = std::move(onBack);

    // folders_ is the AUTHORITATIVE list; the bridge's display names are built from it in the same pass, so the
    // two stay index-aligned. A display name ("Name — Author") must never be mapped back to a folder.
    folders_ = ThemeEngine::availableThemes();
    QStringList names, fitKinds, fitNotes;
    names.reserve(folders_.size());
    for (const QString& folder : folders_)
    {
        names << ThemeEngine::themeDisplayName(folder);
        // EVERY installed theme is listed, whatever its verdict says. Marking a row is the whole of what the
        // form-factor declaration does here — it never removes one, never reorders the list, and never
        // disqualifies a row from being committed. Hiding a mismatched theme would be worse on both ends: a
        // user who deliberately installed it cannot find it, and on the forced first-run step a device whose
        // installed themes all declare something else would be left with an empty list it cannot leave.
        const ThemeFormFactors::Fit f = ThemeEngine::themeFormFactorFit(folder);
        fitKinds << fitKindName(f);
        fitNotes << ThemeFormFactors::shortNote(f);
    }
    bridge_->set(title, names, fitKinds, fitNotes, mustChoose);

    graph_->setZoneCount(QStringLiteral("themeRows"), folders_.size());
    int idx = folders_.indexOf(currentFolder);
    if (idx < 0) idx = 0;                       // absent/empty current -> the first installed theme
    // Invalidate the dedupe key BEFORE select(), not after. select() emits selectionChanged SYNCHRONOUSLY, and
    // our handler rebuilds the preview — so clearing afterwards would blow away the key that build just set and
    // make the explicit rebuildPreview() below build the SAME theme a second time, throwing the first away. One
    // redundant build is a theme.json parse, a whole QQuickWidget/QML scene, the theme's QSoundEffects and any
    // MpvPreview it declares — on every real Appearance re-entry. Clearing first makes the guard cover BOTH
    // paths: whichever of the two calls builds, the other sees a matching previewFolder_ and no-ops.
    previewFolder_.clear();
    graph_->select(QStringLiteral("themeRows"), idx);
    // select() is silent when idx is already the live index (a re-present onto the same row), so force the
    // preview: the theme LIST may have changed under us even when the cursor did not.
    rebuildPreview();
    if (view_) view_->setFocus();
    return true;
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

    // hide() before deleteLater(): a deleteLater'd widget keeps PAINTING until the event loop collects it, so
    // on the failure path below (cast fails / broken theme) the OLD theme would stay on screen for an event-loop
    // turn and only then blank — a stale preview that no longer matches the highlighted row.
    if (preview_) { preview_->hide(); preview_->deleteLater(); preview_ = nullptr; }
    previewFolder_.clear();

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("EverythingBox"));
    const QVariantList items = previewItems();
    // This is a FULL theme instantiation, not a thumbnail: buildView brings its own NavGraph, its own
    // ThemeBridge and signal fan-out, the theme's QSoundEffect set and any MpvPreview it declares — none of
    // which this surface drives (the preview takes no focus, no keys, no selection). That cost is ACCEPTED and
    // is the whole point: the preview must BE the real renderer, because a community theme is arbitrary QML and
    // nothing short of running it can show what it looks like. Do not "optimise" this into a screenshot, a
    // static mock or a cut-down loader — every such fake previews only the themes we happened to anticipate.
    // buildPreview, not buildView: THE constraint — the preview must never take the cursor — is now held by
    // construction (ThemeEngine.h), together with the categories seeding both preview sites used to repeat.
    QWidget* w = ThemeEngine::buildPreview(ThemeEngine::themesRoot() + QStringLiteral("/") + folder,
                                           items, system, this);
    preview_ = qobject_cast<QQuickWidget*>(w);
    if (!preview_) { if (w) w->deleteLater(); return; }

    // Site-specific: this surface has a click-through target behind the preview (the picker chrome), so the
    // preview must not eat the mouse either. The classic Appearance panel deliberately does NOT do this.
    preview_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
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

void ThemePickerHost::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // BOTH callers present() us and only THEN switch the stack to this page, so present()'s first
    // rebuildPreview()/layoutPreview() ran against a hidden page whose QML anchor chain had not resolved —
    // previewW/previewH read 0 and the geometry write was skipped. Recovery used to depend on the QML's
    // slotMoved push happening to fire again once we were shown; that is incidental, and this screen's whole
    // job is the preview. Lay out again here, and once more after this show has been processed (the anchored
    // slot settles during the show, so the immediate call can still read stale values — whichever of the two
    // lands on real geometry wins, and layoutPreview is idempotent).
    layoutPreview();
    QTimer::singleShot(0, this, [this] { layoutPreview(); });
}
