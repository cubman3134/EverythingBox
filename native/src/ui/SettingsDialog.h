#pragma once
#include <QDialog>
#include <QHash>

class QComboBox;

// Emulator settings: which libretro core each system uses, and per-core options (resolution/BIOS/...).
// Input remapping lives in its own window (ControllerRemapDialog), reached from the main toolbar.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void save();

private:
    // Harvest the selected core's options (loading it headlessly, downloading first if needed) and show
    // a per-core options editor. Values persist keyed by core name.
    void editOptions(const QString& systemId);

    QHash<QString, QComboBox*> combos_; // systemId -> core combo
};
