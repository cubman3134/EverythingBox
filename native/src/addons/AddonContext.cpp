#include "AddonContext.h"
#include "../core/AppPaths.h"
#include "BuiltinSecrets.h" // generated into the build tree by cmake/GenerateSecrets.cmake

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
#include <QThread>
#include <QSettings>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

static QSettings& configStore()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

AddonContext::AddonContext(const AddonManifest& manifest, const QString& storageDir)
    : id_(manifest.id.isEmpty() ? QStringLiteral("unknown") : manifest.id),
      storageDir_(storageDir)
{
    for (const QString& p : manifest.permissions) permissions_.insert(p);
    for (const AddonSetting& s : manifest.settings) configDefaults_.insert(s.key, s.defaultValue);
    QDir().mkpath(storageDir_);
}

QString AddonContext::readConfig(const QString& addonId, const QString& key, const QString& defaultValue)
{
    return configStore().value(QStringLiteral("addoncfg/%1/%2").arg(addonId, key), defaultValue).toString();
}

void AddonContext::writeConfig(const QString& addonId, const QString& key, const QString& value)
{
    configStore().setValue(QStringLiteral("addoncfg/%1/%2").arg(addonId, key), value);
    configStore().sync();
}

QString AddonContext::getConfig(const QString& key) const
{
    return readConfig(id_, key, configDefaults_.value(key));
}

QString AddonContext::builtinCredential(const QString& key) const
{
    // Best-effort obfuscation, NOT cryptography: join the two split obfuscated arrays, reverse the
    // rolling XOR (this MUST mirror native/cmake/GenerateSecrets.cmake byte-for-byte), then slice the
    // recovered plaintext blob into devid|devpassword by their stored lengths. The XOR only keeps the
    // creds out of a `strings` scan — anyone with the binary can recover them.
    using namespace mmv_secrets;
    const int total = kScreenScraperALen + kScreenScraperBLen;
    if (total <= 0) return QString(); // secrets file was absent at build → nothing embedded

    static const unsigned char KEY[] = { 90, 195, 23, 158, 66, 189, 47, 113 };
    const int keyLen = static_cast<int>(sizeof(KEY));

    QByteArray blob;
    blob.reserve(total);
    for (int i = 0; i < total; ++i)
    {
        const unsigned char ob = (i < kScreenScraperALen)
            ? kScreenScraperA[i]
            : kScreenScraperB[i - kScreenScraperALen];
        const unsigned char pb = static_cast<unsigned char>((ob ^ KEY[i % keyLen]) ^ (i & 0xFF));
        blob.append(static_cast<char>(pb));
    }

    const int devidLen = kScreenScraperDevidLen;
    const int devpwLen = kScreenScraperDevpasswordLen;
    if (key == QStringLiteral("devid") && devidLen > 0 && devidLen <= blob.size())
        return QString::fromUtf8(blob.constData(), devidLen);
    if (key == QStringLiteral("devpassword") && devpwLen > 0 && devidLen + devpwLen <= blob.size())
        return QString::fromUtf8(blob.constData() + devidLen, devpwLen);
    return QString();
}

void AddonContext::log(const QString& message) const
{
    qInfo().noquote() << QStringLiteral("[addon:%1] %2").arg(id_, message);
}

QString AddonContext::httpGet(const QString& url) const
{
    const QByteArray opts = QJsonDocument(QJsonObject{ { QStringLiteral("url"), url } }).toJson(QJsonDocument::Compact);
    return httpRequest(QString::fromUtf8(opts));
}

QString AddonContext::httpRequest(const QString& optionsJson) const
{
    if (!permissions_.contains(QStringLiteral("network")))
    {
        qWarning().noquote() << QStringLiteral("[addon:%1] denied http (missing \"network\" permission)").arg(id_);
        return QString();
    }

    const QJsonObject o = QJsonDocument::fromJson(optionsJson.toUtf8()).object();
    const QString method = o.value(QStringLiteral("method")).toString(QStringLiteral("GET")).toUpper();
    const QString url = o.value(QStringLiteral("url")).toString();
    const QByteArray body = o.value(QStringLiteral("body")).toString().toUtf8();

    const QUrl u(url);
    if (!u.isValid() || (u.scheme() != QStringLiteral("http") && u.scheme() != QStringLiteral("https")))
        return QString();

    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVaultAddon"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    const QJsonObject headers = o.value(QStringLiteral("headers")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toVariant().toString().toUtf8());

    // Retry transient failures (timeouts, network errors, HTTP 5xx like Google's flaky "backendFailed").
    // We run off the GUI thread, so a short backoff between attempts is fine. 4xx are NOT retried - they
    // are real client errors (bad key, disabled API) whose body we return for the addon to surface.
    const int kMaxAttempts = 3;
    QString out, lastError;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        QNetworkAccessManager nam;
        QNetworkReply* reply = nullptr;
        if (method == QStringLiteral("GET"))       reply = nam.get(req);
        else if (method == QStringLiteral("POST")) reply = nam.post(req, body);
        else                                       reply = nam.sendCustomRequest(req, method.toUtf8(), body);

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start(20000);
        loop.exec();

        const bool finished = reply->isFinished();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (finished) out = QString::fromUtf8(reply->readAll());

        const bool transient = !finished || status == 0 || status >= 500;
        if (!transient) { reply->abort(); reply->deleteLater(); return out; } // success or 4xx - done

        // Transient failure: remember it but stay quiet - a later attempt usually recovers. We only log
        // once, below, if every attempt is exhausted (so a recovered retry doesn't look like an error).
        lastError = finished ? reply->errorString() : QStringLiteral("timed out");
        reply->abort();
        reply->deleteLater();
        if (attempt + 1 < kMaxAttempts) QThread::msleep(400 * (attempt + 1)); // 0.4s, then 0.8s backoff
    }

    // All attempts failed transiently. Stream the URL/error (URLs hold percent-encoded bytes like "%3A"
    // that a chained .arg() would mistake for a "%3" placeholder and corrupt).
    qWarning().noquote() << QStringLiteral("[addon:%1]").arg(id_) << method << url
                         << QStringLiteral("failed after %1 attempts:").arg(kMaxAttempts) << lastError;
    return out; // hand back the last body (may carry the server's 5xx error JSON)
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
