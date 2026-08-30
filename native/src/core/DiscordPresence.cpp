#include "DiscordPresence.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QUuid>

namespace {

// Discord's RPC opcodes.
constexpr int kOpHandshake = 0;
constexpr int kOpFrame     = 1;
constexpr int kOpClose     = 2;
constexpr int kOpPing      = 3;
constexpr int kOpPong      = 4;

constexpr int kHeaderBytes = 8;

// The socket's name for pipe N. On Windows QLocalSocket maps a bare name onto \\.\pipe\<name>; elsewhere the
// socket is a file in the runtime dir, and Snap and Flatpak installs put it one level deeper.
QStringList candidateNames(int n)
{
    // TEST-ONLY OVERRIDE, the EB_UITEST_PIPE precedent. probe_presence stands a QLocalServer of its own in
    // for Discord, and it must not be able to reach - or be reached by - a Discord that happens to be running
    // on the developer's machine. Unset in every real run, where the names below are Discord's own.
    const QString testBase = qEnvironmentVariable("EB_DISCORD_IPC_NAME");
    if (!testBase.isEmpty()) return { QStringLiteral("%1-%2").arg(testBase).arg(n) };

    const QString leaf = QStringLiteral("discord-ipc-%1").arg(n);
#ifdef Q_OS_WIN
    return { leaf };
#else
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty()) base = qEnvironmentVariable("TMPDIR");
    if (base.isEmpty()) base = QStringLiteral("/tmp");
    return { QDir(base).filePath(leaf),
             QDir(base).filePath(QStringLiteral("snap.discord/") + leaf),
             QDir(base).filePath(QStringLiteral("app/com.discordapp.Discord/") + leaf) };
#endif
}

void putLE32(QByteArray& out, quint32 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8)  & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 24) & 0xFF));
}

quint32 readLE32(const QByteArray& b, int at)
{
    return quint32(quint8(b.at(at)))
         | (quint32(quint8(b.at(at + 1))) << 8)
         | (quint32(quint8(b.at(at + 2))) << 16)
         | (quint32(quint8(b.at(at + 3))) << 24);
}

} // namespace

QByteArray DiscordIpc::encodeFrame(int opcode, const QByteArray& json)
{
    QByteArray out;
    out.reserve(kHeaderBytes + json.size());
    putLE32(out, quint32(opcode));
    putLE32(out, quint32(json.size()));
    out.append(json);
    return out;
}

QJsonObject DiscordIpc::activityJson(const Presence::Activity& a)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), a.type);
    // Empty fields are OMITTED. Discord accepts an absent key where it rejects an empty string, and a
    // rejected update is dropped whole and without a word.
    if (!a.details.isEmpty()) o.insert(QStringLiteral("details"), a.details);
    if (!a.state.isEmpty())   o.insert(QStringLiteral("state"),   a.state);

    if (a.startUnix > 0 || a.endUnix > 0) {
        QJsonObject ts;
        if (a.startUnix > 0) ts.insert(QStringLiteral("start"), qint64(a.startUnix));
        if (a.endUnix   > 0) ts.insert(QStringLiteral("end"),   qint64(a.endUnix));
        o.insert(QStringLiteral("timestamps"), ts);
    }

    QJsonObject assets;
    if (!a.largeImage.isEmpty()) assets.insert(QStringLiteral("large_image"), a.largeImage);
    if (!a.largeText.isEmpty())  assets.insert(QStringLiteral("large_text"),  a.largeText);
    if (!a.smallImage.isEmpty()) assets.insert(QStringLiteral("small_image"), a.smallImage);
    if (!a.smallText.isEmpty())  assets.insert(QStringLiteral("small_text"),  a.smallText);
    if (!assets.isEmpty()) o.insert(QStringLiteral("assets"), assets);

    if (!a.buttons.isEmpty()) {
        QJsonArray arr;
        for (const auto& b : a.buttons) {
            QJsonObject j;
            j.insert(QStringLiteral("label"), b.first);
            j.insert(QStringLiteral("url"),   b.second);
            arr.append(j);
        }
        o.insert(QStringLiteral("buttons"), arr);
    }
    return o;
}

DiscordPresence::DiscordPresence(const QString& applicationId, QObject* parent)
    : QObject(parent), appId_(applicationId)
{
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &DiscordPresence::tryConnect);
    if (!appId_.isEmpty()) tryConnect();
}

DiscordPresence::~DiscordPresence() = default;

bool DiscordPresence::connected() const { return handshook_; }

void DiscordPresence::tryConnect()
{
    if (appId_.isEmpty() || socket_) return;

    socket_ = new QLocalSocket(this);
    connect(socket_, &QLocalSocket::connected,     this, &DiscordPresence::onConnected);
    connect(socket_, &QLocalSocket::readyRead,     this, &DiscordPresence::onReadyRead);
    connect(socket_, &QLocalSocket::disconnected,  this, &DiscordPresence::onSocketGone);
    connect(socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { onSocketGone(); });

    // Failing to find a socket here is ORDINARY — Discord is simply not running.
    socket_->connectToServer(candidateNames(nextPipe_).first());
}

