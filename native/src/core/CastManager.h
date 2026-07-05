// Cast the currently-playing stream to a TV/dongle on the LAN. Supports two ecosystems behind one picker:
//   - Chromecast / Google TV: mDNS discovery (_googlecast._tcp) + the CASTV2 protocol over TLS (length-
//     prefixed protobuf frames carrying JSON messages) to LAUNCH the default media receiver and LOAD a URL.
//   - DLNA / UPnP MediaRenderers: SSDP discovery (UDP multicast) + SOAP AVTransport (SetAVTransportURI+Play).
// In both cases the device fetches the media URL itself, so this works for addon/debrid http(s) streams (a
// local file would need to be served over HTTP first — not supported here).
#pragma once
#include <QObject>
#include <QString>
#include <QList>
#include <QByteArray>

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
    bool isCasting() const { return casting_; }
    QString currentDeviceName() const { return castingName_; }

    // Begin casting a stream URL to a device. title/mime are best-effort metadata.
    void cast(const CastDevice& device, const QString& url, const QString& title, const QString& mime);
    void stopCasting();                    // stop the active cast (best-effort)

signals:
    void devicesChanged();
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
    void onMdnsDatagram();
    static bool parseMdns(const QByteArray& pkt, QString& ipOut, QString& nameOut);
    // ---- Chromecast CASTV2 session ----
    void ccConnectAndLoad(const CastDevice& d, const QString& url, const QString& title, const QString& mime);
    void ccSend(const QString& destId, const QString& ns, const QString& payloadJson);
    void ccOnReadyRead();
    void ccTeardown();

    QList<CastDevice> devices_;
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
