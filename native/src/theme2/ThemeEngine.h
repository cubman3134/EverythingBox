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
class QQuickItem;
class QSoundEffect;

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
    std::function<void()> onSearch;
    std::function<void()> onNearEnd;
    std::function<void()> onCategory; // XMB: the category cursor moved (host loads that column)
    std::function<void(int)> onSelect; // XMB: the column cursor moved to `index` (host fetches its metadata)
    std::function<void(int)> onAction; // XMB: chose an inline action (0=Play, 1=Favorite, 2=Add to playlist)
    std::function<void()> onPlaylistAdd; // XMB: "P" on the highlighted item (host adds it to a playlist)

    // Optional per-theme UI sounds (owned by this bridge; null when the theme defines none for that action).
    QSoundEffect* sndNavigate = nullptr; // selection moved
    QSoundEffect* sndSelect = nullptr;   // Enter / open
    QSoundEffect* sndBack = nullptr;     // Esc / back
    QSoundEffect* sndDetails = nullptr;  // opened the detail view
    QSoundEffect* sndTheme = nullptr;    // cycled theme
public slots:
    void activated();
    void back();
    void cycle();
    void search();
    void nearEnd();
    void navigate();  // play the navigation sound
    void details();   // play the open-details sound
    void category();  // XMB: category cursor moved
    void selection(); // XMB: column cursor moved (host fetches the selected item's metadata)
    void action(int which); // XMB: chose an inline Play/Favorite/Add-to-playlist action
    void playlistAdd();     // XMB: "P" - add the highlighted item to a playlist
};

namespace ThemeEngine
{
    // Build a themed view widget for the given theme directory, fed with `items` (the catalog rows) and
    // `system`. The callbacks fire on Enter (with the selected index), Esc/Back, and the theme-cycle key.
    // The returned widget embeds a software-rendered QQuickView (see rootItem() to update it live).
    QWidget* buildView(const QString& themeDir, const QVariantList& items, const QVariantMap& system,
                       QWidget* parent = nullptr,
                       std::function<void(int)> onActivated = {},
                       std::function<void()> onBack = {},
                       std::function<void()> onCycle = {},
                       std::function<void()> onSearch = {},
                       std::function<void()> onNearEnd = {},
                       std::function<void()> onCategory = {},
                       std::function<void(int)> onSelect = {},
                       std::function<void(int)> onAction = {},
                       std::function<void()> onPlaylistAdd = {});

    // The QML root item of a widget returned by buildView(), for setting properties live (items/view/...).
    QQuickItem* rootItem(QWidget* view);

    // True if the theme's `home` view contains an `xmb` element (so the host drives it as an XMB cross).
    bool homeIsXmb(const QString& themeDir);

    QString themesRoot();              // <app>/themes2
    QStringList availableThemes();     // subdirectories of themesRoot that contain a theme.json
    QString themeDisplayName(const QString& folder); // theme.json "name [— author]", else the folder name
}
