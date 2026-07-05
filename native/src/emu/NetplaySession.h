// Two-player LAN netplay between two MyMediaVault instances (not RetroArch-compatible). Uses deterministic
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
    void stop();

    bool active() const { return active_; }
    bool isHost() const { return host_; }
    bool ready() const { return ready_; }          // handshake done -> the frame loop may run

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
    void ended(const QString& reason);

private:
    void attachSocket(QTcpSocket* s);
    void wireSocketErrors(QTcpSocket* s);          // disconnected/errorOccurred handlers (shared by all modes)
    void onReadyRead();
    void onRelayHandshake();                       // consume the relay's PAIRED/NOHOST/BUSY line, then start the session
    void connectToRelay(const QString& relayHost, quint16 relayPort, const QByteArray& verbLine);
    void sendFrame(quint8 type, const QByteArray& payload);
    void onConnected();                            // client side: connected, waiting for HELLO+STATE
    void beginAsHost();                            // host side: peer connected, send HELLO+STATE, start

    QTcpServer* server_ = nullptr;
    QTcpSocket* sock_ = nullptr;
    QByteArray rx_;
    QByteArray relayBuf_;                          // accumulates the relay's handshake line before the session starts
    bool active_ = false, host_ = false, ready_ = false, awaitingPair_ = false;
    QHash<quint32, quint16> remoteInputs_;
};
