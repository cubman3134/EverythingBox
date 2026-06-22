#pragma once
#include <QDialog>
#include <QHash>

class QComboBox;
class Gamepad;
class Keymap;

// Per-system settings: which libretro core each system uses, per-core options (resolution/BIOS/...),
// and controller + keyboard button remapping.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    // pad/keys may be null (input remapping is then unavailable).
    explicit SettingsDialog(Gamepad* pad = nullptr, Keymap* keys = nullptr, QWidget* parent = nullptr);

private slots:
    void save();

private:
    // Harvest the selected core's options (loading it headlessly, downloading first if needed) and show
    // a per-core options editor. Values persist keyed by core name.
    void editOptions(const QString& systemId);
    void editControllerMapping();

    Gamepad* pad_ = nullptr;
    Keymap*  keys_ = nullptr;
    QHash<QString, QComboBox*> combos_; // systemId -> core combo
};
