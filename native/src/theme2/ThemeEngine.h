// The bridge between the app and the QML theme engine. Loads a theme from disk (themeDir/theme.json) and
// hands its "home" view plus the app's real data (items/system) to the embedded QML renderer, returning a
// QQuickWidget the app can drop into a window or the view stack. The QML engine itself ships in the binary
// (qrc:/theme2/...); themes live on disk under <app>/themes2 so users can edit them.
#pragma once
#include <QString>
#include <QVariant>

class QWidget;
class QQuickWidget;

namespace ThemeEngine
{
    // Build a themed view for the given theme directory, fed with `items` (the catalog rows) and `system`
    // (view-level info such as the title). Returns a QQuickWidget parented to `parent`.
    QQuickWidget* buildView(const QString& themeDir, const QVariantList& items,
                            const QVariantMap& system, QWidget* parent = nullptr);

    // The directory themes are read from: <app>/themes2. (Built-in "Default" is deployed there.)
    QString themesRoot();
}
