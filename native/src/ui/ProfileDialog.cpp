#include "ProfileDialog.h"
#include "../core/ProfileStore.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QFrame>

// A little set of cute avatars users can pick from for their profile.
static const char* const kProfileIcons[] = {
    "🐱", "🐶", "🦊", "🐼", "🐸", "🐵", "🦄", "🤖",
    "👾", "🐙", "🐯", "🦉", "🐧", "🐨", "🦁", "🐰",
    "🐝", "🦖", "🐢", "🍄", "⭐", "🌈", "🍀", "🎮"
};

ProfileDialog::ProfileDialog(bool mustChoose, QWidget* parent)
    : QDialog(parent), mustChoose_(mustChoose)
{
    setWindowTitle(tr("Who's using My Media Vault?"));
    setMinimumWidth(360);

    auto* v = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("Select a profile to continue, or create a new one."), this);
    heading->setWordWrap(true);
    v->addWidget(heading);

    rows_ = new QVBoxLayout();
    v->addLayout(rows_);

    auto* create = new QPushButton(tr("＋  Create New Profile"), this);
    connect(create, &QPushButton::clicked, this, &ProfileDialog::createProfile);
    v->addWidget(create);

    if (!mustChoose_)
    {
        auto* cancel = new QPushButton(tr("Cancel"), this);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        v->addWidget(cancel);
    }

    rebuild();
}

void ProfileDialog::rebuild()
{
    while (QLayoutItem* it = rows_->takeAt(0)) { delete it->widget(); delete it; }

    const QVector<Profile> profiles = ProfileStore::list();
    const bool canDelete = profiles.size() > 1; // never delete the last remaining profile
    for (const Profile& p : profiles)
    {
        auto* row = new QHBoxLayout();
        const QString label = (p.icon.isEmpty() ? QStringLiteral("🙂") : p.icon) + QStringLiteral("   ") + p.name;
        auto* pick = new QPushButton(label, this);
        pick->setStyleSheet(QStringLiteral("text-align:left; padding-left:10px; font-size:14px;"));
        pick->setMinimumHeight(44);
        const QString id = p.id;
        connect(pick, &QPushButton::clicked, this, [this, id] { selectedId_ = id; accept(); });
        row->addWidget(pick, 1);

        auto* edit = new QPushButton(tr("✎"), this);
        edit->setFixedWidth(36);
        edit->setToolTip(tr("Edit this profile"));
        connect(edit, &QPushButton::clicked, this, [this, id] { editProfile(id); });
        row->addWidget(edit);

        if (canDelete)
        {
            auto* del = new QPushButton(tr("✕"), this);
            del->setFixedWidth(36);
            del->setToolTip(tr("Delete this profile"));
            const QString name = p.name;
            connect(del, &QPushButton::clicked, this, [this, id, name] {
                if (QMessageBox::question(
                        this, tr("Delete profile"),
                        tr("Delete “%1”? Their recent list will be removed. This can't be undone.").arg(name))
                    == QMessageBox::Yes)
                {
                    ProfileStore::remove(id);
                    rebuild();
                }
            });
            row->addWidget(del);
        }
        rows_->addLayout(row);
    }
}

bool ProfileDialog::pickNameAndIcon(QString& name, QString& icon)
{
    QDialog dlg(this);
    dlg.setWindowTitle(name.isEmpty() ? tr("New Profile") : tr("Edit Profile"));
    auto* v = new QVBoxLayout(&dlg);

    v->addWidget(new QLabel(tr("Name:"), &dlg));
    auto* nameEdit = new QLineEdit(name, &dlg);
    nameEdit->setMaxLength(24);
    v->addWidget(nameEdit);

    v->addWidget(new QLabel(tr("Pick an icon:"), &dlg));
    auto* gridHost = new QWidget(&dlg);
    auto* grid = new QGridLayout(gridHost);
    grid->setSpacing(4);

    QString chosen = icon.isEmpty() ? QString::fromUtf8(kProfileIcons[0]) : icon;
    QVector<QPushButton*> iconButtons;
    const int cols = 8, count = int(sizeof(kProfileIcons) / sizeof(kProfileIcons[0]));
    auto highlight = [&](QPushButton* sel) {
        for (QPushButton* b : iconButtons)
            b->setStyleSheet(b == sel ? QStringLiteral("font-size:20px; border:2px solid #5b8cff; border-radius:6px;")
                                      : QStringLiteral("font-size:20px; border:1px solid #555; border-radius:6px;"));
    };
    QPushButton* chosenBtn = nullptr;
    for (int i = 0; i < count; ++i)
    {
        const QString glyph = QString::fromUtf8(kProfileIcons[i]);
        auto* b = new QPushButton(glyph, gridHost);
        b->setFixedSize(42, 42);
        QObject::connect(b, &QPushButton::clicked, &dlg, [&chosen, glyph, b, highlight] { chosen = glyph; highlight(b); });
        iconButtons.push_back(b);
        if (glyph == chosen) chosenBtn = b;
        grid->addWidget(b, i / cols, i % cols);
    }
    v->addWidget(gridHost);
    highlight(chosenBtn ? chosenBtn : (iconButtons.isEmpty() ? nullptr : iconButtons.first()));

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);

    if (dlg.exec() != QDialog::Accepted) return false;
    const QString entered = nameEdit->text().trimmed();
    if (entered.isEmpty()) { QMessageBox::information(this, dlg.windowTitle(), tr("Please enter a name.")); return false; }
    name = entered;
    icon = chosen;
    return true;
}

void ProfileDialog::createProfile()
{
    QString name, icon;
    if (!pickNameAndIcon(name, icon)) return;
    const Profile p = ProfileStore::add(name, icon);
    selectedId_ = p.id; // use the freshly created profile
    accept();
}

void ProfileDialog::editProfile(const QString& id)
{
    Profile target;
    for (const Profile& p : ProfileStore::list())
        if (p.id == id) { target = p; break; }
    if (target.id.isEmpty()) return;

    QString name = target.name, icon = target.icon;
    if (!pickNameAndIcon(name, icon)) return;
    ProfileStore::update(id, name, icon);
    rebuild(); // reflect the new name/icon in the list
}
