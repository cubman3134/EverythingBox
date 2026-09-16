#include "AddonConfigurePanel.h"

#include "../addons/AddonManager.h"
#include "../addons/StremioTranslate.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

AddonConfigurePanel::AddonConfigurePanel(AddonManager* mgr, const QString& addonName,
                                         const std::function<void()>& onInstalled, QWidget* window)
    : NavOverlay(window), mgr_(mgr), onInstalled_(onInstalled)
{
    auto* v = new QVBoxLayout(panel());
    v->setContentsMargins(22, 18, 22, 18);
    v->setSpacing(12);

    auto* title = new QLabel(addonName.isEmpty() ? tr("Configure on website")
                                                 : tr("Configure %1 on website").arg(addonName), panel());
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
    title->setWordWrap(true);
    title->setMaximumWidth(520);
    v->addWidget(title);

    auto* msg = new QLabel(tr("Finish on the website, then copy the add-on's install link and come back."), panel());
    msg->setWordWrap(true);
    msg->setMaximumWidth(520);
    v->addWidget(msg);

    link_ = new QLineEdit(panel());
    link_->setObjectName(QStringLiteral("addonConfigureLink"));
    link_->setPlaceholderText(tr("Install link"));
    link_->setFocusPolicy(Qt::StrongFocus);
    link_->setMinimumWidth(420);
    v->addWidget(link_);

    status_ = new QLabel(panel());
    status_->setWordWrap(true);
    status_->setMaximumWidth(520);
    status_->setFocusPolicy(Qt::NoFocus);
    v->addWidget(status_);

    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    row->addStretch(1);
    paste_ = new QPushButton(tr("Paste link"), panel());
    install_ = new QPushButton(tr("Install"), panel());
    auto* close = new QPushButton(tr("Close"), panel());
    for (QPushButton* b : { paste_, install_, close }) { b->setFocusPolicy(Qt::StrongFocus); row->addWidget(b); }
    v->addLayout(row);

    connect(paste_, &QPushButton::clicked, this, [this] { pasteFromClipboard(false); });
    connect(install_, &QPushButton::clicked, this, [this] { install(); });
    connect(close, &QPushButton::clicked, this, [this] { dismiss(-1); });

    if (mgr_)
        connect(mgr_, &AddonManager::remoteSourceResult, this, [this](bool ok, const QString& message) {
            if (!pending_) return;   // someone else's add (a registry install elsewhere) — not ours to report
            pending_ = false;
            status_->setText(message);
            if (!ok) return;          // the failure stays on the card; the user can fix the link and retry
            // Close on the NEXT turns, never inside this emission (#28/#211: dismiss() deleteLaters the card).
            QTimer::singleShot(900, this, [this] {
                const std::function<void()> done = onInstalled_;
                dismiss(1);
                if (done) done();
            });
        });

    // The clipboard is read only now that the panel is open (never before), and only to pre-fill — nothing
    // installs until Install is pressed. Deferred behind the base class's own first layout/selection pass, so
    // the focus set here is the one that sticks.
    QTimer::singleShot(0, this, [this] {
        pasteFromClipboard(true);
        if (!link_->text().isEmpty()) install_->setFocus(Qt::OtherFocusReason);
        else paste_->setFocus(Qt::OtherFocusReason);
    });
}

void AddonConfigurePanel::pasteFromClipboard(bool quietWhenEmpty)
{
    const QClipboard* cb = QApplication::clipboard();
    const QString link = StremioTranslate::installLinkFromText(cb ? cb->text() : QString());
    if (link.isEmpty())
    {
        if (!quietWhenEmpty)
            status_->setText(tr("The clipboard doesn't hold an add-on install link yet. On the website, copy the "
                                "link its Install button offers, then press Paste link again."));
        return;
    }
    link_->setText(link);
    status_->setText(tr("Install link ready from %1.").arg(QUrl(link).host()));
    if (!quietWhenEmpty) install_->setFocus(Qt::OtherFocusReason);
}

void AddonConfigurePanel::install()
{
    if (!mgr_ || pending_) return;
    const QString typed = link_->text().trimmed();
    // A stremio:// link typed or pasted by hand is converted the same way the clipboard check converts it; any
    // other text goes to addRemoteSource as typed, which validates it (a bare base URL is also accepted there).
    const QString link = StremioTranslate::installLinkFromText(typed).isEmpty()
                             ? typed : StremioTranslate::installLinkFromText(typed);
    if (link.isEmpty()) { status_->setText(tr("Paste the add-on's install link first.")); return; }
    pending_ = true;
    status_->setText(tr("Fetching add-on…"));
    mgr_->addRemoteSource(link);   // async -> remoteSourceResult (above)
}

QString AddonConfigurePanel::describe() const
{
    const QString host = link_->text().isEmpty() ? QString() : QUrl(link_->text().trimmed()).host();
    return QStringLiteral("link=%1 status=%2 focus=%3")
        .arg(host.isEmpty() ? QStringLiteral("-") : host, status_->text(), NavOverlay::describe());
}
