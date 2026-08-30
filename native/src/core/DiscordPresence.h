// THE ONLY I/O IN THE PRESENCE FEATURE — Discord's local IPC socket.
//
// Discord's desktop client listens on a local socket named discord-ipc-0 through discord-ipc-9 (ten, because
// several Discord builds can run at once and each takes the first free one). On Windows those are named
// pipes; elsewhere they are unix sockets under the runtime dir. QLocalSocket speaks both, which is the whole
// reason this needs no SDK and no third-party dependency.
//
// DISCORD NOT RUNNING IS THE NORMAL CASE, NOT A FAILURE. Most users will not have it open, and many who do
// will start it after us. So a failed connect is silent: no notification, no log line, no error state — just
// a backoff and another attempt. The status line in Settings is the ONLY place this is ever surfaced.
//
// A NAMED HAZARD. A QLocalSocket destroyed inside its own readyRead emission is heap corruption, not a
// warning: Qt's frames resume on the socket after the slot returns, which is past where a QPointer can help.
// That was probe_uitest's "flaky" 3% rc=139. Nothing here deletes or reconnects the socket from inside one of
// its own slots — teardown is always deferred past the emission (see resetSocket).
#pragma once
#include "Presence.h"
#include "PresenceTransport.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QLocalSocket;

// The pure half of the wire, split out so probe_presence can assert the framing and the payload shape
// without a socket, a Discord client or an application id.
namespace DiscordIpc
{
    // [opcode u32 LE][length u32 LE][payload]. Both fields are LITTLE-endian and both are 4 bytes; getting
    // either wrong makes Discord wait forever for a payload that never arrives, which looks like a hang
    // rather than an error.
    QByteArray  encodeFrame(int opcode, const QByteArray& json);

    // The `activity` object of a SET_ACTIVITY command. Empty fields are OMITTED rather than sent empty.
    QJsonObject activityJson(const Presence::Activity& a);
}

class DiscordPresence : public QObject, public PresenceTransport
{
    Q_OBJECT
public:
    // `applicationId` is the Discord application's snowflake. It is public information (it names the app on
    // every card, never the user) and is compiled in. An EMPTY id disables the transport entirely — nothing
    // connects and nothing is sent — so a build made before the application exists is inert, not broken.
    explicit DiscordPresence(const QString& applicationId, QObject* parent = nullptr);
    ~DiscordPresence() override;

    void setActivity(const Presence::Activity& activity) override;
    void clearActivity() override;
    bool connected() const override;

signals:
    void connectionChanged();

private:
    void tryConnect();
    void onConnected();
    void onReadyRead();
    void onSocketGone();          // disconnect OR error: both mean "start over"
    void resetSocket();           // deferred teardown — never called synchronously from a socket slot
    void send(int opcode, const QJsonObject& payload);
    void flushPending();

    QString       appId_;
    QLocalSocket* socket_    = nullptr;
    int           nextPipe_  = 0;      // which discord-ipc-N to try next
    bool          handshook_ = false;
    QByteArray    inbox_;              // partial frames accumulate here
    QTimer        retry_;
    int           backoffMs_ = kBackoffMinMs;

    bool               hasPending_     = false;   // an activity that arrived while disconnected
    Presence::Activity pending_;
    bool               pendingIsClear_ = false;

    static constexpr int kPipeCount    = 10;
    static constexpr int kBackoffMinMs = 5000;
    static constexpr int kBackoffMaxMs = 60000;
};
