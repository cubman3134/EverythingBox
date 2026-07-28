// "Who's using EverythingBox?" — pick an existing profile or create one. Shown at startup (mustChoose: there is
// no Cancel; closing without a choice means the app won't proceed) and from the Home profile button (where
// Cancel just keeps the current profile). On accept, selectedId() is the chosen/created profile id.
#pragma once
#include <QDialog>
#include <QStringList>
#include <functional>

class QVBoxLayout;
class QStackedWidget;

class ProfileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfileDialog(bool mustChoose, QWidget* parent = nullptr);
    QString selectedId() const { return selectedId_; }

    // The set of cute avatar glyphs offered in the name/icon picker — shared with the themed Profiles panel
    // (ThemedPanelHost) so both surfaces offer the SAME icon list from one source of truth.
    static QStringList iconChoices();

signals:
    // "Open the passcode chooser for this profile" (issue #30). A SIGNAL rather than the flow itself: the
    // Set/Change/Remove chooser is a NavOverlay stack that has to live on the main window, and duplicating it
    // here would give the classic builder a second, drifting copy of a security-shaped flow. MainWindow owns
    // the one implementation and both builders raise it. Emitted only from the EDIT page (a profile being
    // created has no id yet, and the passcode is keyed to one).
    //
    // `onDone` is how this page learns the answer. The chooser is a stack of overlays that resolve
    // asynchronously — read the store back on the line after the emit and you get the state from BEFORE the
    // user chose anything — so the receiver runs this once its flow has unwound. Carrying a std::function
    // through a signal is safe here and only here: the connection is same-thread and DIRECT, so no metatype
    // registration and no cross-thread copy of a capturing callable is involved.
    void passcodeRequested(const QString& profileId, std::function<void()> onDone);

private:
    void rebuild();         // (re)draw the list of profile rows
    void createProfile();   // prompt for a name + icon and add it (auto-selects the new profile)
    void editProfile(const QString& id); // rename / re-pick the icon of an existing profile
    // Shared name + cute-icon picker shown as an in-place page (no popup); onAccept(name, icon) runs on OK.
    // `passcodeFor` non-empty adds the "Passcode…" row and emits passcodeRequested with it (edit only).
    void showPicker(const QString& title, const QString& name, const QString& icon,
                    const std::function<void(const QString& name, const QString& icon)>& onAccept,
                    const QString& passcodeFor = QString());

    bool mustChoose_ = false;
    QString selectedId_;
    QVBoxLayout* rows_ = nullptr;
    QStackedWidget* stack_ = nullptr; // page 0 = profile list, page 1 = (transient) name/icon picker
};
