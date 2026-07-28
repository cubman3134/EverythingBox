// ThemePickerHost — the ONE theme-chooser surface, used both as the forced first-run step and from
// Appearance. A persistent stack page constructed in the MainWindow ctor (like ThemedPanelHost and
// ReaderChromeHost), so it can present PRE-HOME: at first run openHome() has not run and home_ does not
// exist yet.
//
// It owns:
//   * a NavGraph with a single Vertical `themeRows` zone, exposed to the QML as `nav`;
//   * a PickerBridge (`picker` context property) carrying the title, the resolved settingsPanel style, the
//     theme display names, and whether Back is allowed;
//   * the live preview: a real ThemeEngine::buildView of the highlighted theme, rebuilt on selection change
//     and reparented over the QML's preview slot.
//
// THE PREVIEW IS REGISTERED IN NO NAV ZONE and is created Qt::NoFocus. A focusable live view inside a nav
// surface takes the cursor and strands the user in a preview they cannot leave — the single constraint this
// class exists to hold. (See rebuildPreview() in the .cpp: the setFocusPolicy(Qt::NoFocus) call, and the
// ctor's ONE registerZone.)
#pragma once
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
#include <functional>

class NavGraph;
class QQuickWidget;

// The `picker` context property the QML reads.
class PickerBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QVariantMap style READ style NOTIFY changed)
    Q_PROPERTY(QStringList names READ names NOTIFY changed)
    Q_PROPERTY(bool mustChoose READ mustChoose NOTIFY changed)
public:
    using QObject::QObject;
    QString title() const { return title_; }
    QVariantMap style() const { return style_; }
    QStringList names() const { return names_; }
    bool mustChoose() const { return mustChoose_; }

    void set(const QString& t, const QStringList& n, bool must);
    void setStyleMap(const QVariantMap& s);

    // The QML preview slot calls this whenever its anchored geometry settles (and once on completion). The
    // host relays it to layoutPreview() — a PUSH, so the host never reads a half-resolved anchor chain.
    Q_INVOKABLE void slotMoved() { emit slotChanged(); }
signals:
    void changed();
    void slotChanged();
private:
    QString title_;
    QVariantMap style_;
    QStringList names_;
    bool mustChoose_ = false;
};

class ThemePickerHost : public QWidget
{
    Q_OBJECT
public:
    explicit ThemePickerHost(QWidget* parent = nullptr);

    // Present the picker. `currentFolder` seeds the highlighted row. `mustChoose` = the forced first-run
    // mode: Back runs onBack (the caller wires the quit-confirm), and there is no other exit.
    // onPicked receives the chosen FOLDER name (not the display name) — persisting it (ThemeChoice) belongs
    // to the caller, exactly as ThemedPanelHost hands its onActivate the raw row value.
    void present(const QString& title, const QString& currentFolder, bool mustChoose,
                 std::function<void(const QString& folder)> onPicked,
                 std::function<void()> onBack);

    void setStyle(const QVariantMap& style);
    QString title() const { return titleText_; }
    NavGraph* graph() const { return graph_; }
    QWidget* quickWidget() const;   // the QQuickWidget key events are delivered to (MainWindow key routing)

    // The synthetic sample data both this surface and the classic Appearance preview render. At first run
    // there is no library and no home_, so the synthetic path is the ONLY path that works there — sharing it
    // is what stops the two previews drifting.
    static QVariantList previewItems();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuildPreview();          // tear down + rebuild the live preview for the selected row
    void layoutPreview();           // position the preview widget over the QML's preview slot

    QQuickWidget* view_ = nullptr;  // the picker chrome
    QQuickWidget* preview_ = nullptr; // the live theme preview (NoFocus, no nav zone)
    NavGraph* graph_ = nullptr;
    PickerBridge* bridge_ = nullptr;
    QString titleText_;
    QStringList folders_;           // folder names, index-aligned with the bridge's display names
    QString previewFolder_;         // the folder preview_ currently renders (skip redundant rebuilds)
    std::function<void(const QString&)> onPicked_;
    std::function<void()> onBack_;
};
