// The bridge between the app and the QML theme engine. Loads a theme from disk (themeDir/theme.json) and
// hands its "home" view plus the app's real data (items/system) to the embedded QML renderer, returning a
// QQuickWidget the app can drop into a window or the view stack. The QML engine itself ships in the binary
// (qrc:/theme2/...); themes live on disk under <app>/themes2 so users can edit them.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <functional>

class QWidget;
class QQuickWidget;
class QQuickItem;

// Relays the QML view's signals (navigation activate / back / cycle-theme) to plain C++ callbacks. The
// activate signal carries no argument; we read the current selection off the root item.
class ThemeBridge : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QQuickItem* root = nullptr;
    std::function<void(int)> onActivated;
    std::function<void()> onBack;
    std::function<void()> onCycle;
public slots:
    void activated();
    void back();
    void cycle();
};

namespace ThemeEngine
{
    // Build a themed view for the given theme directory, fed with `items` (the catalog rows) and `system`.
    // The callbacks fire on Enter (with the selected index), Esc/Back, and the theme-cycle key.
    QQuickWidget* buildView(const QString& themeDir, const QVariantList& items, const QVariantMap& system,
                            QWidget* parent = nullptr,
                            std::function<void(int)> onActivated = {},
                            std::function<void()> onBack = {},
                            std::function<void()> onCycle = {});

    QString themesRoot();              // <app>/themes2
    QStringList availableThemes();     // subdirectories of themesRoot that contain a theme.json
    QString themeDisplayName(const QString& folder); // theme.json "name [— author]", else the folder name
}
