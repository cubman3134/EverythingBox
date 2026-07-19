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
class NavGraph;

// Relays the QML view's signals (navigation activate / back / cycle-theme) to plain C++ callbacks. The
// activate signal carries no argument; we read the current selection off the root item.
class ThemeBridge : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~ThemeBridge() override;
    QQuickItem* root = nullptr;
    NavGraph* graph = nullptr;   // the selection model for this view (owned by the QQuickWidget)
    std::function<void(int)> onActivated;
    std::function<void()> onBack;
    std::function<void()> onCycle;
    std::function<void()> onSearch;
    std::function<void()> onNearEnd;
    std::function<void()> onCategory; // XMB: the category cursor moved (host loads that column)
    std::function<void(int)> onSelect; // XMB: the column cursor moved to `index` (host fetches its metadata)
    std::function<void(int)> onAction; // XMB: chose an inline action (0=Play, 1=Favorite, 2=Add to playlist)
    std::function<void()> onPlaylistAdd; // XMB: "P" on the highlighted item (host adds it to a playlist)
    std::function<void(QString)> onButton; // a `button` element fired its named action (e.g. settings/profile)
    std::function<void()> onDetails;       // "I"/Info: open the themed detail view for the current selection
    std::function<void(QString)> onDetailAction; // the detail action row fired a verb (play/download/favorite/playlist)

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
    void button(const QString& name); // a `button` element fired its named action
    void detailsOpen();               // "I"/Info -> the host opens the themed detail view
    void detailAction(const QString& verb); // the detail action row fired a verb -> the host runs it

    // ---- NavGraph bridge: the selection model (`nav` in QML) drives these; they write the SAME QML props
    //      every element binds today, so all the existing animations/handlers keep working untouched. -------
    // The selection moved (arrow/mouse/programmatic): write the render prop for the selected zone. Side
    // effects (nav sound, column reload, metadata fetch, near-end paging) still flow through the QML signals
    // (navigate/categoryChanged/onCurrentIndexChanged), so nothing about the render pipeline changes here.
    void onNavSelection(const QString& zone, int index);
    // Enter/click activated the selection: dispatch to the existing fan-out for the selected zone.
    void onNavActivated(const QString& zone, int index);
    void onNavRootBack();      // nav.back() bottomed out -> the existing themed back() path (pause menu, drill)
    void syncActionsZone();    // actionsOpen flipped: (de)register the inline-chooser zone + park the selection
    void syncDetailZone();     // currentView flipped: (de)register the detail-view zones + park the selection
};

namespace ThemeEngine
{
    // Build a themed view widget for the given theme directory, fed with `items` (the catalog rows) and
    // `system`. The callbacks fire on Enter (with the selected index), Esc/Back, and the theme-cycle key.
    // The returned widget is a software-rendered QQuickWidget (see rootItem() to update it live).
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
                       std::function<void()> onPlaylistAdd = {},
                       std::function<void(QString)> onButton = {},
                       std::function<void()> onDetails = {},
                       std::function<void(QString)> onDetailAction = {});

    // The QML root item of a widget returned by buildView(), for setting properties live (items/view/...).
    QQuickItem* rootItem(QWidget* view);

    // The selection model (`nav`) behind a widget returned by buildView(), for the host/NavContext to register
    // its presence and drive selection. Mirrors rootItem(); null for a non-themed widget.
    NavGraph* navGraph(QWidget* view);

    // True if the theme's `home` view contains an `xmb` element (so the host drives it as an XMB cross).
    bool homeIsXmb(const QString& themeDir);

    // True if the theme sets top-level "hideAppearanceTile": true (it provides its own settings/theme button,
    // so the host shouldn't also add an Appearance catalog tile to the grid).
    bool homeHidesAppearanceTile(const QString& themeDir);

    QString themesRoot();              // <app>/themes2
    QStringList availableThemes();     // subdirectories of themesRoot that contain a theme.json
    // True when at least one real theme (a theme.json on disk) is installed. availableThemes() pads its empty
    // result with a "Default" placeholder for the picker, so callers that need an actually-loadable theme
    // (the themed home) check this instead — an empty theme renders an all-black, invisibly-navigable view.
    bool hasInstalledTheme();
    QString themeDisplayName(const QString& folder); // theme.json "name [— author]", else the folder name
}
