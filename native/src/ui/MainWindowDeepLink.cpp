// everythingbox:// deep links (issue #80, proposal 1), the MainWindow half — its own translation unit, off
// MainWindow.cpp, for the same reason as MainWindowPlayOn.cpp (#143/#186).
//
// A deep link is an UNAUTHENTICATED INSTALL VECTOR: any web page, chat message or program on this machine can
// hand the OS one. So the path from a link to an installed add-on has exactly one door, and it is a person
// pressing Install on a card that says what is being installed and from where:
//
//   DeepLinkChannel (argv / a second process over the per-user socket / macOS FileOpen) -> DeepLink::parse
//     -> handleDeepLink: re-validated; refused on a kids profile with a parental PIN (Settings needs that PIN,
//        so a link must not be the way round it); held until the current profile is past its passcode (the
//        landing gate's own rule); the window comes forward
//     -> AddonManager::fetchRemotePreview: the manifest, fetched with a size cap and a deadline, persisting
//        nothing
//     -> a nav-kit NavConfirm: name, host, resources, catalog types, "This link came from outside
//        EverythingBox", Install / Cancel with the FOCUS ON CANCEL. A fetch failure is a plain sentence
//        (NetErrorText) on a card of its own, with nothing to install.
//     -> Install -> AddonManager::addRemoteSource, so #80's same-id-replaces-in-place rule applies.
//
// One card at a time (DeepLink::Inbox): a second link closes the open card and its fetch supersedes the older
// one; an Install press counts only on the card that is open, once.
//
// The log names the manifest by LogSafeText::url() only — scheme, host and file name. A configured add-on's
// manifest URL carries the user's options, debrid keys among them.
#include "MainWindow.h"

#include <QDateTime>
#include <QFile>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "../addons/AddonManager.h"
#include "../core/AppPaths.h"
#include "../core/DeepLink.h"
#include "../core/DeepLinkChannel.h"
#include "../core/DeepLinkRegistration.h"
#include "../core/LogSafeText.h"
#include "../core/ProfileStore.h"
#include "../core/Settings.h"
#include "LibraryView.h"
#include "nav/NavOverlay.h"

namespace {
void linkLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  deep link: ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

// The per-window state, as a child object rather than MainWindow members, so the feature stays in this file.
class DeepLinkState : public QObject
{
public:
    explicit DeepLinkState(QObject* parent) : QObject(parent) { setObjectName(QStringLiteral("eb.deeplinks")); }

    DeepLink::Inbox          inbox;
    QPointer<QNetworkReply>  fetch;          // the newest link's manifest fetch, while in flight
    QPointer<NavConfirm>     card;           // the open card, if any
    QString                  parked;         // a link waiting for a profile to be entered (newest wins)
    QTimer*                  parkTimer = nullptr;
    bool                     installPending = false;   // an Install of OURS is in flight
};

DeepLinkState* stateOf(QObject* window)
{
    // Found by its name, and only ever created here, so the cast is exact (a moc-less class has no qobject_cast).
    if (QObject* s = window->findChild<QObject*>(QStringLiteral("eb.deeplinks"), Qt::FindDirectChildrenOnly))
        return static_cast<DeepLinkState*>(s);
    return new DeepLinkState(window);
}
} // namespace

void MainWindow::startDeepLinks(DeepLinkChannel* channel)
{
    if (!channel) return;
    DeepLinkState* s = stateOf(this);
    connect(channel, &DeepLinkChannel::linkArrived, this, &MainWindow::handleDeepLink);
    connect(channel, &DeepLinkChannel::linkRefused, this, &MainWindow::handleDeepLinkRefusal);
    if (addons_)
        connect(addons_.get(), &AddonManager::remoteSourceResult, s, [this, s](bool ok, const QString& message) {
            if (!s->installPending) return;   // someone else's add (the Add-ons screen, a configure card)
            s->installPending = false;
            linkLog(ok ? QStringLiteral("installed") : QStringLiteral("install failed"));
            notify(message);
            if (ok && library_) library_->refreshSources();
        });
    linkLog(QStringLiteral("answering %1:// links (%2)")
                .arg(DeepLinkChannel::activeScheme(),
                     channel->listening() ? QStringLiteral("handoff listening") : QStringLiteral("handoff NOT listening")));
    // A moved exe re-registers here: apply() compares the stored command with this exe and rewrites it only
    // when they differ.
    if (Settings::deepLinksEnabled())
        linkLog(QStringLiteral("startup registration: ") + DeepLinkRegistration::apply(true).detail);
    channel->setReady();
}

void MainWindow::handleDeepLinkRefusal(int refusal)
{
    const auto r = DeepLink::Refusal(refusal);
    linkLog(QStringLiteral("refused (%1)").arg(QString::fromLatin1(DeepLink::refusalCode(r))));
    notify(DeepLink::refusalText(r));
}

