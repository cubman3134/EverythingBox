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
#include <QStackedWidget>
#include <QVector>
#include <memory>

// A little set of cute avatars users can pick from for their profile.
static const char* const kProfileIcons[] = {
    "🐱", "🐶", "🦊", "🐼", "🐸", "🐵", "🦄", "🤖",
    "👾", "🐙", "🐯", "🦉", "🐧", "🐨", "🦁", "🐰",
    "🐝", "🦖", "🐢", "🍄", "⭐", "🌈", "🍀", "🎮"
};

QStringList ProfileDialog::iconChoices()
{
    QStringList out;
    for (const char* const g : kProfileIcons) out << QString::fromUtf8(g);
    return out;
}

ProfileDialog::ProfileDialog(bool mustChoose, QWidget* parent)
    : QDialog(parent), mustChoose_(mustChoose)
{
    setWindowTitle(tr("Who's using My Media Vault?"));
    setMinimumWidth(360);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    root->addWidget(stack_);

    // Page 0: the profile list. The name/icon picker is pushed as page 1 in place (no popup window).
    auto* listPage = new QWidget(stack_);
    stack_->addWidget(listPage);
    auto* v = new QVBoxLayout(listPage);

    auto* heading = new QLabel(tr("Select a profile to continue, or create a new one."), listPage);
    heading->setWordWrap(true);
    v->addWidget(heading);

    rows_ = new QVBoxLayout();
    v->addLayout(rows_);

    auto* create = new QPushButton(tr("＋  Create New Profile"), listPage);
    connect(create, &QPushButton::clicked, this, &ProfileDialog::createProfile);
    v->addWidget(create);

    if (!mustChoose_)
    {
        auto* cancel = new QPushButton(tr("Cancel"), listPage);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        v->addWidget(cancel);
    }
    v->addStretch(1);

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
                // Inline confirm page (no popup): push it onto the stack with Delete / Cancel.
                auto* page = new QWidget(stack_);
                auto* v = new QVBoxLayout(page);
                v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Delete profile")), page));
                auto* msg = new QLabel(
                    tr("Delete “%1”? Their recent list will be removed. This can't be undone.").arg(name), page);
                msg->setWordWrap(true);
                v->addWidget(msg);
                v->addStretch(1);
                auto* box = new QDialogButtonBox(page);
                auto* confirm = box->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
                box->addButton(QDialogButtonBox::Cancel);
                v->addWidget(box);
                auto leave = [this, page] {
                    stack_->setCurrentIndex(0);
                    stack_->removeWidget(page);
                    page->deleteLater();
                };
                connect(confirm, &QPushButton::clicked, this, [this, id, leave] {
                    ProfileStore::remove(id); rebuild(); leave();
                });
                connect(box, &QDialogButtonBox::rejected, this, leave);
                stack_->addWidget(page);
                stack_->setCurrentWidget(page);
            });
            row->addWidget(del);
        }
        rows_->addLayout(row);
    }
}

void ProfileDialog::showPicker(const QString& title, const QString& name, const QString& icon,
                               const std::function<void(const QString&, const QString&)>& onAccept)
{
    auto* page = new QWidget(stack_);
    auto* v = new QVBoxLayout(page);

    v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(title), page));

    v->addWidget(new QLabel(tr("Name:"), page));
    auto* nameEdit = new QLineEdit(name, page);
    nameEdit->setMaxLength(24);
    v->addWidget(nameEdit);

    v->addWidget(new QLabel(tr("Pick an icon:"), page));
    auto* gridHost = new QWidget(page);
    auto* grid = new QGridLayout(gridHost);
    grid->setSpacing(4);

    // chosen lives on the heap so the icon-button lambdas can mutate it for the page's lifetime.
    auto chosen = std::make_shared<QString>(icon.isEmpty() ? QString::fromUtf8(kProfileIcons[0]) : icon);
    auto iconButtons = std::make_shared<QVector<QPushButton*>>();
    const int cols = 8, count = int(sizeof(kProfileIcons) / sizeof(kProfileIcons[0]));
    // padding:0 + min-width:0 override the global QPushButton padding (8px 16px), which would otherwise
    // squeeze the emoji into a sliver of the 42x42 button and crop it.
    auto highlight = [iconButtons](QPushButton* sel) {
        for (QPushButton* b : *iconButtons)
            b->setStyleSheet(b == sel
                ? QStringLiteral("font-size:22px; padding:0; min-width:0; border:2px solid #5b8cff; border-radius:6px;")
                : QStringLiteral("font-size:22px; padding:0; min-width:0; border:1px solid #555; border-radius:6px;"));
    };
    QPushButton* chosenBtn = nullptr;
    for (int i = 0; i < count; ++i)
    {
        const QString glyph = QString::fromUtf8(kProfileIcons[i]);
        auto* b = new QPushButton(glyph, gridHost);
        b->setFixedSize(42, 42);
        QObject::connect(b, &QPushButton::clicked, page, [chosen, glyph, b, highlight] { *chosen = glyph; highlight(b); });
        iconButtons->push_back(b);
        if (glyph == *chosen) chosenBtn = b;
        grid->addWidget(b, i / cols, i % cols);
    }
    v->addWidget(gridHost);
    highlight(chosenBtn ? chosenBtn : (iconButtons->isEmpty() ? nullptr : iconButtons->first()));

    auto* err = new QLabel(page);
    err->setStyleSheet(QStringLiteral("color:#c0392b;"));
    v->addWidget(err);
    v->addStretch(1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, page);
    v->addWidget(box);

    auto leave = [this, page] {
        stack_->setCurrentIndex(0); // back to the profile list
        stack_->removeWidget(page);
        page->deleteLater();
    };
    QObject::connect(box, &QDialogButtonBox::accepted, page, [nameEdit, err, chosen, onAccept, leave] {
        const QString entered = nameEdit->text().trimmed();
        if (entered.isEmpty()) { err->setText(tr("Please enter a name.")); return; }
        onAccept(entered, *chosen);
        leave();
    });
    QObject::connect(box, &QDialogButtonBox::rejected, page, leave);

    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
    nameEdit->setFocus();
}

void ProfileDialog::createProfile()
{
    showPicker(tr("New Profile"), QString(), QString(), [this](const QString& name, const QString& icon) {
        const Profile p = ProfileStore::add(name, icon);
        selectedId_ = p.id; // use the freshly created profile
        accept();
    });
}

void ProfileDialog::editProfile(const QString& id)
{
    Profile target;
    for (const Profile& p : ProfileStore::list())
        if (p.id == id) { target = p; break; }
    if (target.id.isEmpty()) return;

    showPicker(tr("Edit Profile"), target.name, target.icon, [this, id](const QString& name, const QString& icon) {
        ProfileStore::update(id, name, icon);
        rebuild(); // reflect the new name/icon in the list
    });
}
