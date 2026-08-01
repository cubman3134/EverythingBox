// Two-player LAN netplay between two EverythingBox instances (not RetroArch-compatible). Uses deterministic
// input-delay lockstep: both peers run the same libretro core + ROM, sync an initial save state, then exchange
// one input packet per frame and advance in step. The host is player 0, the client player 1. Requires a
// deterministic core (most 2D cores) and identical core options/BIOS on both sides.
//
// This class owns the TCP connection + the handshake + the remote-input buffer. The frame loop (generating
// local input, injecting both players' inputs, advancing the core) lives in RetroView, which drives it via
// remoteInput()/sendLocalInput() and the serialize/loadState callbacks used to sync the starting state.
#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <functional>

class QTcpServer;
class QTcpSocket;

class NetplaySession : public QObject
{
    Q_OBJECT
public:
    explicit NetplaySession(QObject* parent = nullptr);
    ~NetplaySession() override;

    void host(quint16 port);                       // LAN: listen for a peer directly
    void join(const QString& host, quint16 port);  // LAN / UPnP-direct: connect straight to a host
    // Online via a relay both peers reach outbound (no port-forwarding). Same room `code` on both sides; the relay
    // pairs them and then just pipes bytes, so the lockstep protocol runs over it unchanged.
    void hostViaRelay(const QString& relayHost, quint16 relayPort, const QString& code);
    void joinViaRelay(const QString& relayHost, quint16 relayPort, const QString& code);
    // "Both" online mode: host listens for a DIRECT (UPnP-forwarded) connection AND on the relay at once — the
    // first peer to PROVE it holds the room code wins, and only then is the other path dropped. The joiner tries
    // the host's direct endpoint first (if any), then falls back to the relay. Lowest latency when UPnP works;
    // always connects thanks to the relay. A peer that just opens the direct port and says nothing (a scanner,
    // a stale client) is hung up on, not adopted.
    void hostOnline(quint16 localPort, const QString& relayHost, quint16 relayPort, const QString& code);
    void joinOnline(const QString& relayHost, quint16 relayPort, const QString& code,
                    const QString& directIp, quint16 directPort);
    void stop();

    bool active() const { return active_; }
    bool isHost() const { return host_; }
    bool ready() const { return ready_; }          // handshake done -> the frame loop may run
    // The port the direct server actually bound, 0 if it isn't listening. Pass 0 to hostOnline() to let the OS
    // pick a free one and read it back here — the endpoint has to be advertised to the joiner either way, and a
    // hard-coded port is one more thing two instances on a machine can collide on.
    quint16 directPort() const;
    // How long an unproven peer on the direct port has to announce itself before the host hangs up on it. Public
    // so a test can assert that an ACCEPTED direct session is still alive on the far side of this deadline.
    static constexpr int kDirectGreetingTimeoutMs = 5000;

    // Game identity for the handshake mismatch check, set before host()/join().
    QString gameId;   // "<rom-basename>|<size>"
    QString coreName;
    // Host serializes its state to send at handshake; client adopts the received state. Set by RetroView.
    std::function<QByteArray()> serializeState;
    std::function<void(const QByteArray&)> applyState;

    // ---- called by the RetroView frame loop ----
    void sendLocalInput(quint32 frame, quint16 buttons);      // queue+send this peer's input for a frame
    bool remoteInput(quint32 frame, quint16& out) const;      // true if the peer's input for `frame` has arrived
    void pruneBefore(quint32 frame);                          // drop buffered remote inputs older than `frame`

signals:
    void status(const QString& message);
    void started();                                // handshake complete: begin lockstep at frame 0
    // The relay confirmed our room is registered, so a joiner using this code can now be paired to us. Until this
    // fires a JOIN with the same code races the registration and gets NOHOST — anything that has to wait for the
    // room to exist must wait for THIS, not for a timer.
    void roomOpen();
    void ended(const QString& reason);

private:
    void attachSocket(QTcpSocket* s);
    void wireSocketErrors(QTcpSocket* s);          // disconnected/errorOccurred handlers (shared by all modes)
    void onReadyRead();
    void onRelayHandshake();                       // consume the relay's HOSTED/PAIRED/NOHOST/BUSY lines, then start
    // Consume whole relay lines from `buf`: 1 = PAIRED (session bytes that trailed it land in `leftover`),
    // -1 = refused (`err` holds the message), 0 = need more data. HOSTED is absorbed here (emits roomOpen()).
    int takeRelayLines(QByteArray& buf, QByteArray& leftover, QString& err);
    void vetDirectPeer(QTcpSocket* s, const QString& code);   // prove an inbound direct peer before committing
    void connectToRelay(const QString& relayHost, quint16 relayPort, const QByteArray& verbLine);
    void sendFrame(quint8 type, const QByteArray& payload);
    void onConnected();                            // client side: connected, waiting for HELLO+STATE
    void beginAsHost();                            // host side: peer connected, send HELLO+STATE, start

    QTcpServer* server_ = nullptr;
    QTcpSocket* sock_ = nullptr;
    QTcpSocket* relaySock_ = nullptr;              // host's relay socket while it races the direct server (first wins)
    QByteArray rx_;
    QByteArray relayBuf_;                          // accumulates the relay's handshake line before the session starts
    bool active_ = false, host_ = false, ready_ = false, awaitingPair_ = false;
    QHash<quint32, quint16> remoteInputs_;
};
