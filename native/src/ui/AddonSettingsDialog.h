// Per-addon settings: renders a form from the addon's declared manifest "settings" (API keys, base URLs,
// toggles, ...), pre-filled from the stored values, and saves them. The addon's script reads them at
// runtime via getConfig(key). Values are stored per addon in mymediavault.ini (plaintext - desktop-local).
#pragma once
#include <QDialog>
#include <QHash>
#include "../addons/AddonModels.h"

class QWidget;

class AddonSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AddonSettingsDialog(const AddonManifest& manifest, QWidget* parent = nullptr);

private slots:
    void save();

private:
    AddonManifest manifest_;
    QHash<QString, QWidget*> fields_; // setting key -> editor widget (QLineEdit / QCheckBox)
};