void DiscordPresence::onConnected()
{
    QJsonObject hs;
    hs.insert(QStringLiteral("v"), 1);
    hs.insert(QStringLiteral("client_id"), appId_);
    send(kOpHandshake, hs);
}

void DiscordPresence::onReadyRead()
{
    if (!socket_) return;
    inbox_.append(socket_->readAll());

    while (inbox_.size() >= kHeaderBytes) {
        const quint32 op  = readLE32(inbox_, 0);
        const quint32 len = readLE32(inbox_, 4);
        if (quint32(inbox_.size()) < quint32(kHeaderBytes) + len) break;  // a partial frame; await the rest
        const QByteArray payload = inbox_.mid(kHeaderBytes, int(len));
        inbox_.remove(0, kHeaderBytes + int(len));

        if (op == kOpPing) {
            // An unanswered ping makes Discord drop us.
            send(kOpPong, QJsonDocument::fromJson(payload).object());
        }
        else if (op == kOpClose) {
            onSocketGone();
            return;                     // the socket is being torn down; stop touching inbox_
        }
        else if (op == kOpFrame && !handshook_) {
            // The first frame after a successful handshake is READY.
            handshook_ = true;
            backoffMs_ = kBackoffMinMs;
            emit connectionChanged();
            flushPending();
        }
    }
}

void DiscordPresence::onSocketGone()
{
    const bool was = handshook_;
    handshook_ = false;
    inbox_.clear();
    resetSocket();

    // WALK ALL TEN PIPES BEFORE BACKING OFF. Discord takes the first free socket, so a second Discord build
    // (or a stale one) puts the live client on discord-ipc-1 or higher. Backing off between each attempt
    // would make that user wait a minute per pipe — up to ten minutes to be found at all — and it would look
    // exactly like presence being broken. A whole round of ten failed connects costs nothing, so the backoff
    // belongs BETWEEN rounds, not between pipes.
    nextPipe_ = (nextPipe_ + 1) % kPipeCount;
    if (nextPipe_ != 0) { retry_.start(0); return; }   // same round: try the next pipe at once

    if (was) {
        // A client that was connected and has just quit is likely to come back soon, so this restarts at the
        // floor rather than inheriting a backoff grown while Discord was closed.
        backoffMs_ = kBackoffMinMs;
        retry_.start(kBackoffMinMs);
        emit connectionChanged();
        return;
    }
    backoffMs_ = qMin(backoffMs_ * 2, kBackoffMaxMs);
    retry_.start(backoffMs_);
}

void DiscordPresence::resetSocket()
{
    if (!socket_) return;
    QLocalSocket* doomed = socket_;
    socket_ = nullptr;
    doomed->disconnect(this);       // no further slots on a socket we are done with
    doomed->abort();
    // NEVER a plain delete, and NEVER deleteLater alone. This can be reached from inside readyRead, and
    // deleteLater only defers past the INNERMOST delivery — Qt's own frames resume on the socket after the
    // slot returns. Hopping through the event loop with a zero-timer puts the destruction after every frame
    // that could still touch it. This is the probe_uitest rc=139 lesson; see the header.
    QTimer::singleShot(0, doomed, [doomed] { doomed->deleteLater(); });
}

void DiscordPresence::send(int opcode, const QJsonObject& payload)
{
    if (!socket_ || socket_->state() != QLocalSocket::ConnectedState) return;
    const QByteArray json  = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QByteArray frame = DiscordIpc::encodeFrame(opcode, json);
    // ONE write. A frame split across two writes breaks the pipe.
    socket_->write(frame);
    socket_->flush();
}

void DiscordPresence::setActivity(const Presence::Activity& activity)
{
    pending_        = activity;
    pendingIsClear_ = false;
    hasPending_     = true;
    if (handshook_) flushPending();
}

void DiscordPresence::clearActivity()
{
    pending_        = Presence::Activity{};
    pendingIsClear_ = true;
    hasPending_     = true;
    if (handshook_) flushPending();
}

void DiscordPresence::flushPending()
{
    if (!hasPending_ || !handshook_) return;

    QJsonObject args;
    args.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
    // A null activity is how SET_ACTIVITY says "show nothing".
    if (pendingIsClear_) args.insert(QStringLiteral("activity"), QJsonValue());
    else                 args.insert(QStringLiteral("activity"), DiscordIpc::activityJson(pending_));

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"),   QStringLiteral("SET_ACTIVITY"));
    cmd.insert(QStringLiteral("args"),  args);
    cmd.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    send(kOpFrame, cmd);
    hasPending_ = false;
}
