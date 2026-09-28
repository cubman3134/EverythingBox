#include "DeepLinkChannel.h"
#include "DeepLink.h"

#include <QCoreApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QUrl>
#include <memory>

#ifdef Q_OS_MACOS
#  include <QFileOpenEvent>   // QtGui: the macOS URL hand-over
#endif
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace
{
constexpr int kIdleSocketMs = 3000;   // a connection that never finishes its one line is dropped after this

bool uitestOn() { return qEnvironmentVariableIsSet("EB_UITEST"); }
} // namespace

QString DeepLinkChannel::activeScheme()
{
    return DeepLink::schemeFor(uitestOn(), qEnvironmentVariable("EB_UITEST_DEEPLINK_SCHEME"));
}

QString DeepLinkChannel::registrationScheme()
{
    return DeepLink::registrationSchemeFor(uitestOn(), qEnvironmentVariable("EB_UITEST_DEEPLINK_SCHEME"));
}

QString DeepLinkChannel::serverName()
{
    // The user profile, as the OS spells it for this account (case folded on Windows, where it is not
    // significant), and the login session: one name per account per session.
    QString user = QDir::homePath();
    QString session;
#ifdef Q_OS_WIN
    user = user.toLower();
    DWORD sid = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sid)) session = QString::number(sid);
#endif
    QString test;
    if (uitestOn())
    {
        test = qEnvironmentVariable("EB_UITEST_PIPE");
        if (test.isEmpty()) test = QStringLiteral("uitest");
    }
    return DeepLink::handoffServerName(user, session, test);
}

namespace
{
DeepLinkChannel::Handoff sendFrame(const QString& name, const QByteArray& frame, int timeoutMs)
{
    QLocalSocket s;
    s.connectToServer(name);
    if (!s.waitForConnected(timeoutMs)) return DeepLinkChannel::Handoff::NoInstance;
#ifdef Q_OS_WIN
    // This process was started by the user's click, so it may hand the foreground on: the running window can
    // then come forward to show its card instead of flashing in the taskbar.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    s.write(frame);
    if (!s.waitForBytesWritten(timeoutMs)) return DeepLinkChannel::Handoff::NoInstance;
    QByteArray ack;
    while (!ack.contains('\n') && ack.size() < 16 && s.waitForReadyRead(timeoutMs)) ack += s.readAll();
    s.disconnectFromServer();
    if (ack.startsWith("NO")) return DeepLinkChannel::Handoff::Refused;
    // "OK", or a running instance that took the line and has not answered in time. Starting a SECOND full app
    // for the same link would be worse than a late card, so both count as delivered.
    return DeepLinkChannel::Handoff::Delivered;
}
} // namespace

DeepLinkChannel::Handoff DeepLinkChannel::sendToRunning(const QString& name, const QString& manifestUrl, int timeoutMs)
{
    const QByteArray frame = DeepLink::encodeHandoff(manifestUrl);
    if (frame.isEmpty()) return Handoff::Refused;   // never send what we would not accept
    return sendFrame(name, frame, timeoutMs);
}

DeepLinkChannel::Handoff DeepLinkChannel::sendRefusalToRunning(const QString& name, int refusal, int timeoutMs)
{
    const QByteArray frame = DeepLink::encodeRefusalHandoff(DeepLink::Refusal(refusal));
    if (frame.isEmpty()) return Handoff::NoInstance;
    return sendFrame(name, frame, timeoutMs);
}

DeepLinkChannel::DeepLinkChannel(QObject* parent) : QObject(parent)
{
#ifdef Q_OS_MACOS
    // A filter on the application object sees every event in the app, so it is installed only where the
    // FileOpen event it waits for exists.
    if (QCoreApplication* app = QCoreApplication::instance()) app->installEventFilter(this);
#endif
}

DeepLinkChannel::~DeepLinkChannel()
{
#ifdef Q_OS_MACOS
    if (QCoreApplication* app = QCoreApplication::instance()) app->removeEventFilter(this);
#endif
}

bool DeepLinkChannel::handOffFromArguments(const QStringList& args)
{
    const QString scheme = activeScheme();
    for (int i = 1; i < args.size(); ++i)
    {
        if (!DeepLink::isLinkArgument(args.at(i), scheme)) continue;
        const DeepLink::Result r = DeepLink::parse(args.at(i), scheme);
        if (!r.ok())
        {
            // Refused here, before anything else runs. If an app is already up it is told WHY (and nothing else:
            // no URL crosses) and this process ends; otherwise this process starts and says so itself.
            if (sendRefusalToRunning(serverName(), int(r.refusal)) != Handoff::NoInstance) return true;
            heldRefusals_ << int(r.refusal);
            return false;
        }
        if (sendToRunning(serverName(), r.manifestUrl) != Handoff::NoInstance) return true;
        heldUrl_ = r.manifestUrl;   // nobody running: this process is the app, and handles it once ready
        return false;
    }
    return false;
}

