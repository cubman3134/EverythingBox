// The live half of the remote-control API (issue #76): a QTcpServer that accepts a LAN connection, reads one
// tiny HTTP request, routes it through the PURE RemoteApi core, dispatches the resulting Command onto the
// running app, and writes an HTTP response. It follows the same discipline as UiTestServer / CloudSync's
// loopback: the socket work is isolated here, the decisions live in a pure unit a probe can drive.
//
// SAFETY, and it is the whole reason this file is small:
//   * OFF BY DEFAULT. Nothing here binds a port until MainWindow, gated on Settings::remoteControlEnabled(),
//     calls start(). A disabled install has no listening socket at all.
//   * CONTROL-ONLY. The vocabulary is exactly RemoteApi's three routes (state / player / input). There is no
//     path that reaches the filesystem, no eval, no library browse — an unknown path is a 404, never a file.
//   * Bound to all interfaces (so a phone on the LAN can reach it) ONLY while enabled; stop() tears the
//     listener down the moment the setting is turned off.
//   * A per-connection read cap (kMaxRequestBytes) so a malicious or broken client cannot make the app buffer
//     an unbounded request.
#pragma once
#include <QHash>
#include <QObject>
#include "RemoteApi.h"
#include <functional>

class QTcpServer;
class QTcpSocket;

class RemoteServer : public QObject
{
    Q_OBJECT
public:
    // The app-side seam. `state` reads a snapshot of what is playing (for GET /state); `dispatch` performs a
    // Player/Input command on the GUI thread and returns whether it was applied. Both are set by MainWindow;
    // an unset hook degrades to an empty state / a 503, never a crash.
    struct Hooks
    {
        std::function<RemoteApi::PlayerStateView()>     state;
        std::function<bool(const RemoteApi::Command&)>  dispatch;
    };

    explicit RemoteServer(QObject* parent = nullptr);
    ~RemoteServer() override;

    void setHooks(const Hooks& h) { hooks_ = h; }

    // Bind to all interfaces on `port` (called ONLY when the setting is on). Returns true when it is listening.
    // A failure to bind (port in use, permission) leaves the server not listening and returns false.
    bool start(quint16 port);
    void stop();
    bool isListening() const;
    quint16 port() const { return port_; }

    // The reachable LAN URL for the user to open on a phone: "http://<first non-loopback IPv4>:<port>", or a
    // loopback URL if no LAN address is found. Pure best-effort presentation; never empty.
    static QString lanUrl(quint16 port);

private:
    void onReadyRead(QTcpSocket* sock);
    void finish(QTcpSocket* sock, const QByteArray& responseBytes);

    QTcpServer* server_ = nullptr;
    Hooks       hooks_;
    quint16     port_ = 0;
    QHash<QTcpSocket*, QByteArray> buffers_;   // per-connection accumulation until a full request has arrived

    static constexpr int kMaxRequestBytes = 64 * 1024;  // cap a single request; over this -> 413 + close
};
