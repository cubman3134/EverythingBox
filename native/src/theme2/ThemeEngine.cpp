#include "ThemeEngine.h"
#include "../core/AppPaths.h"
#include "../ui/nav/NavGraph.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QWidget>
#include <QQmlContext>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QColor>
#include <QSoundEffect>
#include <QThread>
#include <QCoreApplication>

// The theme UI sound effects live on a dedicated audio thread, not the GUI thread. Reason: the FIRST
// QSoundEffect open in the process activates the Windows audio endpoint, and on slow outputs (HDMI TVs,
// AVRs, Bluetooth — the norm for a couch media app) that activation blocks its caller for several seconds.
// If it ran on the GUI thread it would stall the first paint of the themed home by that whole duration
// (QSoundEffect loads asynchronously, so the open lands on the event loop's first pass, right when the home
// should be appearing). Owning + opening + playing them here, off the GUI thread, keeps that cost entirely
// out of the paint path — the home appears immediately and the effects are ready a moment later.
static QThread* themeAudioThread()
{
    static QThread* t = [] {
        auto* th = new QThread;                       // heap, never deleted: no ~QThread at exit -> no teardown race
        th->setObjectName(QStringLiteral("mmv-theme-audio"));
        // Stop accepting work and let the current open finish (bounded) when the app is quitting.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, th, [th] { th->quit(); th->wait(2000); });
        th->start();
        return th;
    }();
    return t;
}

// Restart-and-play a UI sound effect (no-op if the theme defined none for this action). Queued onto the
// audio thread (where the effect lives): stop()+play() there retriggers cleanly on rapid navigation without
// ever touching the GUI thread's audio path.
static void playEffect(QSoundEffect* e)
{
    if (!e) return;
    QMetaObject::invokeMethod(e, [e] { e->stop(); e->play(); }, Qt::QueuedConnection);
}

// Build a QSoundEffect for one action from the theme's "sounds" map, or null if that action has no file.
// `keys` lets an action accept aliases (e.g. "navigate"/"move"). Paths are relative to the theme folder;
// QSoundEffect plays uncompressed WAV. Volume (0..1) applies to every effect in the theme. The effect is
// created unparented and moved to the theme audio thread; its source is loaded there (see themeAudioThread).
static QSoundEffect* loadEffect(QObject* /*parent*/, const QString& themeDir, const QVariantMap& sounds,
                                const QStringList& keys, qreal volume)
{
    QString file;
    for (const QString& k : keys) { file = sounds.value(k).toString(); if (!file.isEmpty()) break; }
    if (file.isEmpty()) return nullptr;
    auto* e = new QSoundEffect;                       // no parent: it lives on the audio thread, not the GUI thread
    e->moveToThread(themeAudioThread());
    const QUrl src = QUrl::fromLocalFile(QDir(themeDir).absoluteFilePath(file));
    QMetaObject::invokeMethod(e, [e, src, volume] { e->setSource(src); e->setVolume(volume); }, Qt::QueuedConnection);
    return e;
}

// The sound effects live on the theme audio thread (not parented to this bridge, which is on the GUI
// thread). deleteLater posts each delete to that thread, where it runs safely on the thread's event loop.
ThemeBridge::~ThemeBridge()
{
    for (QSoundEffect* e : { sndNavigate, sndSelect, sndBack, sndDetails, sndTheme })
        if (e) e->deleteLater();
}

void ThemeBridge::activated() { playEffect(sndSelect); if (onActivated && root) onActivated(root->property("currentIndex").toInt()); }
void ThemeBridge::back()      { playEffect(sndBack); if (onBack) onBack(); }
void ThemeBridge::cycle()     { playEffect(sndTheme); if (onCycle) onCycle(); }
void ThemeBridge::search()    { if (onSearch) onSearch(); }
void ThemeBridge::nearEnd()   { if (onNearEnd) onNearEnd(); }
void ThemeBridge::navigate()  { playEffect(sndNavigate); }
void ThemeBridge::details()   { playEffect(sndDetails); }
void ThemeBridge::category()  { if (onCategory) onCategory(); }
void ThemeBridge::selection() { if (onSelect && root) onSelect(root->property("currentIndex").toInt()); }
void ThemeBridge::action(int which) { if (onAction) onAction(which); }
void ThemeBridge::playlistAdd() { if (onPlaylistAdd) onPlaylistAdd(); }
void ThemeBridge::button(const QString& name) { playEffect(sndSelect); if (onButton) onButton(name); }

