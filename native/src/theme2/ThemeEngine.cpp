#include "ThemeEngine.h"
#include "../core/AppPaths.h"

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

// Restart-and-play a UI sound effect (no-op if the theme defined none for this action). stop() first so
// rapid navigation retriggers cleanly instead of waiting for the previous play to finish.
static void playEffect(QSoundEffect* e) { if (e) { e->stop(); e->play(); } }

// Build a QSoundEffect for one action from the theme's "sounds" map, or null if that action has no file.
// `keys` lets an action accept aliases (e.g. "navigate"/"move"). Paths are relative to the theme folder;
// QSoundEffect plays uncompressed WAV. Volume (0..1) applies to every effect in the theme.
static QSoundEffect* loadEffect(QObject* parent, const QString& themeDir, const QVariantMap& sounds,
                                const QStringList& keys, qreal volume)
{
    QString file;
    for (const QString& k : keys) { file = sounds.value(k).toString(); if (!file.isEmpty()) break; }
    if (file.isEmpty()) return nullptr;
    auto* e = new QSoundEffect(parent);
    e->setSource(QUrl::fromLocalFile(QDir(themeDir).absoluteFilePath(file)));
    e->setVolume(volume);
    return e;
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
        // theme folder; WAV). Missing actions are simply silent.
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
    }

    qv->setFocusPolicy(Qt::StrongFocus);
    qv->setProperty("mmvQuickView", QVariant::fromValue<QObject*>(qv)); // marks a themed page + for rootItem()
    return qv;
}

QQuickItem* rootItem(QWidget* view)
{
    if (!view) return nullptr;
    auto* qw = qobject_cast<QQuickWidget*>(view->property("mmvQuickView").value<QObject*>());
    return qw ? qw->rootObject() : nullptr;
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
