#include "ThemeEngine.h"
#include "../core/AppPaths.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QUrl>

void ThemeBridge::activated() { if (onActivated && root) onActivated(root->property("currentIndex").toInt()); }
void ThemeBridge::back()      { if (onBack) onBack(); }
void ThemeBridge::cycle()     { if (onCycle) onCycle(); }

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

QQuickWidget* buildView(const QString& themeDir, const QVariantList& items, const QVariantMap& system,
                        QWidget* parent, std::function<void(int)> onActivated,
                        std::function<void()> onBack, std::function<void()> onCycle)
{
    // The "home" view of the theme on disk (falls back to an empty view, which renders just a background).
    QVariantMap view;
    QFile tf(themeDir + QStringLiteral("/theme.json"));
    if (tf.open(QIODevice::ReadOnly))
    {
        const QJsonObject theme = QJsonDocument::fromJson(tf.readAll()).object();
        view = theme.value(QStringLiteral("views")).toObject()
                    .value(QStringLiteral("home")).toObject().toVariantMap();
    }

    auto* w = new QQuickWidget(parent);
    w->setResizeMode(QQuickWidget::SizeRootObjectToView);
    w->setFocusPolicy(Qt::StrongFocus); // so the embedded QML scene receives key events
    w->setSource(QUrl(QStringLiteral("qrc:/theme2/ThemeView.qml")));

    // QQuickWidget has no setInitialProperties; set the properties on the loaded root object. Bindings
    // re-evaluate as each lands, so `view` is set last (everything depends on it).
    if (QQuickItem* root = w->rootObject())
    {
        root->setProperty("base", QUrl::fromLocalFile(themeDir).toString());
        root->setProperty("system", system);
        root->setProperty("items", items);
        root->setProperty("currentIndex", 0);
        root->setProperty("view", view);

        auto* bridge = new ThemeBridge(w);
        bridge->root = root;
        bridge->onActivated = std::move(onActivated);
        bridge->onBack = std::move(onBack);
        bridge->onCycle = std::move(onCycle);
        QObject::connect(root, SIGNAL(activated(int)), bridge, SLOT(activated()));
        QObject::connect(root, SIGNAL(back()), bridge, SLOT(back()));
        QObject::connect(root, SIGNAL(cycleTheme()), bridge, SLOT(cycle()));
    }
    return w;
}

} // namespace ThemeEngine
