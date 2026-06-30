#include "ThemeEngine.h"
#include "../core/AppPaths.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QUrl>
#include <QDir>

namespace ThemeEngine
{

QString themesRoot()
{
    return AppPaths::dataDir() + QStringLiteral("/themes2");
}

QQuickWidget* buildView(const QString& themeDir, const QVariantList& items,
                        const QVariantMap& system, QWidget* parent)
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
    }
    return w;
}

} // namespace ThemeEngine
