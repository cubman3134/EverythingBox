// UPnP-IGD automatic port forwarding, so an online-netplay host behind a home router can accept a DIRECT
// connection without the user opening ports by hand. Discovers the router (SSDP), finds its WAN connection
// service, asks it to forward externalPort -> this machine:internalPort (AddPortMapping), and reads the public
// IP (GetExternalIPAddress). Best-effort: if the router has no UPnP / it's disabled, map() emits failed() and
// netplay falls back to the relay. Mirrors CastManager's SSDP + SOAP.
#pragma once
#include <QObject>
#include <QString>

class QUdpSocket;
class QNetworkAccessManager;

class PortMapper : public QObject
{
    Q_OBJECT
public:
    explicit PortMapper(QObject* parent = nullptr);
    ~PortMapper() override;

    void map(quint16 internalPort, quint16 externalPort); // forward externalPort -> localIP:internalPort, get public IP
    void unmap();                                          // best-effort remove the mapping we added

signals:
    void mapped(const QString& publicIp, quint16 externalPort);
    void failed(const QString& reason);

private:
    void onDatagram();
    void fetchDescription(const QString& location);
    void findControlUrl(const QString& descUrl, const QByteArray& xml);
    void soap(const QString& action, const QString& bodyArgs,
              std::function<void(bool ok, const QByteArray& resp)> cb);
    void addMapping();
    void getExternalIp();

    QUdpSocket* ssdp_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
    QString controlUrl_, serviceType_, localIp_;
    quint16 internal_ = 0, external_ = 0;
    bool done_ = false, mapped_ = false;
};