bool DeepLinkChannel::listen(const QString& name)
{
    if (server_) return server_->isListening();
    // Somebody already answers on this name: a second copy of the app started without a link. It must not
    // take the name over — Windows would give the pipe two servers and hand each link to either, and on Unix
    // removing the "stale" socket file would cut the first copy off — so the first copy keeps receiving.
    {
        QLocalSocket owner;
        owner.connectToServer(name);
        if (owner.waitForConnected(300)) { owner.abort(); return false; }
    }
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);   // this account only
    if (!server_->listen(name))
    {
        // Nobody answered above, so what blocks the name is a socket file a crashed copy left behind (Unix).
        QLocalServer::removeServer(name);
        if (!server_->listen(name)) return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = server_->nextPendingConnection()) accept(s);
    });
    return true;
}

bool DeepLinkChannel::listening() const { return server_ && server_->isListening(); }

void DeepLinkChannel::accept(QLocalSocket* socket)
{
    auto buf = std::make_shared<QByteArray>();
    auto done = std::make_shared<bool>(false);
    QPointer<QLocalSocket> guard(socket);
    // Every exit from here tears the socket down on a LATER turn, never inside its own readyRead emission
    // (the #211 family: a socket collected inside its own emission is freed under Qt's frames).
    auto finish = [guard] {
        QTimer::singleShot(0, guard, [guard] {
            if (!guard) return;
            guard->disconnectFromServer();
            guard->deleteLater();
        });
    };
    QTimer::singleShot(kIdleSocketMs, socket, [guard, done, finish] {
        if (!guard || *done) return;
        *done = true;
        guard->abort();
        finish();
    });
    connect(socket, &QLocalSocket::readyRead, this, [this, guard, buf, done, finish] {
        if (!guard || *done) return;
        buf->append(guard->read(DeepLink::kMaxHandoffBytes + 1 - buf->size()));
        const bool line = buf->contains('\n');
        const bool over = buf->size() > DeepLink::kMaxHandoffBytes;
        if (!line && !over) return;   // wait for the rest of the one line
        *done = true;
        // Exactly one line and nothing after it; anything else is refused whole.
        DeepLink::Result r;
        if (over)                                         r.refusal = DeepLink::Refusal::TooLong;
        else if (buf->indexOf('\n') + 1 != buf->size())   r.refusal = DeepLink::Refusal::BadCharacters;
        else                                              r = DeepLink::decodeHandoff(*buf);
        guard->write(r.ok() ? "OK\n" : "NO\n");
        guard->flush();
        finish();
        // Delivered on a later turn too: whatever a link does next (a fetch, a card) must not run inside this
        // socket's emission.
        const QString url = r.manifestUrl;
        const int refusal = int(r.refusal);
        QTimer::singleShot(0, this, [this, url, refusal] {
            if (refusal == int(DeepLink::Refusal::None)) deliverAccepted(url);
            else deliverRefused(refusal);
        });
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
}

void DeepLinkChannel::offer(const QString& rawLink, const QString& scheme)
{
    const DeepLink::Result r = DeepLink::parse(rawLink, scheme);
    if (r.ok()) deliverAccepted(r.manifestUrl);
    else deliverRefused(int(r.refusal));
}

void DeepLinkChannel::deliverAccepted(const QString& manifestUrl)
{
    heldUrl_ = manifestUrl;   // the newest link wins; an older one still held is superseded
    if (ready_) QTimer::singleShot(0, this, [this] { flush(); });
}

void DeepLinkChannel::deliverRefused(int refusal)
{
    if (heldRefusals_.size() < 4) heldRefusals_ << refusal;
    if (ready_) QTimer::singleShot(0, this, [this] { flush(); });
}

void DeepLinkChannel::setReady()
{
    ready_ = true;
    QTimer::singleShot(0, this, [this] { flush(); });
}

void DeepLinkChannel::flush()
{
    const QVector<int> refusals = heldRefusals_;
    heldRefusals_.clear();
    const QString url = heldUrl_;
    heldUrl_.clear();
    for (int r : refusals) emit linkRefused(r);
    if (!url.isEmpty()) emit linkArrived(url);
}

bool DeepLinkChannel::eventFilter(QObject* watched, QEvent* event)
{
#ifdef Q_OS_MACOS
    // macOS hands a clicked URL to the running (or just-launched) app as a FileOpen event, not an argument.
    if (event->type() == QEvent::FileOpen)
    {
        const QUrl u = static_cast<QFileOpenEvent*>(event)->url();
        const QString scheme = activeScheme();
        if (u.isValid() && u.scheme().compare(scheme, Qt::CaseInsensitive) == 0)
        {
            // The event's QUrl has already been parsed once; take its ENCODED form so parse() does the one
            // decode itself, exactly as it does for a command-line argument.
            offer(QString::fromLatin1(u.toEncoded()), scheme);
            return true;
        }
    }
#endif
    return QObject::eventFilter(watched, event);
}