void MainWindow::handleDeepLink(const QString& manifestUrl)
{
    // Every way in has validated it already; it is checked again here because this is the door.
    const DeepLink::Result v = DeepLink::validateManifestUrl(manifestUrl);
    if (!v.ok()) { handleDeepLinkRefusal(int(v.refusal)); return; }
    const QString url = v.manifestUrl;
    const QString safe = LogSafeText::url(url);
    DeepLinkState* s = stateOf(this);

    if (!Settings::deepLinksEnabled())
    {
        linkLog(QStringLiteral("ignored, links are off: %1").arg(safe));
        notify(tr("A link offered an add-on from %1, but opening everythingbox:// links is off. You can turn it "
                  "on in Settings.").arg(QUrl(url).host()));
        return;
    }
    if (!addons_) return;

    // A kids profile with a parental PIN cannot open Settings without that PIN, so it cannot add an add-on;
    // a link must not be the way round it. Refused outright (asking for the PIN here would mean a nested
    // event loop inside a signal delivery — the #28/#211 family).
    if (Settings::hasParentalPin() && ProfileStore::current().restricted)
    {
        linkLog(QStringLiteral("refused on a restricted profile: %1").arg(safe));
        notify(tr("Add-on links can't be opened on this profile. Switch to another profile and open the link again."));
        return;
    }
    // Not past the profile passcode: the landing gate's own rule (ensureActiveProfileUnlocked) — the current
    // profile either has no passcode or its code was entered this session. Until then the newest link waits.
    const auto profileOpen = [this] {
        const Profile p = ProfileStore::current();
        return !p.id.isEmpty() && (p.id == enteredProfile_ || p.passHash.isEmpty());
    };
    if (!profileOpen())
    {
        if (s->parked.isEmpty()) linkLog(QStringLiteral("held until a profile is open: %1").arg(safe));
        s->parked = url;
        if (!s->parkTimer)
        {
            s->parkTimer = new QTimer(s);
            s->parkTimer->setInterval(1000);
            connect(s->parkTimer, &QTimer::timeout, this, [s, profileOpen, this] {
                if (s->parked.isEmpty() || !profileOpen()) return;
                s->parkTimer->stop();
                const QString next = s->parked;
                s->parked.clear();
                handleDeepLink(next);
            });
        }
        s->parkTimer->start();
        return;
    }

    linkLog(QStringLiteral("received %1").arg(safe));
    if (isMinimized()) showNormal();
    raise();
    activateWindow();

    const DeepLink::Inbox::Arrival a = s->inbox.arrive();   // FIRST: anything older is now stale
    if (a.closeOpenCard && s->card) s->card->dismiss(-1);    // a second link replaces the open card
    if (s->fetch) s->fetch->abort();                         // its callback lands on a stale ticket and drops
    notify(tr("Checking the add-on at %1…").arg(QUrl(url).host()), 2500);

    const quint64 ticket = a.ticket;
    s->fetch = addons_->fetchRemotePreview(url, [this, s, ticket, url, safe](const AddonManager::RemotePreview& p) {
        if (!s->inbox.presentCard(ticket)) return;   // superseded by a newer link
        hideNotice();
        if (!p.ok)
        {
            linkLog(QStringLiteral("fetch failed for %1").arg(safe));
            auto* card = new NavConfirm(tr("Couldn't open that add-on link"), p.error, { tr("OK") }, 0, this);
            card->setObjectName(QStringLiteral("deepLinkCard"));
            s->card = card;
            connect(card, &NavOverlay::closed, s, [s, ticket](int) { s->inbox.cardClosed(ticket); });
            return;
        }
        linkLog(QStringLiteral("confirming %1 (%2) from %3").arg(p.id, p.name.left(60), safe));
        // Install first and Cancel second, as every confirmation in the app reads — but the FOCUS starts on
        // Cancel, so a stray Enter (or a pad's A pressed as the card appears) installs nothing.
        auto* card = new NavConfirm(DeepLink::confirmTitle(p.name),
                                    DeepLink::confirmMessage(p.host, p.resources, p.catalogTypes, p.permissions),
                                    { tr("Install"), tr("Cancel") }, 1, this);
        card->setObjectName(QStringLiteral("deepLinkCard"));
        s->card = card;
        // closed() is emitted from inside the button's own click; everything below only starts async work.
        connect(card, &NavOverlay::closed, s, [this, s, ticket, url, safe](int result) {
            const bool install = result == 0 && s->inbox.takeInstall(ticket);
            s->inbox.cardClosed(ticket);
            if (!install) { linkLog(QStringLiteral("cancelled %1").arg(safe)); return; }
            linkLog(QStringLiteral("install confirmed for %1").arg(safe));
            s->installPending = true;
            addons_->addRemoteSource(url);   // #80: a same-id add-on is replaced in place
        });
    });
}

void MainWindow::setDeepLinksFromUi(bool on)
{
    Settings::setDeepLinksEnabled(on);
    const DeepLinkRegistration::Outcome o = DeepLinkRegistration::apply(on);
    linkLog(QStringLiteral("setting turned %1: %2").arg(on ? QStringLiteral("on") : QStringLiteral("off"), o.detail));
    if (on)
        notify(o.ok ? tr("everythingbox:// links will now open here, and always ask before installing anything.")
                    : tr("Couldn't register everythingbox:// links on this computer."));
    else
        notify(o.ok ? tr("everythingbox:// links are off.")
                    : tr("everythingbox:// links are off, but the old registration couldn't be removed."));
}
