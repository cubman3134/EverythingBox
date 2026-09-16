// "Configure on website…" (issue #80): the waiting panel shown after EB hands an add-on's configure page to the
// browser. A NavOverlay — an in-window child of the main window, never a dialog or a top-level window — so it
// is the same card on the themed and the classic layout, D-pad reachable, and Back closes it.
//
// The flow it finishes: the user picks options on the website, copies the add-on's install link, and comes
// back. The panel offers Paste link (reading the clipboard only now, when the user is here to see it), and
// when the clipboard ALREADY holds an install link at the moment the panel opens it is pre-filled and Install
// is focused. Nothing installs until Install is pressed. Install goes through AddonManager::addRemoteSource,
// so a link for an add-on that is already installed replaces it in place (the same id, new options).
//
// The link itself is never logged and never described to the UI-test channel beyond its host: a configured
// add-on's URL routinely carries the options the user just chose, debrid keys included.
#pragma once
#include "nav/NavOverlay.h"
#include <QPointer>

class AddonManager;
class QLabel;
class QLineEdit;
class QPushButton;

class AddonConfigurePanel : public NavOverlay
{
    Q_OBJECT
public:
    // `addonName` titles the card (empty = a generic title). `onInstalled` runs once, after a successful
    // install, just before the panel closes — the caller refreshes whatever surface it came from.
    AddonConfigurePanel(AddonManager* mgr, const QString& addonName, const std::function<void()>& onInstalled,
                        QWidget* window = nullptr);

    // UI-test channel: "link=<host or empty> status=<text> focus=<button>" — never the link itself.
    QString describe() const override;

private:
    void pasteFromClipboard(bool quietWhenEmpty);
    void install();

    QPointer<AddonManager> mgr_;
    std::function<void()> onInstalled_;
    QLineEdit* link_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* paste_ = nullptr;
    QPushButton* install_ = nullptr;
    bool pending_ = false;   // an Install of OURS is in flight: only then is remoteSourceResult ours to show
};
