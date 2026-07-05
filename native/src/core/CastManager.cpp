#include "CastManager.h"

#include <QUdpSocket>
#include <QSslSocket>
#include <QSslError>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QTimer>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

namespace {
constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr quint16 kSsdpPort = 1900;
constexpr const char* kMdnsAddr = "224.0.0.251";
constexpr quint16 kMdnsPort = 5353;

// Cast namespaces + the default media receiver app id.
constexpr const char* NS_CONNECTION = "urn:x-cast:com.google.cast.tp.connection";
constexpr const char* NS_HEARTBEAT  = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr const char* NS_RECEIVER   = "urn:x-cast:com.google.cast.receiver";
constexpr const char* NS_MEDIA      = "urn:x-cast:com.google.cast.media";
constexpr const char* DEFAULT_APP   = "CC1AD845";

// ---- minimal protobuf wire helpers (CastMessage is a flat message of strings + two varint enums) ----
QByteArray pbVarint(quint64 v)
{
    QByteArray o;
    do { quint8 b = v & 0x7F; v >>= 7; if (v) b |= 0x80; o.append(char(b)); } while (v);
    return o;
}
QByteArray pbString(int field, const QByteArray& s)
{
    QByteArray o = pbVarint(quint64(field) << 3 | 2); o += pbVarint(quint64(s.size())); o += s; return o;
}
QByteArray pbVarField(int field, quint64 v) { return pbVarint(quint64(field) << 3) + pbVarint(v); }

// CastMessage: 1=protocol_version(varint) 2=source_id 3=destination_id 4=namespace 5=payload_type(varint) 6=payload_utf8
QByteArray encodeCast(const QString& src, const QString& dst, const QString& ns, const QString& payload)
{
    QByteArray m;
    m += pbVarField(1, 0);                       // CASTV2_1_0
    m += pbString(2, src.toUtf8());
    m += pbString(3, dst.toUtf8());
    m += pbString(4, ns.toUtf8());
    m += pbVarField(5, 0);                        // payload_type = STRING
    m += pbString(6, payload.toUtf8());
    return m;
}
// Pull the namespace (field 4) and payload_utf8 (field 6) back out of an incoming CastMessage.
void decodeCast(const QByteArray& m, QString& ns, QString& payload)
{
    int i = 0; const int n = m.size();
    auto rv = [&]() -> quint64 { quint64 r = 0; int s = 0; while (i < n) { quint8 b = quint8(m[i++]); r |= quint64(b & 0x7F) << s; if (!(b & 0x80)) break; s += 7; } return r; };
    while (i < n)
    {
        const quint64 tag = rv(); const int field = int(tag >> 3), wire = int(tag & 7);
        if (wire == 0) { rv(); }
        else if (wire == 2)
        {
            const quint64 len = rv(); if (i + int(len) > n) break;
            const QByteArray s = m.mid(i, int(len)); i += int(len);
            if (field == 4) ns = QString::fromUtf8(s);
            else if (field == 6) payload = QString::fromUtf8(s);
        }
        else break;
    }
}

QString xmlEscape(const QString& s)
{
    QString o = s; o.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;");
    return o;
}
} // namespace

