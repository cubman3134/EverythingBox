// Cast the currently-playing stream to a TV/dongle on the LAN. Supports two ecosystems behind one picker:
//   - Chromecast / Google TV: mDNS discovery (_googlecast._tcp) + the CASTV2 protocol over TLS (length-
//     prefixed protobuf frames carrying JSON messages) to LAUNCH the default media receiver and LOAD a URL.
//   - DLNA / UPnP MediaRenderers: SSDP discovery (UDP multicast) + SOAP AVTransport (SetAVTransportURI+Play).
// In both cases the device fetches the media URL itself, so this works for addon/debrid http(s) streams (a
// local file would need to be served over HTTP first — not supported here).
//
// #143 lodges here too, and on purpose: an EverythingBox instance is a THIRD kind of playback target, found
// on the same mDNS socket by the same burst of queries. This class browses for `_everythingbox._tcp` and, when
// an advert has been set (i.e. the #76 server is on), ANSWERS for it as well -- one socket, both directions,
// because two sockets bound to 5353 in one process is a needless second thing to get wrong. It carries no
// hand-off logic: the peers it finds are handed to PlayOn::mergeTargets and everything after that lives in
// PlayOnDevice / PlayOnClient.
#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QByteArray>
#include <QElapsedTimer>
#include "PlayOnDevice.h"

class QUdpSocket;
class QSslSocket;
class QNetworkAccessManager;
class QTimer;

struct CastDevice
{
    enum Type { Chromecast, Dlna };
    Type type = Dlna;
    QString id;         // stable key (host + type)
    QString name;       // friendly name shown in the picker
    QString host;       // IPv4
    quint16 port = 0;   // Chromecast: 8009
    QString controlUrl; // DLNA: absolute AVTransport control URL
};

class CastManager : public QObject
{
    Q_OBJECT
public:
    explicit CastManager(QObject* parent = nullptr);
    ~CastManager() override;

    void startDiscovery();                 // (re)issue SSDP + mDNS queries; devices arrive via devicesChanged
    QList<CastDevice> devices() const { return devices_; }

    // ---- "Play on device" (#143) ----
    // Start/stop ANSWERING for _everythingbox._tcp. Called only when the #76 server is listening: an instance
    // with no controllable surface has nothing to advertise, and advertising it would put a row in every peer's
    // picker that fails the moment it is pressed. Setting an advert also sends one unsolicited announcement,
    // so a freshly started instance appears on a peer that is already browsing without waiting for a query.
    void setPlayOnAdvert(const PlayOn::Advert& advert);
    void clearPlayOnAdvert();
    bool isAdvertising() const { return advertising_; }
    QList<PlayOn::Peer> peers() const { return peers_; }
    bool isCasting() const { return casting_; }
    QString currentDeviceName() const { return castingName_; }

    // Begin casting a stream URL to a device. title/mime are best-effort metadata.
    void cast(const CastDevice& device, const QString& url, const QString& title, const QString& mime);
    void stopCasting();                    // stop the active cast (best-effort)

signals:
    void devicesChanged();
    void peersChanged();                   // #143: the EverythingBox peer list grew or changed
    void castStarted(const QString& deviceName);
    void castError(const QString& message);
    void castStopped();

private:
    void addOrUpdate(const CastDevice& d);
    // ---- SSDP / DLNA ----
    void sendSsdpQuery();
    void onSsdpDatagram();
    void fetchDlnaDescription(const QString& location);
    void dlnaSoap(const QString& action, const QString& xmlBody);
    // ---- mDNS / Chromecast ----
    void sendMdnsQuery();
    // ---- mDNS / EverythingBox peers (#143) ----
    void sendPlayOnQuery();
    void answerPlayOnQuery();
    void addOrUpdatePeer(const PlayOn::Peer& p);
    static quint32 firstLanIpv4();         // host-order IPv4 for the advert's A record; 0 if none
    void onMdnsDatagram();
    static bool parseMdns(const QByteArray& pkt, QString& ipOut, QString& nameOut);
    // ---- Chromecast CASTV2 session ----
    void ccConnectAndLoad(const CastDevice& d, const QString& url, const QString& title, const QString& mime);
    void ccSend(const QString& destId, const QString& ns, const QString& payloadJson);
    void ccOnReadyRead();
    void ccTeardown();

    QList<CastDevice> devices_;
    QList<PlayOn::Peer> peers_;            // #143: discovered EverythingBox instances (never this one)
    PlayOn::Advert advert_;                // what we answer with; only valid while advertising_
    bool advertising_ = false;
    QElapsedTimer lastAnswer_;             // rate limit: two instances must not answer each other in a loop
    QUdpSocket* ssdp_ = nullptr;
    QUdpSocket* mdns_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;

    bool casting_ = false;
    QString castingName_;
    CastDevice active_;                    // the device currently being cast to

    // Chromecast session state
    QSslSocket* cc_ = nullptr;
    QByteArray ccBuf_;                     // accumulates length-prefixed frames
    int ccReqId_ = 1;
    QString ccTransportId_;                // media session destination, from RECEIVER_STATUS
    QString ccSessionId_;
    QString ccPendingUrl_, ccPendingTitle_, ccPendingMime_;
    QTimer* ccHeartbeat_ = nullptr;
};
