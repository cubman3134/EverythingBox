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
    // The form-factor verdict per row (issue #32), INDEX-ALIGNED with names. Two lists rather than one:
    //   * `fitNotes` is the sentence to show, resolved in C++ by ThemeFormFactors::shortNote so the QML,
    //     the classic Appearance list and the themed Appearance row cannot word it three different ways.
    //     An EMPTY entry means "this theme fits — decorate nothing".
    //   * `fitKinds` is "supported" | "unsupported" | "undeclared", which is what the QML colours by: "the
    //     author did not list this device" is a warning, "nobody said" is merely dim, and the note text
    //     alone cannot carry that distinction.
    Q_PROPERTY(QStringList fitKinds READ fitKinds NOTIFY changed)
    Q_PROPERTY(QStringList fitNotes READ fitNotes NOTIFY changed)
    Q_PROPERTY(bool mustChoose READ mustChoose NOTIFY changed)
public:
    using QObject::QObject;
    QString title() const { return title_; }
    QVariantMap style() const { return style_; }
    QStringList names() const { return names_; }
    QStringList fitKinds() const { return fitKinds_; }
    QStringList fitNotes() const { return fitNotes_; }
    bool mustChoose() const { return mustChoose_; }

    void set(const QString& t, const QStringList& n, const QStringList& kinds, const QStringList& notes,
             bool must);
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
    QStringList fitKinds_;
    QStringList fitNotes_;
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
    //
    // RETURNS FALSE WHEN IT REFUSES TO PRESENT — currently the one case where no theme is installed
    // (ThemeEngine::hasInstalledTheme() is false; availableThemes() would pad the list with a folder that is
    // not on disk, so there is nothing real to offer and nothing to preview). On false NOTHING is shown, no
    // state is touched, and neither onPicked nor onBack will ever fire: the CALLER must fall through to its own
    // path (the classic non-themed UI, an import flow, an error) rather than leaving a dead screen up. On true
    // the surface is presented and holds the callbacks. onPicked is guaranteed to receive an installed folder.
    [[nodiscard]] bool present(const QString& title, const QString& currentFolder, bool mustChoose,
                               std::function<void(const QString& folder)> onPicked,
                               std::function<void()> onBack);

    void setStyle(const QVariantMap& style);
    QString title() const { return titleText_; }
    NavGraph* graph() const { return graph_; }
    QWidget* quickWidget() const;   // the QQuickWidget key events are delivered to (MainWindow key routing)

    // The DISPLAY name of the row Enter would commit right now ("Name — Author"), for the uitest state snapshot.
    // Mirrors ThemedPanelHost::focusedRowLabel: the QQuickWidget's focus is opaque, so the surface has to report
    // its own selection or automation cannot see this screen at all. Empty when nothing is presented.
    QString focusedRowLabel() const;

    // The form-factor note on the row Enter would commit right now, EMPTY when that theme fits this device
    // (issue #32). Reported separately from focusedRowLabel rather than appended to it: that label is the
    // theme's display name and existing automation matches it exactly, so growing it would break every such
    // assertion. Empty when nothing is presented.
    QString focusedRowFitNote() const;

    // The synthetic sample data both this surface and the classic Appearance preview render. At first run
    // there is no library and no home_, so the synthetic path is the ONLY path that works there — sharing it
    // is what stops the two previews drifting.
    static QVariantList previewItems();

protected:
    void resizeEvent(QResizeEvent* e) override;
    // present() runs its first rebuildPreview/layoutPreview while this page is still HIDDEN (both callers switch
    // the stack afterwards), so the QML slot's anchored geometry is still zero and the preview would be laid out
    // at 0x0. Re-layout on show rather than relying on the QML's slotMoved push happening to fire again.
    void showEvent(QShowEvent* e) override;

private:
    void rebuildPreview();          // tear down + rebuild the live preview for the selected row
    void layoutPreview();           // position the preview widget over the QML's preview slot
    // Run `work` on the next event-loop turn, once the QML signal emission that reached us has unwound
    // (issue #28 / #165). Same mechanism, same rule, same reason as ThemedPanelHost::deferPastQmlEmission —
    // ThemePicker.qml's row-delegate MouseArea and root Keys handler drive nav.activate()/nav.back(), so this
    // host's two caller dispatches also run on a live delegate's emission, and both of them reach a nested
    // event loop (the startup Back is a NavConfirm quit prompt) or retire this very surface.
    void deferPastQmlEmission(std::function<void()> work);

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