CastManager::CastManager(QObject* parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

CastManager::~CastManager() { ccTeardown(); }

void CastManager::addOrUpdate(const CastDevice& d)
{
    for (CastDevice& e : devices_)
        if (e.id == d.id)
        {
            // Fill in fields as later probes resolve them (e.g. DLNA control URL).
            if (!d.name.isEmpty()) e.name = d.name;
            if (!d.controlUrl.isEmpty()) e.controlUrl = d.controlUrl;
            if (d.port) e.port = d.port;
            emit devicesChanged();
            return;
        }
    devices_.push_back(d);
    emit devicesChanged();
}

void CastManager::startDiscovery()
{
    // Devices persist across calls (addOrUpdate dedups), so re-issuing queries just refreshes/extends the list
    // rather than wiping it — callers can prime discovery when playback starts and again when the menu opens.
    if (!ssdp_)
    {
        ssdp_ = new QUdpSocket(this);
        ssdp_->bind(QHostAddress(QHostAddress::AnyIPv4), 0, QUdpSocket::ShareAddress);
        connect(ssdp_, &QUdpSocket::readyRead, this, &CastManager::onSsdpDatagram);
    }
    if (!mdns_)
    {
        mdns_ = new QUdpSocket(this);
        mdns_->bind(QHostAddress(QHostAddress::AnyIPv4), kMdnsPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
            if ((iface.flags() & QNetworkInterface::CanMulticast) && (iface.flags() & QNetworkInterface::IsUp))
                mdns_->joinMulticastGroup(QHostAddress(kMdnsAddr), iface);
        connect(mdns_, &QUdpSocket::readyRead, this, &CastManager::onMdnsDatagram);
    }
    sendSsdpQuery();
    sendMdnsQuery();
    // A second burst shortly after, since UDP discovery packets are lossy.
    QTimer::singleShot(1500, this, [this] { sendSsdpQuery(); sendMdnsQuery(); });
}

// ---------------------------------------------------------------- SSDP / DLNA ----
void CastManager::sendSsdpQuery()
{
    if (!ssdp_) return;
    const QByteArray msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n";
    ssdp_->writeDatagram(msg, QHostAddress(kSsdpAddr), kSsdpPort);
}

void CastManager::onSsdpDatagram()
{
    while (ssdp_ && ssdp_->hasPendingDatagrams())
    {
        const QByteArray data = ssdp_->receiveDatagram().data();
        // Pull the LOCATION header; fetch the device description to get its name + AVTransport control URL.
        const QRegularExpression re(QStringLiteral("(?im)^LOCATION:\\s*(\\S+)\\s*$"));
        const auto m = re.match(QString::fromUtf8(data));
        if (m.hasMatch()) fetchDlnaDescription(m.captured(1));
    }
}

void CastManager::fetchDlnaDescription(const QString& location)
{
    // Skip if we already have this host as a DLNA device (LOCATION repeats across bursts).
    const QString host = QUrl(location).host();
    for (const CastDevice& e : devices_)
        if (e.type == CastDevice::Dlna && e.host == host && !e.controlUrl.isEmpty()) return;

    QNetworkReply* r = nam_->get(QNetworkRequest(QUrl(location)));
    connect(r, &QNetworkReply::finished, this, [this, r, location] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;
        const QString xml = QString::fromUtf8(r->readAll());
        if (!xml.contains(QStringLiteral("MediaRenderer"))) return; // only renderers can play media

        QString name = QStringLiteral("DLNA device");
        const auto fn = QRegularExpression(QStringLiteral("(?is)<friendlyName>(.*?)</friendlyName>")).match(xml);
        if (fn.hasMatch()) name = fn.captured(1).trimmed();

        // Find the AVTransport service block and its controlURL, resolved against the description's base URL.
        QString control;
        const auto svc = QRegularExpression(QStringLiteral(
            "(?is)<service>(?:(?!</service>).)*?AVTransport:1.*?</service>")).match(xml);
        if (svc.hasMatch())
        {
            const auto cu = QRegularExpression(QStringLiteral("(?is)<controlURL>(.*?)</controlURL>")).match(svc.captured(0));
            if (cu.hasMatch())
                control = QUrl(location).resolved(QUrl(cu.captured(1).trimmed())).toString();
        }
        if (control.isEmpty()) return;

        CastDevice d;
        d.type = CastDevice::Dlna;
        d.host = QUrl(location).host();
        d.id = QStringLiteral("dlna:") + d.host;
        d.name = name;
        d.controlUrl = control;
        addOrUpdate(d);
    });
}

void CastManager::dlnaSoap(const QString& action, const QString& xmlBody)
{
    if (active_.controlUrl.isEmpty()) return;
    QNetworkRequest req{ QUrl(active_.controlUrl) };
    req.setRawHeader("SOAPACTION",
        ("\"urn:schemas-upnp-org:service:AVTransport:1#" + action + "\"").toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    const QString env = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>") + xmlBody +
        QStringLiteral("</s:Body></s:Envelope>");
    QNetworkReply* r = nam_->post(req, env.toUtf8());
    connect(r, &QNetworkReply::finished, this, [this, r, action] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError && action == QStringLiteral("SetAVTransportURI"))
            emit castError(tr("The device rejected the stream (%1).").arg(r->errorString()));
    });
}

