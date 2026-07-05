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

    void host(quint16 port);                       // listen for a peer
    void join(const QString& host, quint16 port);  // connect to a host
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
    void onReadyRead();
    void sendFrame(quint8 type, const QByteArray& payload);
    void onConnected();                            // client side: connected, waiting for HELLO+STATE
    void beginAsHost();                            // host side: peer connected, send HELLO+STATE, start

    QTcpServer* server_ = nullptr;
    QTcpSocket* sock_ = nullptr;
    QByteArray rx_;
    bool active_ = false, host_ = false, ready_ = false;
    QHash<quint32, quint16> remoteInputs_;
};