// ---- NavGraph bridge --------------------------------------------------------------------------------------
// The selection moved: write the render prop for the SELECTED zone (and derive focusZone). setProperty to an
// unchanged value is a no-op in QML (no xChanged), so a no-op "pre-position" select from the QML costs
// nothing and fires no handler. Every side effect (nav sound / column reload / metadata fetch / near-end
// paging) still originates from the QML signals (navigate/categoryChanged) and the existing
// onCurrentIndexChanged handler that the currentIndex write re-triggers — this slot only mirrors the model
// into the props the theme binds.
void ThemeBridge::onNavSelection(const QString& zone, int index)
{
    if (!root) return;
    if (zone == QStringLiteral("items"))
        { root->setProperty("currentIndex", index); root->setProperty("focusZone", 0); }
    else if (zone == QStringLiteral("categories"))
        { root->setProperty("catIndex", index);     root->setProperty("focusZone", 0); }
    else if (zone == QStringLiteral("buttons"))
        { root->setProperty("buttonIndex", index);  root->setProperty("focusZone", 1); }
    else if (zone == QStringLiteral("actions"))
        { root->setProperty("actionIndex", index); } // the chooser is an overlay: focusZone is untouched
}

// Enter/click on the selection: route to the same fan-out the QML signals used to drive directly.
void ThemeBridge::onNavActivated(const QString& zone, int index)
{
    if (zone == QStringLiteral("buttons"))
        button(root ? root->property("focusedButtonAction").toString() : QString());
    else if (zone == QStringLiteral("actions"))
        action(index);
    else
        activated(); // items/categories -> onActivated(currentIndex), exactly as the old activated(int) signal
}

// nav.back() bottomed out (empty level stack): run the host's ROOT back action (the pause menu on the XMB /
// grid home, or the themed home from a browse view). The back SOUND is played once by the backInvoked hook
// (wired in buildView) for EVERY back gesture — pop or root — so it is deliberately not replayed here.
void ThemeBridge::onNavRootBack() { if (onBack) onBack(); }

// The inline action chooser opened/closed. The `actions` zone is registered up-front (hidden); opening the
// chooser shows it (count 4) and parks the selection there, closing hides it and restores the item cursor —
// this executes the declared actions--Esc-->items edge (see buildView). Even without the explicit restore,
// the model's reassignment now lands on the ITEM zone's remembered index (never the carried actionIndex);
// the explicit select keeps the restore in lockstep with the currentIndex prop the host may have updated.
void ThemeBridge::syncActionsZone()
{
    if (!graph || !root) return;
    if (root->property("actionsOpen").toBool())
    {
        graph->setZoneCount(QStringLiteral("actions"), 4);
        graph->select(QStringLiteral("actions"), root->property("actionIndex").toInt());
    }
    else
    {
        graph->select(QStringLiteral("items"), root->property("currentIndex").toInt());
        graph->setZoneCount(QStringLiteral("actions"), 0);
    }
}