// ---------------------------------------------------------------- mDNS / Chromecast ----
void CastManager::sendMdnsQuery()
{
    if (!mdns_) return;
    // A standard mDNS query: header + one question for PTR _googlecast._tcp.local.
    QByteArray q;
    auto put16 = [&](quint16 v) { q.append(char(v >> 8)); q.append(char(v & 0xFF)); };
    put16(0); put16(0);            // id, flags (query)
    put16(1); put16(0); put16(0); put16(0); // qd=1, an/ns/ar=0
    for (const QByteArray& label : { QByteArray("_googlecast"), QByteArray("_tcp"), QByteArray("local") })
    { q.append(char(label.size())); q.append(label); }
    q.append(char(0));             // end of name
    put16(12);                     // QTYPE = PTR
    put16(1);                      // QCLASS = IN
    mdns_->writeDatagram(q, QHostAddress(kMdnsAddr), kMdnsPort);
}

void CastManager::onMdnsDatagram()
{
    while (mdns_ && mdns_->hasPendingDatagrams())
    {
        const QByteArray pkt = mdns_->receiveDatagram().data();
        QString ip, name;
        if (parseMdns(pkt, ip, name) && !ip.isEmpty())
        {
            CastDevice d;
            d.type = CastDevice::Chromecast;
            d.host = ip;
            d.port = 8009;
            d.id = QStringLiteral("cc:") + ip;
            d.name = name.isEmpty() ? QStringLiteral("Chromecast") : name;
            addOrUpdate(d);
        }
    }
}

// Minimal mDNS response parse: walk the records, pull the first A record (IPv4) and a TXT "fn=" friendly name.
// Names use compression pointers, so we skip over names rather than decoding them fully.
bool CastManager::parseMdns(const QByteArray& pkt, QString& ipOut, QString& nameOut)
{
    const int n = pkt.size();
    if (n < 12) return false;
    auto u16 = [&](int p) { return int((quint8(pkt[p]) << 8) | quint8(pkt[p + 1])); };
    const int qd = u16(4), an = u16(6), ns = u16(8), ar = u16(10);
    int p = 12;
    auto skipName = [&](int pos) -> int {
        while (pos < n)
        {
            const quint8 len = quint8(pkt[pos]);
            if (len == 0) return pos + 1;
            if ((len & 0xC0) == 0xC0) return pos + 2; // compression pointer terminates the name
            pos += 1 + len;
        }
        return pos;
    };
    for (int i = 0; i < qd && p < n; ++i) { p = skipName(p); p += 4; } // question: name + qtype + qclass
    const int total = an + ns + ar;
    for (int i = 0; i < total && p < n; ++i)
    {
        p = skipName(p);
        if (p + 10 > n) break;
        const int type = u16(p);
        const int rdlen = u16(p + 8);
        p += 10;
        if (p + rdlen > n) break;
        if (type == 1 && rdlen == 4 && ipOut.isEmpty()) // A record
            ipOut = QStringLiteral("%1.%2.%3.%4").arg(quint8(pkt[p])).arg(quint8(pkt[p + 1]))
                        .arg(quint8(pkt[p + 2])).arg(quint8(pkt[p + 3]));
        else if (type == 16 && nameOut.isEmpty()) // TXT record: length-prefixed strings
        {
            int q = p;
            while (q < p + rdlen)
            {
                const int slen = quint8(pkt[q++]);
                if (q + slen > p + rdlen) break;
                const QString kv = QString::fromUtf8(pkt.mid(q, slen));
                if (kv.startsWith(QStringLiteral("fn="))) { nameOut = kv.mid(3); break; }
                q += slen;
            }
        }
        p += rdlen;
    }
    return !ipOut.isEmpty();
}

