#include "ThemeEngine.h"
#include "../core/AppPaths.h"
#include "FormFactor.h"
#include "../ui/nav/NavGraph.h"
#include "../ui/nav/NavThemeGraph.h"

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
void ThemeBridge::detailsOpen() { if (onDetails) onDetails(); }
void ThemeBridge::detailAction(const QString& verb) { playEffect(sndSelect); if (onDetailAction) onDetailAction(verb); }
void ThemeBridge::audioTransport(const QString& verb) { playEffect(sndSelect); if (onAudioTransport) onAudioTransport(verb); }
void ThemeBridge::audioQueue(int row) { playEffect(sndSelect); if (onAudioQueue) onAudioQueue(row); }

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
    // Detail-view zones: mirror the cursor into the props the detail elements read (the action row draws its
    // focus ring from detailActionIndex + detailZone), and record which detail zone holds the cursor.
    else if (zone == QStringLiteral("detailActions"))
        { root->setProperty("detailActionIndex", index); root->setProperty("detailZone", QStringLiteral("actions")); }
    else if (zone == QStringLiteral("detailBody"))
        { root->setProperty("detailZone", QStringLiteral("body")); }
    else if (zone == QStringLiteral("detailChildren"))
        { root->setProperty("detailChildIndex", index); root->setProperty("detailZone", QStringLiteral("children")); }
    // Audio now-playing zones: mirror the cursor into the props the audio page reads (the transport strip /
    // queue list draw their focus ring from these), and record which zone holds the cursor.
    else if (zone == QStringLiteral("transport"))
        { root->setProperty("audioTransportIndex", index); root->setProperty("audioZone", QStringLiteral("transport")); }
    else if (zone == QStringLiteral("queue"))
        { root->setProperty("audioQueueIndex", index); root->setProperty("audioZone", QStringLiteral("queue")); }
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

// The detail view opened/closed (currentView flipped to/from "detail"). The detail zones are registered
// up-front (hidden); opening counts them up from the QML-computed detailActionCount / detailChildCount and
// parks the cursor in the action row, closing hides them (their edges go inert) and restores the item cursor.
// Mirrors syncActionsZone for the transient inline chooser.
void ThemeBridge::syncDetailZone()
{
    if (!graph || !root) return;
    if (root->property("currentView").toString() == QStringLiteral("detail"))
    {
        graph->setZoneCount(QStringLiteral("detailActions"), root->property("detailActionCount").toInt());
        graph->setZoneCount(QStringLiteral("detailBody"), 1);
        graph->setZoneCount(QStringLiteral("detailChildren"), root->property("detailChildCount").toInt());
        graph->select(QStringLiteral("detailActions"), 0); // land the cursor on the first action
    }
    else
    {
        graph->setZoneCount(QStringLiteral("detailActions"), 0);
        graph->setZoneCount(QStringLiteral("detailBody"), 0);
        graph->setZoneCount(QStringLiteral("detailChildren"), 0);
        graph->select(QStringLiteral("items"), root->property("currentIndex").toInt());
    }
}

