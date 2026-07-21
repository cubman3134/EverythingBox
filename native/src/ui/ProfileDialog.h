// "Who's using My Media Vault?" — pick an existing profile or create one. Shown at startup (mustChoose: there is
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

private:
    void rebuild();         // (re)draw the list of profile rows
    void createProfile();   // prompt for a name + icon and add it (auto-selects the new profile)
    void editProfile(const QString& id); // rename / re-pick the icon of an existing profile
    // Shared name + cute-icon picker shown as an in-place page (no popup); onAccept(name, icon) runs on OK.
    void showPicker(const QString& title, const QString& name, const QString& icon,
                    const std::function<void(const QString& name, const QString& icon)>& onAccept);

    bool mustChoose_ = false;
    QString selectedId_;
    QVBoxLayout* rows_ = nullptr;
    QStackedWidget* stack_ = nullptr; // page 0 = profile list, page 1 = (transient) name/icon picker
};
