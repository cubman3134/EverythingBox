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
#include <QPointer>
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

ProfileDialog::ProfileDialog(bool mustChoose, std::function<bool(const QString&)> unlockGate, QWidget* parent)
    : QDialog(parent), mustChoose_(mustChoose), unlockGate_(std::move(unlockGate))
{
    setWindowTitle(tr("Who's using EverythingBox?"));
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

// THE gate for the two escape routes on this page. Copies the callable before invoking it because the call
// spins a nested overlay loop on the main window, inside which this dialog can be torn down (a navigation
// away, the panel host closing); the QPointer then stops us walking back into a freed object. A missing
// gate is treated as REFUSED — a dead button is a bug, an ungated Delete on a locked profile is the defect
// this exists to close, and the constructor makes the missing case unreachable anyway.
bool ProfileDialog::allowAction(const QString& id)
{
    auto gate = unlockGate_;
    if (!gate) return false;
    QPointer<ProfileDialog> self(this);
    const bool ok = gate(id);
    return ok && !self.isNull();
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
        // GATED: the edit page carries the "Passcode…" row, so an ungated ✎ on a locked profile is a way to
        // remove the code and walk in. The themed row menu gates the same action; this is not a second copy
        // of the policy, it is the same MainWindow::profilePasscodeUnlock reached through unlockGate_.
        connect(edit, &QPushButton::clicked, this, [this, id] { if (allowAction(id)) editProfile(id); });
        row->addWidget(edit);

        if (canDelete)
        {
            auto* del = new QPushButton(tr("✕"), this);
            del->setFixedWidth(36);
            del->setToolTip(tr("Delete this profile"));
            const QString name = p.name;
            connect(del, &QPushButton::clicked, this, [this, id, name] {
                // GATED, and BEFORE the confirm page is even built: deleting a locked profile does not open
                // it, but it does defeat "this profile cannot be entered" for anyone who only wanted it
                // gone — the same escape the themed row menu closes. At the STARTUP picker this was the
                // whole lock: anyone could delete a passcode-protected profile outright.
                if (!allowAction(id)) return;
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
                               const std::function<void(const QString&, const QString&)>& onAccept,
                               const QString& passcodeFor)
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

    // Passcode row (edit only — a profile being created has no id to key the hash to). The label reports the
    // current state; the flow itself is MainWindow's, raised by the signal. NOT bound to this page's OK: the
    // chooser applies immediately, exactly as it does in the themed builder, so the two agree.
    if (!passcodeFor.isEmpty())
    {
        auto* pcRow = new QHBoxLayout();
        auto* pcLabel = new QLabel(page);
        auto* pcBtn = new QPushButton(tr("Passcode…"), page);
        pcBtn->setMinimumHeight(32);
        // QPointer, not a raw capture: the flow the receiver runs is a nested event loop, and this page can
        // be torn down inside it (Back, a profile deletion, a navigation away). A callback that wrote to a
        // freed QLabel would crash in the receiver's code, nowhere near this line.
        QPointer<QLabel> safeLabel(pcLabel);
        auto refresh = [safeLabel, passcodeFor] {
            if (!safeLabel) return;
            safeLabel->setText(ProfileStore::hasPasscode(passcodeFor) ? tr("A passcode is set.")
                                                                      : tr("No passcode set."));
        };
        refresh();
        pcLabel->setStyleSheet(QStringLiteral("color:#888;font-size:12px;"));
        QObject::connect(pcBtn, &QPushButton::clicked, page, [this, passcodeFor, refresh] {
            emit passcodeRequested(passcodeFor, refresh);
        });
        pcRow->addWidget(pcBtn);
        pcRow->addWidget(pcLabel, 1);
        v->addLayout(pcRow);
    }

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
    }, /*passcodeFor*/ id);
}