namespace ThemeEngine
{

QString themesRoot()
{
    return AppPaths::dataDir() + QStringLiteral("/themes2");
}

QStringList availableThemes()
{
    QStringList out;
    const QFileInfoList subs = QDir(themesRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : subs)
        if (QFile::exists(d.absoluteFilePath() + QStringLiteral("/theme.json"))) out << d.fileName();
    if (out.isEmpty()) out << QStringLiteral("Default");
    return out;
}

bool hasInstalledTheme()
{
    const QFileInfoList subs = QDir(themesRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : subs)
        if (QFile::exists(d.absoluteFilePath() + QStringLiteral("/theme.json"))) return true;
    return false;
}

QString themeDisplayName(const QString& folder)
{
    QFile f(themesRoot() + QStringLiteral("/") + folder + QStringLiteral("/theme.json"));
    if (f.open(QIODevice::ReadOnly))
    {
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QString author = o.value(QStringLiteral("author")).toString();
        if (!name.isEmpty()) return author.isEmpty() ? name : (name + QStringLiteral(" — ") + author);
    }
    return folder;
}

QWidget* buildView(const QString& themeDir, const QVariantList& items, const QVariantMap& system,
                   QWidget* parent, std::function<void(int)> onActivated,
                   std::function<void()> onBack, std::function<void()> onCycle,
                   std::function<void()> onSearch, std::function<void()> onNearEnd,
                   std::function<void()> onCategory, std::function<void(int)> onSelect,
                   std::function<void(int)> onAction, std::function<void()> onPlaylistAdd,
                   std::function<void(QString)> onButton)
{
    // The whole theme on disk (all views). An empty map renders just a background.
    QVariantMap theme;
    QFile tf(themeDir + QStringLiteral("/theme.json"));
    if (tf.open(QIODevice::ReadOnly))
        theme = QJsonDocument::fromJson(tf.readAll()).object().toVariantMap();

    // A QQuickWidget: with the app's SOFTWARE Qt Quick backend (set in main()) it renders into the widget
    // backing store like any other widget — no native child window. The earlier QQuickView+createWindowContainer
    // approach embedded a real native window, which Windows would intermittently stop compositing over a
    // fullscreen app (black screen) and whose activation churn made the shell flash the taskbar. (The historic
    // "QQuickWidget rendered blank" note was the GL path, before the software backend was forced.)
    auto* qv = new QQuickWidget(parent);
    qv->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qv->setClearColor(QColor(QStringLiteral("#0F1216")));

    // The selection model for this view. Created + exposed as the `nav` context property BEFORE setSource
    // (context properties must precede the QML load). It is parented to the widget, so it dies with the view.
    //
    // Zone layout: `items` (the XMB column / the grid, Vertical) and `categories` (the XMB horizontal axis)
    // are CO-LOCATED at (0,0) — they are the two always-visible cursors of ONE surface, which pure spatial
    // crossing cannot express, so their transitions are DECLARED edges with the fused co-located step (one
    // press switches cursor AND moves it — see NavGraph.h). `actions` (the inline chooser overlay) is
    // co-located too; it is entered by activation (the host flips actionsOpen), and its declared Esc edge
    // documents the dismissal transition so validate() sees it connected. `buttons` (the bottom button bar)
    // is spatially real at row 1; its transitions restore the neighbor's cursor from per-zone memory.
    // Counts + divider sets are fed live from the QML (see ThemeView.qml); a hidden zone (count 0) makes
    // its edges inert, so XMB themes (no buttons) and grid themes (no categories) share this one wiring.
    auto* graph = new NavGraph(qv);
    graph->registerZone(QStringLiteral("items"), int(items.size()), 0, 0, Qt::Vertical);
    graph->registerZone(QStringLiteral("categories"), 0, 0, 0, Qt::Horizontal);
    graph->registerZone(QStringLiteral("buttons"), 0, 1, 0, Qt::Horizontal);
    graph->registerZone(QStringLiteral("actions"), 0, 0, 0, Qt::Vertical, /*wraps=*/true);
    graph->setDefaultZone(QStringLiteral("items"));
    // Two-cursor XMB surface: Left/Right switch to + step the category axis; Up/Down from the category
    // axis switch to + step the item column (fused step = old stepCat/step parity, no eaten press).
    graph->addEdge(QStringLiteral("items"), Qt::Key_Left,  QStringLiteral("categories"));
    graph->addEdge(QStringLiteral("items"), Qt::Key_Right, QStringLiteral("categories"));
    graph->addEdge(QStringLiteral("categories"), Qt::Key_Down, QStringLiteral("items"));
    graph->addEdge(QStringLiteral("categories"), Qt::Key_Up,   QStringLiteral("items"));
    // The bottom button bar: entered from the grid's bottom row (the QML gates WHEN — it owns the gridCols
    // geometry), left back upward with the grid cursor restored from zone memory.
    graph->addEdge(QStringLiteral("items"), Qt::Key_Down, QStringLiteral("buttons"));
    graph->addEdge(QStringLiteral("buttons"), Qt::Key_Up, QStringLiteral("items"));
    // The chooser's dismissal transition (Esc -> back onto the leaf), executed by syncActionsZone below;
    // declared so the connectivity walk sees the overlay zone linked to the surface it covers.
    graph->addEdge(QStringLiteral("actions"), Qt::Key_Escape, QStringLiteral("items"));
    // Invariant 2 gate, run on the REAL graph: geometric+declared union must be connected.
    {
        QString why;
        if (!graph->validate(&why))
            qWarning("theme2: nav graph failed validate(): %s", qPrintable(why));
    }
    qv->rootContext()->setContextProperty(QStringLiteral("nav"), graph);
    qv->setProperty("mmvNavGraph", QVariant::fromValue<QObject*>(graph)); // for ThemeEngine::navGraph()

    qv->setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));

    if (QQuickItem* root = qv->rootObject())
    {
        root->setProperty("base", QUrl::fromLocalFile(themeDir).toString());
        root->setProperty("system", system);
        root->setProperty("items", items);
        root->setProperty("currentIndex", 0);
        root->setProperty("currentView", QStringLiteral("home"));
        root->setProperty("theme", theme); // set last - everything depends on it

        auto* bridge = new ThemeBridge(qv);
        bridge->root = root;
        bridge->graph = graph;
        bridge->onActivated = std::move(onActivated);
        bridge->onBack = std::move(onBack);
        bridge->onCycle = std::move(onCycle);
        bridge->onSearch = std::move(onSearch);
        bridge->onNearEnd = std::move(onNearEnd);
        bridge->onCategory = std::move(onCategory);
        bridge->onSelect = std::move(onSelect);
        bridge->onAction = std::move(onAction);
        bridge->onPlaylistAdd = std::move(onPlaylistAdd);
        bridge->onButton = std::move(onButton);

        // Optional per-theme UI sounds: theme.json "sounds": { "navigate":"move.wav", "select":"ok.wav",
        // "back":"back.wav", "details":"info.wav", "theme":"swap.wav", "volume":0.6 } (paths relative to the
        // theme folder; WAV). Missing actions are simply silent. loadEffect owns + opens them on the theme
        // audio thread so their (potentially slow) endpoint activation never touches the GUI thread.
        const QVariantMap sounds = theme.value(QStringLiteral("sounds")).toMap();
        if (!sounds.isEmpty())
        {
            const qreal vol = sounds.contains(QStringLiteral("volume"))
                              ? qBound(0.0, sounds.value(QStringLiteral("volume")).toDouble(), 1.0) : 0.7;
            bridge->sndNavigate = loadEffect(bridge, themeDir, sounds, { QStringLiteral("navigate"), QStringLiteral("move") }, vol);
            bridge->sndSelect   = loadEffect(bridge, themeDir, sounds, { QStringLiteral("select"), QStringLiteral("open") }, vol);
            bridge->sndBack     = loadEffect(bridge, themeDir, sounds, { QStringLiteral("back") }, vol);
            bridge->sndDetails  = loadEffect(bridge, themeDir, sounds, { QStringLiteral("details") }, vol);
            bridge->sndTheme    = loadEffect(bridge, themeDir, sounds, { QStringLiteral("theme") }, vol);
        }

        QObject::connect(root, SIGNAL(activated(int)), bridge, SLOT(activated()));
        QObject::connect(root, SIGNAL(back()), bridge, SLOT(back()));
        QObject::connect(root, SIGNAL(cycleTheme()), bridge, SLOT(cycle()));
        QObject::connect(root, SIGNAL(searchRequested()), bridge, SLOT(search()));
        QObject::connect(root, SIGNAL(nearEnd()), bridge, SLOT(nearEnd()));
        QObject::connect(root, SIGNAL(navigate()), bridge, SLOT(navigate()));
        QObject::connect(root, SIGNAL(details()), bridge, SLOT(details()));
        QObject::connect(root, SIGNAL(categoryChanged()), bridge, SLOT(category()));
        QObject::connect(root, SIGNAL(selectionMoved()), bridge, SLOT(selection()));
        QObject::connect(root, SIGNAL(actionChosen(int)), bridge, SLOT(action(int)));
        QObject::connect(root, SIGNAL(addToPlaylistRequested()), bridge, SLOT(playlistAdd()));
        QObject::connect(root, SIGNAL(actionRequested(QString)), bridge, SLOT(button(QString)));

        // The NavGraph selection model -> the render props + the activate/back fan-out. The QML routes all
        // key/mouse/wheel navigation through `nav`; these connections mirror the resulting selection into the
        // properties the theme binds and dispatch activation/back to the existing callbacks.
        QObject::connect(graph, &NavGraph::selectionChanged, bridge, &ThemeBridge::onNavSelection);
        QObject::connect(graph, &NavGraph::activated,        bridge, &ThemeBridge::onNavActivated);
        QObject::connect(graph, &NavGraph::rootBack,         bridge, &ThemeBridge::onNavRootBack);
        // The ONE back sound: every back gesture (a level pop — drill-up / catalog-list / overlay-close — OR a
        // rootBack) emits backInvoked, so the sound plays exactly once per Back regardless of what it unwinds.
        QObject::connect(graph, &NavGraph::backInvoked, bridge, [bridge] { playEffect(bridge->sndBack); });
        // The inline action chooser is a transient zone; (de)register it whenever actionsOpen flips (from the
        // host opening a leaf, the host acting on a choice, or the QML dismissing it with Esc).
        QObject::connect(root, SIGNAL(actionsOpenChanged()), bridge, SLOT(syncActionsZone()));
    }

    qv->setFocusPolicy(Qt::StrongFocus);
    qv->setProperty("mmvQuickView", QVariant::fromValue<QObject*>(qv)); // marks a themed page + for rootItem()
    // The scene root, for the nav kit: NavOverlay::dismiss must forceActiveFocus() it when an overlay (the
    // OSK, a menu, a confirm) closes back onto this page — widget focus alone leaves the scene's Keys
    // handlers dead after the overlay's keyboard grab (arrow nav froze after every themed search).
    qv->setProperty("mmvQuickRoot", QVariant::fromValue<QObject*>(qv->rootObject()));
    return qv;
}

QQuickItem* rootItem(QWidget* view)
{
    if (!view) return nullptr;
    auto* qw = qobject_cast<QQuickWidget*>(view->property("mmvQuickView").value<QObject*>());
    return qw ? qw->rootObject() : nullptr;
}

NavGraph* navGraph(QWidget* view)
{
    if (!view) return nullptr;
    auto* qw = qobject_cast<QQuickWidget*>(view->property("mmvQuickView").value<QObject*>());
    if (!qw) return nullptr;
    return qobject_cast<NavGraph*>(qw->property("mmvNavGraph").value<QObject*>());
}

bool homeIsXmb(const QString& themeDir)
{
    QFile f(themeDir + QStringLiteral("/theme.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray els = root.value(QStringLiteral("views")).toObject()
                               .value(QStringLiteral("home")).toObject()
                               .value(QStringLiteral("elements")).toArray();
    for (const QJsonValue& e : els)
        if (e.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("xmb")) return true;
    return false;
}

bool homeHidesAppearanceTile(const QString& themeDir)
{
    QFile f(themeDir + QStringLiteral("/theme.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    return QJsonDocument::fromJson(f.readAll()).object()
           .value(QStringLiteral("hideAppearanceTile")).toBool();
}

} // namespace ThemeEngine
