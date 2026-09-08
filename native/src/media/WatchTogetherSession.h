// "Watch together" (issue #86), the SOCKET half — one TCP connection between two EverythingBox installs,
// carrying WatchTogether's newline-delimited control messages and NOTHING ELSE.
//
// It is deliberately thin. Every decision — what a message means, who may pause, how far out of sync is too
// far, what happens when someone stalls — is in WatchTogether.{h,cpp}, which is pure and probe-driven. This
// class owns the connection, the relay rendezvous, and the beacon timer, and it hands each inbound message to
// the Room and each of the Room's answers to the socket.
//
// TRANSPORTS, all three of them netplay's:
//   * direct LAN  — one instance listens, the other connects. No relay, no rendezvous, lowest latency.
//   * relay       — both connect OUTBOUND to tools/netplay-relay.py and are paired by a room code. The relay
//                   is a byte pipe: it never sees a message it understands, and at roughly ten control
//                   messages a minute plus a beacon it costs it nothing.
// The relay address is netplay's own setting. The two features share a server on purpose: a user reading a
// code off a TV cannot be asked to know which kind of room it is.
//
// NO MEDIA CROSSES THIS SOCKET. What is shared is an identity — PlayOn::ItemRef, #143's type, reused
// verbatim — and each participant resolves its own stream with its own addons and its own accounts. The
// encoder in WatchTogether.cpp enforces that on every byte written here.
#pragma once
#include "WatchTogether.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

class WatchTogetherSession : public QObject
{
    Q_OBJECT
public:
    explicit WatchTogetherSession(QObject* parent = nullptr);
    ~WatchTogetherSession() override;

    // The beacon cadence. One a second is plenty: the drift controller corrects at 3%, so a beacon lost to a
    // hiccup costs at most 30 ms of catch-up, and a watch party's whole traffic budget is a few hundred bytes
    // a minute.
    static constexpr int kBeaconMs = 1000;

    void hostDirect(quint16 port, const QString& code, const QString& selfId, const QString& selfName);
    void joinDirect(const QString& hostAddr, quint16 port, const QString& code,
                    const QString& selfId, const QString& selfName);
    void hostViaRelay(const QString& relayHost, quint16 relayPort, const QString& code,
                      const QString& selfId, const QString& selfName);
    void joinViaRelay(const QString& relayHost, quint16 relayPort, const QString& code,
                      const QString& selfId, const QString& selfName);
    void leave();                       // says Bye, then stops

    bool active() const { return room_.active(); }
    bool isHost() const { return room_.isHost(); }
    bool connected() const { return paired_; }
    QString code() const { return room_.code(); }
    QList<WatchTogether::Participant> participants() const { return room_.participants(); }
    bool hostPaused() const { return room_.hostPaused(); }
    double hostPosition() const { return room_.hostPosition(); }
    WatchTogether::BufferPolicy bufferPolicy() const { return room_.bufferPolicy(); }
    void setBufferPolicy(WatchTogether::BufferPolicy p);
    const WatchTogether::Room& room() const { return room_; }

    // ---- host ----
    void shareItem(const PlayOn::ItemRef& ref, double positionSec, bool paused);
    void setTransport(bool paused, double positionSec);   // the local user pressed play/pause/seek
    void notePosition(double positionSec);                // the clock ticked; feeds the beacon

    // ---- guest ----
    void requestPause(bool paused);
    void requestSeek(double positionSec);
    void reportBuffering(bool buffering, double positionSec);
    void reportUnresolved(const QString& why);
    void reportResolved();

    // The drift answer for where WE are now, against the last beacon and HOW LONG AGO WE RECEIVED IT. The age
    // is measured on the local clock, deliberately: the host's clock reading rides along in the message but is
    // never differenced against ours, because two machines' clocks are not comparable and pretending they are
    // is how a controller ends up chasing a clock skew instead of a drift.
    // `nudging` is the CALLER's state: only the thing that owns the player's rate knows whether it is
    // currently off 1.0, and passing it in each time is what makes the hysteresis work across calls.
    WatchTogether::DriftDecision evaluateDrift(double localPositionSec, bool nudging) const;

signals:
    void status(const QString& message);
    void ended(const QString& reason);
    void joined();                                        // the pipe is up and Hello has been sent
    void roomOpen();                                      // relay host: the code can now be handed out
    void participantsChanged();
    // Guest: play THIS (a reference — resolve it yourself), and where the host is.
    void itemProposed(const PlayOn::ItemRef& ref, double positionSec, bool paused);
    // Either side: the authoritative transport moved. On the host this also fires for an automatic
    // buffering hold, which is a pause this machine did not ask for and its player has to follow.
    void transportChanged(bool paused, double positionSec);

private:
    void attach(QTcpSocket* s);
    void connectToRelay(const QString& relayHost, quint16 relayPort, const QByteArray& verbLine);
    void onRelayHandshake();
    void onReadyRead();
    void onDirectPeer();
    void listenDirect();                                  // the listening half of hostDirect, re-runnable
    void beginSession();                                  // paired: send Hello, start the beacon
    void send(const WatchTogether::Message& m);
    void send(const QList<WatchTogether::Message>& ms);
    void deliver(const WatchTogether::Message& m);
    void stopSocket();

    WatchTogether::Room room_;
    QTcpServer* server_ = nullptr;
    QTcpSocket* sock_ = nullptr;
    QTimer* beacon_ = nullptr;
    QByteArray rx_;
    QByteArray relayBuf_;
    // How this instance opened the room, kept so it can be RE-OPENED when a guest leaves. Without it the
    // host's listening socket is gone the moment the first guest proves itself (it has to be — see
    // onReadyRead), and a guest that leaves and comes back finds nothing to connect to. MEASURED: a rejoin
    // simply never arrived, with no error on either side.
    enum class HostMode { None, Direct, Relay };
    HostMode hostMode_ = HostMode::None;
    quint16 hostPort_ = 0;
    QString relayHost_;
    quint16 relayPort_ = 0;
    QString selfName_;
    void rearmHost();

    QString pendingCode_;
    bool awaitingPair_ = false;
    bool paired_ = false;
    // When the last beacon LANDED HERE, on this machine's clock. See evaluateDrift.
    qint64 beaconSeenMs_ = 0;
};
