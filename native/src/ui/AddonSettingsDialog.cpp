#include "AddonSettingsDialog.h"
#include "../addons/AddonContext.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QDialogButtonBox>

AddonSettingsDialog::AddonSettingsDialog(const AddonManifest& manifest, QWidget* parent)
    : QDialog(parent), manifest_(manifest)
{
    const QString name = manifest_.name.isEmpty() ? manifest_.id : manifest_.name;
    setWindowTitle(tr("%1 — Settings").arg(name));

    auto* v = new QVBoxLayout(this);

    if (manifest_.settings.isEmpty())
    {
        v->addWidget(new QLabel(tr("This addon has no configurable settings."), this));
        auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(box, &QDialogButtonBox::accepted, this, &QDialog::reject);
        v->addWidget(box);
        return;
    }

    auto* form = new QFormLayout();
    for (const AddonSetting& s : manifest_.settings)
    {
        const QString stored = AddonContext::readConfig(manifest_.id, s.key, s.defaultValue);

        QWidget* editor = nullptr;
        if (s.type == QStringLiteral("checkbox"))
        {
            auto* cb = new QCheckBox(this);
            cb->setChecked(stored.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || stored == QStringLiteral("1"));
            editor = cb;
        }
        else
        {
            auto* le = new QLineEdit(stored, this);
            if (s.type == QStringLiteral("password"))
            {
                le->setEchoMode(QLineEdit::Password);
                le->setClearButtonEnabled(true);
            }
            else if (s.type == QStringLiteral("number"))
            {
                le->setInputMethodHints(Qt::ImhDigitsOnly);
            }
            editor = le;
        }

        if (!s.description.isEmpty())
            editor->setToolTip(s.description);
        fields_.insert(s.key, editor);
        form->addRow(s.label.isEmpty() ? s.key : s.label, editor);
    }
    v->addLayout(form);

    auto* note = new QLabel(
        tr("Credentials are stored on this device (plaintext in goliath.ini) and are only sent where the "
           "addon’s script chooses to use them."),
        this);
    note->setWordWrap(true);
    v->addWidget(note);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &AddonSettingsDialog::save);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(box);
}

void AddonSettingsDialog::save()
{
    for (const AddonSetting& s : manifest_.settings)
    {
        QWidget* editor = fields_.value(s.key);
        if (!editor) continue;
        QString value;
        if (auto* cb = qobject_cast<QCheckBox*>(editor))
            value = cb->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
        else if (auto* le = qobject_cast<QLineEdit*>(editor))
            value = le->text();
        AddonContext::writeConfig(manifest_.id, s.key, value);
    }
    accept();
}
