// The SOURCE side of "Play on device" (issue #143): the half that talks to another instance's #76 HTTP
// surface. Everything it decides lives in PlayOnDevice.h — this file only carries bytes, so the whole of it
// is request/reply plumbing plus a timeout.
//
// Two rules govern the code below and neither is negotiable:
//
//   * THE TOKEN IS A CREDENTIAL. It travels in one header on one request and is never logged, never put in a
//     signal argument that reaches a log line, never included in an error string. Every failure here reports
//     QNetworkReply::errorString() or the peer's own reason text — never the request.
//   * A HAND-OFF STILL MOVES NO BYTES. It is a reference plus a position (PlayOn::Handoff); this class has
//     no notion of a stream URL and cannot acquire one, and the target resolves its own. What issue #127
//     added below is a DIFFERENT thing on the same transport and is named as such: sendBundleItem carries
//     one item's ART, which is exactly the payload the hand-off rule was written to keep off the wire, so
//     the two must not be confused. No media file, no ROM and no stream ever rides either of them.
//
// Every call is fire-and-forget with a hard timeout: a peer that has gone off the LAN must fail in seconds
// and say so, not leave a spinner up. Replies are matched by the peer id the caller passed in, so a caller
// that has moved on to another device simply ignores the late signal.
#pragma once
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <functional>
#include "LibraryBundle.h"
#include "PlayOnDevice.h"

class QNetworkAccessManager;

class PlayOnClient : public QObject
{
    Q_OBJECT
public:
    explicit PlayOnClient(QObject* parent = nullptr);

    // ---- pairing ----
    // Ask the peer to put a code on its screen. The peer answers 200 as soon as the code is displayed.
    void requestPairing(const PlayOn::Peer& peer);
    // Send the code the user read off the target. On success `paired` carries the token to store.
    void redeemPairing(const PlayOn::Peer& peer, const QString& code);

    // ---- hand-off ----
    // POST /open with the reference + position. `token` is the stored pairing credential.
    void handOff(const PlayOn::Peer& peer, const QString& token, const PlayOn::Handoff& h);

    // ---- remote mode ----
    void pollState(const PlayOn::Peer& peer);
    void sendPlayerCommand(const PlayOn::Peer& peer, const QByteArray& body);

    // ---- "Continue on this device" ----
    void pullState(const PlayOn::Peer& peer);

    // ---- the library transfer (issue #127) ----
    // The SAME transport, the same peer and the same paired credential: this is not a second connection to a
    // second service, it is two more requests on the one #76 listener #143 already authenticates against.
    // fetchInventory asks what the target's art cache holds; sendBundleItem posts ONE item's art. One item
    // per request, so a transfer is resumable by construction — an interrupted run simply re-diffs.
    void fetchInventory(const PlayOn::Peer& peer, const QString& token);
    void sendBundleItem(const PlayOn::Peer& peer, const QString& token, const QString& itemId,
                        const QByteArray& payload);

    // How long a peer has to answer before we call it gone.
    static constexpr int kTimeoutMs = 4000;
    // A bundle is megabytes over a LAN, not a control message: it gets its own budget. Still bounded, so a
    // peer that goes off the network mid-transfer fails in seconds rather than leaving a spinner up.
    static constexpr int kBundleTimeoutMs = 60000;

signals:
    void pairingOffered(const QString& peerId);                       // the code is now on the peer's screen
    void paired(const QString& peerId, const QString& token);         // CREDENTIAL: store it, never log it
    void pairingFailed(const QString& peerId, const QString& message);

    void handedOff(const QString& peerId);
    void handOffRefused(const QString& peerId, const QString& message);   // the 409/403/401 text, ready to show

    void stateArrived(const QString& peerId, const PlayOn::RemoteView& view);
    void pullArrived(const QString& peerId, const PlayOn::Pull& pull);

    // #127. `ok` false means the peer did not answer or refused; `message` is then what to show.
    void inventoryArrived(const QString& peerId, const QList<LibraryBundle::Entry>& items,
                          bool ok, const QString& message);
    // One item's answer: `result` is the receipt's word ("landed" / "kept" / "current" / "refused"),
    // `message` the peer's reason when it is not a plain success.
    void bundleItemDone(const QString& peerId, const QString& itemId, bool ok,
                        const QString& result, const QString& message);

private:
    QString base(const PlayOn::Peer& peer) const;
    // One place that issues a request, arms the timeout and hands the caller (status, body, ok).
    void post(const PlayOn::Peer& peer, const QString& path, const QByteArray& body, const QString& token,
              std::function<void(int, const QByteArray&, bool)> done);
    void post(const PlayOn::Peer& peer, const QString& path, const QByteArray& body, const QString& token,
              int timeoutMs, std::function<void(int, const QByteArray&, bool)> done);
    void get(const PlayOn::Peer& peer, const QString& path, const QString& token,
             std::function<void(int, const QByteArray&, bool)> done);

    QNetworkAccessManager* nam_ = nullptr;
};

// Registered so the two payload structs can cross a queued connection if a caller ever needs one; a
// direct connection would not require it, and a signal that silently drops on a queued connect is a
// remote that stops updating for reasons nothing reports.
Q_DECLARE_METATYPE(PlayOn::RemoteView)
Q_DECLARE_METATYPE(PlayOn::Pull)
Q_DECLARE_METATYPE(QList<LibraryBundle::Entry>)