// ---------------------------------------------------------------- Chromecast CASTV2 session ----
void CastManager::ccSend(const QString& destId, const QString& ns, const QString& payloadJson)
{
    if (!cc_) return;
    const QByteArray msg = encodeCast(QStringLiteral("sender-0"), destId, ns, payloadJson);
    QByteArray frame;
    const quint32 len = quint32(msg.size());
    frame.append(char(len >> 24)); frame.append(char(len >> 16)); frame.append(char(len >> 8)); frame.append(char(len));
    frame += msg;
    cc_->write(frame);
}

void CastManager::ccConnectAndLoad(const CastDevice& d, const QString& url, const QString& title, const QString& mime)
{
    ccTeardown();
    ccPendingUrl_ = url; ccPendingTitle_ = title; ccPendingMime_ = mime;
    ccTransportId_.clear(); ccSessionId_.clear(); ccBuf_.clear(); ccReqId_ = 1;

    cc_ = new QSslSocket(this);
    cc_->setPeerVerifyMode(QSslSocket::VerifyNone); // Chromecasts present a self-signed cert
    connect(cc_, &QSslSocket::encrypted, this, [this] {
        ccSend(QStringLiteral("receiver-0"), QString::fromLatin1(NS_CONNECTION), QStringLiteral("{\"type\":\"CONNECT\"}"));
        ccSend(QStringLiteral("receiver-0"), QString::fromLatin1(NS_RECEIVER),
               QStringLiteral("{\"type\":\"LAUNCH\",\"appId\":\"%1\",\"requestId\":%2}")
                   .arg(QString::fromLatin1(DEFAULT_APP)).arg(ccReqId_++));
    });
    connect(cc_, &QSslSocket::readyRead, this, &CastManager::ccOnReadyRead);
    connect(cc_, &QSslSocket::sslErrors, cc_, [this](const QList<QSslError>&) { if (cc_) cc_->ignoreSslErrors(); });
    connect(cc_, &QAbstractSocket::errorOccurred, this, [this] {
        emit castError(tr("Couldn't reach the Chromecast."));
        casting_ = false; emit castStopped();
    });
    if (!ccHeartbeat_)
    {
        ccHeartbeat_ = new QTimer(this);
        ccHeartbeat_->setInterval(4500);
        connect(ccHeartbeat_, &QTimer::timeout, this, [this] {
            ccSend(QStringLiteral("receiver-0"), QString::fromLatin1(NS_HEARTBEAT), QStringLiteral("{\"type\":\"PING\"}")); });
    }
    ccHeartbeat_->start();
    cc_->connectToHostEncrypted(d.host, d.port ? d.port : 8009);
}