// The audio now-playing view opened/closed (currentView flipped to/from "nowplayingAudio"). The transport /
// queue zones are registered up-front (hidden); opening counts them up (the transport strip has a fixed verb
// count; the queue is the session queue length) and parks the cursor on the transport strip, closing hides
// them (their edges go inert). Connected to currentViewChanged ALONGSIDE syncDetailZone — each slot owns its
// own view's zones and only ever restores the home `items` cursor when leaving ITS view (audioWasOpen_ gates
// that), so entering the detail view (which syncDetailZone parks on detailActions) is never clobbered here.
void ThemeBridge::syncAudioPageZone()
{
    if (!graph || !root) return;
    const bool nowAudio = root->property("currentView").toString() == QStringLiteral("nowplayingAudio");
    if (nowAudio)
    {
        graph->setZoneCount(QStringLiteral("transport"), root->property("audioTransportCount").toInt());
        graph->setZoneCount(QStringLiteral("queue"), root->property("audioQueueCount").toInt());
        graph->select(QStringLiteral("transport"), 0); // land the cursor on the first transport button
        audioWasOpen_ = true;
    }
    else
    {
        graph->setZoneCount(QStringLiteral("transport"), 0);
        graph->setZoneCount(QStringLiteral("queue"), 0);
        // Restore the home cursor ONLY when leaving the audio page for a home/browse view; when leaving for the
        // detail view, syncDetailZone (which ran first) already parked the cursor on detailActions — don't undo it.
        if (audioWasOpen_)
        {
            audioWasOpen_ = false;
            if (root->property("currentView").toString() != QStringLiteral("detail"))
                graph->select(QStringLiteral("items"), root->property("currentIndex").toInt());
        }
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
                   std::function<void(QString)> onButton, std::function<void()> onDetails,
                   std::function<void(QString)> onDetailAction,
                   std::function<void(QString)> onAudioTransport, std::function<void(int)> onAudioQueue)
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
    // The zone layout + declared edges are built by buildThemedNavGraph (NavThemeGraph.h) — the ONE
    // definition of the themed surface's graph shape, shared verbatim with probe_navqml section 9 so the CI
    // assertion can never drift from the shipped graph. Counts + divider sets are then fed live from the QML
    // (see ThemeView.qml); a hidden zone (count 0) makes its edges inert, so XMB themes (no buttons) and grid
    // themes (no categories) share this one wiring. (See NavThemeGraph.h for the per-zone rationale.)
    auto* graph = new NavGraph(qv);
    buildThemedNavGraph(*graph, int(items.size()));
    // The audio now-playing surface's zones (transport strip + queue list) live on the SAME graph as the home,
    // registered hidden (count 0) and count-gated by syncAudioPageZone when the "nowplayingAudio" view opens —
    // exactly like the detail zones. Built by the shared buildAudioPageNavGraph (NavThemeGraph.h), the ONE
    // definition probe_navqml shape-tests, so the CI assertion can never drift from this shipped graph.
    buildAudioPageNavGraph(*graph);
    // Invariant 2 gate, run on the REAL graph: geometric+declared union must be connected. Under MMV_UITEST
    // (the probe/UI-test build) a failure is FATAL, not a warning: the graph shape is CI-asserted by
    // probe_navqml against the same builder, so a validate() failure here means a real structural break that
    // must halt the test run loudly rather than log-and-continue into undefined navigation.
    {
        QString why;
        if (!graph->validate(&why))
        {
            if (qEnvironmentVariableIntValue("MMV_UITEST") == 1)
                qFatal("theme2: nav graph failed validate(): %s", qPrintable(why)); // fail loudly in test/probe runs
            else
                qWarning("theme2: nav graph failed validate(): %s", qPrintable(why));
        }
    }
    qv->rootContext()->setContextProperty(QStringLiteral("nav"), graph);
    // The form-factor authority (subsystem D): every themed surface reads `form` for uiScale / safe-area insets.
    // Context properties must precede setSource; the singleton outlives every view, so it is not parented here.
    qv->rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
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
        bridge->onDetails = std::move(onDetails);
        bridge->onDetailAction = std::move(onDetailAction);
        bridge->onAudioTransport = std::move(onAudioTransport);
        bridge->onAudioQueue = std::move(onAudioQueue);

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
        QObject::connect(root, SIGNAL(detailsRequested()), bridge, SLOT(detailsOpen()));
        QObject::connect(root, SIGNAL(detailActionRequested(QString)), bridge, SLOT(detailAction(QString)));
        QObject::connect(root, SIGNAL(audioTransportRequested(QString)), bridge, SLOT(audioTransport(QString)));
        QObject::connect(root, SIGNAL(audioQueueActivateRequested(int)), bridge, SLOT(audioQueue(int)));

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
        // The detail view's zones follow currentView: opening (-> "detail") counts them up + parks the cursor
        // in the action row, closing hides them + restores the item cursor.
        QObject::connect(root, SIGNAL(currentViewChanged()), bridge, SLOT(syncDetailZone()));
        // The audio now-playing view's zones follow currentView too (-> "nowplayingAudio"). Connected AFTER
        // syncDetailZone so, on a detail<->audio transition, the detail slot parks its cursor first and the
        // audio slot's leave-restore stands off (see syncAudioPageZone).
        QObject::connect(root, SIGNAL(currentViewChanged()), bridge, SLOT(syncAudioPageZone()));
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
