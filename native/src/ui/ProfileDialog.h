// "Who's using My Media Vault?" — pick an existing profile or create one. Shown at startup (mustChoose: there is
// no Cancel; closing without a choice means the app won't proceed) and from the Home profile button (where
// Cancel just keeps the current profile). On accept, selectedId() is the chosen/created profile id.
#pragma once
#include <QDialog>

class QVBoxLayout;

class ProfileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProfileDialog(bool mustChoose, QWidget* parent = nullptr);
    QString selectedId() const { return selectedId_; }

private:
    void rebuild();         // (re)draw the list of profile rows
    void createProfile();   // prompt for a name + icon and add it (auto-selects the new profile)
    void editProfile(const QString& id); // rename / re-pick the icon of an existing profile
    // Shared name + cute-icon picker, pre-filled from (name, icon); returns true if confirmed.
    bool pickNameAndIcon(QString& name, QString& icon);

    bool mustChoose_ = false;
    QString selectedId_;
    QVBoxLayout* rows_ = nullptr;
};