void CastManager::ccOnReadyRead()
{
    ccBuf_ += cc_->readAll();
    // Frames are a 4-byte big-endian length followed by the protobuf CastMessage.
    while (ccBuf_.size() >= 4)
    {
        const quint32 len = (quint8(ccBuf_[0]) << 24) | (quint8(ccBuf_[1]) << 16)
                          | (quint8(ccBuf_[2]) << 8) | quint8(ccBuf_[3]);
        if (quint32(ccBuf_.size()) < 4 + len) break;
        const QByteArray msg = ccBuf_.mid(4, int(len));
        ccBuf_.remove(0, int(4 + len));

        QString ns, payload;
        decodeCast(msg, ns, payload);
        const QJsonObject o = QJsonDocument::fromJson(payload.toUtf8()).object();
        const QString type = o.value(QStringLiteral("type")).toString();

        if (ns == QString::fromLatin1(NS_HEARTBEAT) && type == QStringLiteral("PING"))
            ccSend(QStringLiteral("receiver-0"), QString::fromLatin1(NS_HEARTBEAT), QStringLiteral("{\"type\":\"PONG\"}"));
        else if (ns == QString::fromLatin1(NS_RECEIVER) && type == QStringLiteral("RECEIVER_STATUS"))
        {
            // Once the media receiver is running, grab its transportId and LOAD the stream into it.
            if (ccTransportId_.isEmpty())
            {
                const QJsonArray apps = o.value(QStringLiteral("status")).toObject()
                                            .value(QStringLiteral("applications")).toArray();
                for (const QJsonValue& v : apps)
                {
                    const QJsonObject a = v.toObject();
                    if (a.value(QStringLiteral("appId")).toString() == QString::fromLatin1(DEFAULT_APP))
                    {
                        ccTransportId_ = a.value(QStringLiteral("transportId")).toString();
                        ccSessionId_ = a.value(QStringLiteral("sessionId")).toString();
                    }
                }
                if (!ccTransportId_.isEmpty())
                {
                    ccSend(ccTransportId_, QString::fromLatin1(NS_CONNECTION), QStringLiteral("{\"type\":\"CONNECT\"}"));
                    QJsonObject media{ { QStringLiteral("contentId"), ccPendingUrl_ },
                                       { QStringLiteral("streamType"), QStringLiteral("BUFFERED") },
                                       { QStringLiteral("contentType"), ccPendingMime_.isEmpty() ? QStringLiteral("video/mp4") : ccPendingMime_ } };
                    if (!ccPendingTitle_.isEmpty())
                        media.insert(QStringLiteral("metadata"), QJsonObject{
                            { QStringLiteral("type"), 0 }, { QStringLiteral("metadataType"), 0 },
                            { QStringLiteral("title"), ccPendingTitle_ } });
                    const QJsonObject load{ { QStringLiteral("type"), QStringLiteral("LOAD") },
                                            { QStringLiteral("requestId"), ccReqId_++ },
                                            { QStringLiteral("media"), media },
                                            { QStringLiteral("autoplay"), true },
                                            { QStringLiteral("currentTime"), 0 } };
                    ccSend(ccTransportId_, QString::fromLatin1(NS_MEDIA),
                           QString::fromUtf8(QJsonDocument(load).toJson(QJsonDocument::Compact)));
                    casting_ = true;
                    emit castStarted(castingName_);
                }
            }
        }
    }
}

void CastManager::ccTeardown()
{
    if (ccHeartbeat_) ccHeartbeat_->stop();
    if (cc_)
    {
        if (cc_->state() == QAbstractSocket::ConnectedState)
            ccSend(QStringLiteral("receiver-0"), QString::fromLatin1(NS_CONNECTION), QStringLiteral("{\"type\":\"CLOSE\"}"));
        cc_->disconnect(this);
        cc_->abort();
        cc_->deleteLater();
        cc_ = nullptr;
    }
    ccBuf_.clear();
}

// ---------------------------------------------------------------- dispatch ----
void CastManager::cast(const CastDevice& device, const QString& url, const QString& title, const QString& mime)
{
    active_ = device;
    castingName_ = device.name;
    if (device.type == CastDevice::Chromecast)
    {
        ccConnectAndLoad(device, url, title, mime);
        return;
    }
    // DLNA: hand the renderer the URL (with minimal DIDL metadata), then start playback.
    const QString didl = xmlEscape(QStringLiteral(
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\"><dc:title>%1</dc:title>"
        "<upnp:class>object.item.videoItem</upnp:class>"
        "<res protocolInfo=\"http-get:*:%2:*\">%3</res></item></DIDL-Lite>")
        .arg(xmlEscape(title.isEmpty() ? QStringLiteral("Stream") : title),
             mime.isEmpty() ? QStringLiteral("video/mp4") : mime, xmlEscape(url)));
    dlnaSoap(QStringLiteral("SetAVTransportURI"), QStringLiteral(
        "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID><CurrentURI>%1</CurrentURI>"
        "<CurrentURIMetaData>%2</CurrentURIMetaData></u:SetAVTransportURI>")
        .arg(xmlEscape(url), didl));
    dlnaSoap(QStringLiteral("Play"), QStringLiteral(
        "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play>"));
    casting_ = true;
    emit castStarted(device.name);
}

void CastManager::stopCasting()
{
    if (active_.type == CastDevice::Chromecast) ccTeardown();
    else if (!active_.controlUrl.isEmpty())
        dlnaSoap(QStringLiteral("Stop"), QStringLiteral(
            "<u:Stop xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<InstanceID>0</InstanceID></u:Stop>"));
    casting_ = false;
    castingName_.clear();
    emit castStopped();
}
