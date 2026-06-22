#include "AddonContext.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

AddonContext::AddonContext(const AddonManifest& manifest, const QString& storageDir)
    : id_(manifest.id.isEmpty() ? QStringLiteral("unknown") : manifest.id),
      storageDir_(storageDir)
{
    for (const QString& p : manifest.permissions) permissions_.insert(p);
    QDir().mkpath(storageDir_);
}

void AddonContext::log(const QString& message) const
{
    qInfo().noquote() << QStringLiteral("[addon:%1] %2").arg(id_, message);
}

QString AddonContext::httpGet(const QString& url) const
{
    if (!permissions_.contains(QStringLiteral("network")))
    {
        qWarning().noquote() << QStringLiteral("[addon:%1] denied httpGet (missing \"network\" permission)").arg(id_);
        return QString();
    }
    const QUrl u(url);
    if (!u.isValid() || (u.scheme() != QStringLiteral("http") && u.scheme() != QStringLiteral("https")))
        return QString();

    // Synchronous (the JS engine is synchronous). A local event loop pumps the request with a timeout.
    QNetworkAccessManager nam;
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GoliathAddon"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam.get(req);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(15000);
    loop.exec();

    QString body;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError)
        body = QString::fromUtf8(reply->readAll());
    else
        qWarning().noquote() << QStringLiteral("[addon:%1] httpGet failed: %2").arg(id_, reply->errorString());
    reply->abort();
    reply->deleteLater();
    return body;
}

QString AddonContext::getStorage(const QString& key) const
{
    QFile f(storageDir_ + QStringLiteral("/") + sanitize(key) + QStringLiteral(".txt"));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

void AddonContext::setStorage(const QString& key, const QString& value) const
{
    QFile f(storageDir_ + QStringLiteral("/") + sanitize(key) + QStringLiteral(".txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(value.toUtf8());
}

QString AddonContext::sanitize(const QString& key)
{
    QString s = key.isEmpty() ? QStringLiteral("key") : key;
    s.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_-]")), QStringLiteral("_"));
    return s;
}
