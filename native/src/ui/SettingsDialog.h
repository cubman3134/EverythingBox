#pragma once
#include <QDialog>
#include <QHash>

class QComboBox;
class QStackedWidget;
class QLabel;

// Emulator settings: which libretro core each system uses, and per-core options (resolution/BIOS/...).
// Input remapping lives in its own window (ControllerRemapDialog), reached from the main toolbar.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

signals:
    // Issue #98: a supports_no_game custom core's Run button. The dialog has no launcher of its own, so
    // it asks; MainWindow wires this to GameLauncher::runCoreWithoutContent where it builds the dialog.
    void runCustomCoreRequested(const QString& coreRef, const QString& title);

private slots:
    void save();

private:
    // Harvest the selected core's options (loading it headlessly, downloading first if needed) and show
    // a per-core options editor. Values persist keyed by core name.
    void editOptions(const QString& systemId);

    // Issue #98's classic twin of MainWindow::presentCustomCores — the second settings builder, without which
    // the escape hatch would be unreachable on the classic layout. A transient page pushed onto the same
    // stack the per-core options editor uses (in place, no popup window).
    void editCustomCores();
    void loadCustomCorePicked();   // native file dialog -> CustomCoreInstall::loadFromFile + the one-time notice
    void sayCustomCore(const QString& text);   // write one message to BOTH status lines

    QHash<QString, QComboBox*> combos_; // systemId -> core combo
    QStackedWidget* stack_ = nullptr;   // page 0 = cores list, page 1 = (transient) per-core options editor
    QLabel* status_ = nullptr;          // inline error line (e.g. a core that won't load), no popup
    // Issue #98: the custom-core page's OWN status line. `status_` lives on page 0, so a message written
    // there while page 1 is showing is invisible - and a load that fails would then look like a button
    // that does nothing. Both are written, so the message is visible wherever the user ends up.
    QLabel* customStatus_ = nullptr;
};
