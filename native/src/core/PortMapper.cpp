#include "PortMapper.h"

#include <QUdpSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QUrl>
#include <QRegularExpression>
#include <QTimer>

PortMapper::PortMapper(QObject* parent) : QObject(parent) {}
PortMapper::~PortMapper() { }

// The LAN address the router should forward to (first non-loopback IPv4).
static QString localIPv4()
{
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            return a.toString();
    return QString();
}

void PortMapper::map(quint16 internalPort, quint16 externalPort)
{
    internal_ = internalPort;
    external_ = externalPort;
    done_ = false;
    localIp_ = localIPv4();
    if (localIp_.isEmpty()) { emit failed(tr("No local network address.")); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);

    ssdp_ = new QUdpSocket(this);
    ssdp_->bind(QHostAddress(QHostAddress::AnyIPv4), 0, QUdpSocket::ShareAddress);
    connect(ssdp_, &QUdpSocket::readyRead, this, &PortMapper::onDatagram);

    // Ask any Internet Gateway Device to identify itself. Send twice (UDP is lossy).
    const QByteArray msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    ssdp_->writeDatagram(msearch, QHostAddress(QStringLiteral("239.255.255.250")), 1900);
    QTimer::singleShot(400, this, [this, msearch] {
        if (!done_ && ssdp_) ssdp_->writeDatagram(msearch, QHostAddress(QStringLiteral("239.255.255.250")), 1900); });

    // Give up if no gateway answers.
    QTimer::singleShot(4000, this, [this] {
        if (!done_ && controlUrl_.isEmpty()) { done_ = true; emit failed(tr("No UPnP router found (or UPnP is off).")); } });
}

void PortMapper::onDatagram()
{
    while (ssdp_ && ssdp_->hasPendingDatagrams())
    {
        QByteArray buf(int(ssdp_->pendingDatagramSize()), 0);
        ssdp_->readDatagram(buf.data(), buf.size());
        const QString resp = QString::fromLatin1(buf);
        if (!resp.contains(QStringLiteral("InternetGatewayDevice"), Qt::CaseInsensitive)
            && !resp.contains(QStringLiteral("WANConnectionDevice"), Qt::CaseInsensitive)
            && !resp.contains(QStringLiteral("WAN"), Qt::CaseInsensitive)) continue;
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("(?im)^LOCATION:\\s*(\\S+)")).match(resp);
        if (m.hasMatch() && controlUrl_.isEmpty() && !done_) { fetchDescription(m.captured(1).trimmed()); return; }
    }
}

void PortMapper::fetchDescription(const QString& location)
{
    QNetworkReply* r = nam_->get(QNetworkRequest(QUrl(location)));
    connect(r, &QNetworkReply::finished, this, [this, r, location] {
        r->deleteLater();
        if (r->error() == QNetworkReply::NoError) findControlUrl(location, r->readAll());
    });
}

void PortMapper::findControlUrl(const QString& descUrl, const QByteArray& xml)
{
    if (!controlUrl_.isEmpty() || done_) return;
    const QString s = QString::fromUtf8(xml);
    // Prefer WANIPConnection, else WANPPPConnection. Grab the service block and its controlURL.
    for (const QString& svc : { QStringLiteral("WANIPConnection"), QStringLiteral("WANPPPConnection") })
    {
        const QRegularExpressionMatch block = QRegularExpression(
            QStringLiteral("(?is)<service>(?:(?!</service>).)*?%1:1.*?</service>").arg(svc)).match(s);
        if (!block.hasMatch()) continue;
        const QRegularExpressionMatch ctrl = QRegularExpression(QStringLiteral("(?is)<controlURL>\\s*(.*?)\\s*</controlURL>"))
                                                 .match(block.captured(0));
        if (!ctrl.hasMatch()) continue;
        serviceType_ = QStringLiteral("urn:schemas-upnp-org:service:%1:1").arg(svc);
        controlUrl_ = QUrl(descUrl).resolved(QUrl(ctrl.captured(1).trimmed())).toString();
        addMapping();
        return;
    }
    // This description had no WAN service (a sub-device desc); keep listening for the right one.
}

void PortMapper::soap(const QString& action, const QString& bodyArgs,
                      std::function<void(bool, const QByteArray&)> cb)
{
    QNetworkRequest req{ QUrl(controlUrl_) };
    req.setRawHeader("SOAPAction", ("\"" + serviceType_ + "#" + action + "\"").toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    const QByteArray env =
        ("<?xml version=\"1.0\"?>"
         "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
         "<u:" + action + " xmlns:u=\"" + serviceType_ + "\">" + bodyArgs +
         "</u:" + action + "></s:Body></s:Envelope>").toUtf8();
    QNetworkReply* r = nam_->post(req, env);
    connect(r, &QNetworkReply::finished, this, [r, cb] {
        r->deleteLater();
        cb(r->error() == QNetworkReply::NoError, r->readAll());
    });
}

void PortMapper::addMapping()
{
    const QString args =
        QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
                       "<NewProtocol>TCP</NewProtocol><NewInternalPort>%2</NewInternalPort>"
                       "<NewInternalClient>%3</NewInternalClient><NewEnabled>1</NewEnabled>"
                       "<NewPortMappingDescription>MyMediaVault netplay</NewPortMappingDescription>"
                       "<NewLeaseDuration>3600</NewLeaseDuration>")
            .arg(external_).arg(internal_).arg(localIp_);
    soap(QStringLiteral("AddPortMapping"), args, [this](bool ok, const QByteArray& resp) {
        if (done_) return;
        if (!ok) { done_ = true; emit failed(tr("The router refused the port mapping (%1).")
                       .arg(QString::fromUtf8(resp).left(120))); return; }
        mapped_ = true;
        getExternalIp();
    });
}

void PortMapper::getExternalIp()
{
    soap(QStringLiteral("GetExternalIPAddress"), QString(), [this](bool ok, const QByteArray& resp) {
        if (done_) return;
        done_ = true;
        const QRegularExpressionMatch m = QRegularExpression(
            QStringLiteral("(?is)<NewExternalIPAddress>\\s*(.*?)\\s*</NewExternalIPAddress>")).match(QString::fromUtf8(resp));
        const QString ip = (ok && m.hasMatch()) ? m.captured(1).trimmed() : QString();
        if (ip.isEmpty() || ip == QLatin1String("0.0.0.0"))
            emit failed(tr("Couldn't read the router's public IP."));
        else
            emit mapped(ip, external_);
    });
}

void PortMapper::unmap()
{
    if (!mapped_ || controlUrl_.isEmpty()) return;
    mapped_ = false;
    const QString args = QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
                                        "<NewProtocol>TCP</NewProtocol>").arg(external_);
    soap(QStringLiteral("DeletePortMapping"), args, [](bool, const QByteArray&) {});
}
