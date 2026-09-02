#include "PlayOnHost.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QSettings>
#include "AppBrand.h"
#include "AppPaths.h"
#include "Settings.h"

namespace
{
    QSettings& store()
    {
        static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                           QSettings::IniFormat);
        return s;
    }

    // The ISSUED side. One key per token rather than a list, so revoking one peer is a remove() and a
    // half-written list can never lose the others.
    constexpr const char* kIssuedGroup = "playon/issued";
    constexpr const char* kPeersGroup  = "playon/peers";

    QByteArray freshEntropy()
    {
        // 32 bytes from the system generator, which is what QRandomGenerator::system() is for. It is hashed
        // by PlayOn::mintToken before it becomes a credential, so the stored token is never these bytes.
        QByteArray b(32, '\0');
        QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(b.data()),
                                             reinterpret_cast<quint32*>(b.data() + b.size()));
        return b;
    }
}

PlayOnHost::PlayOnHost(QObject* parent) : QObject(parent) {}

PlayOn::Advert PlayOnHost::advertFor(quint16 remotePort)
{
    PlayOn::Advert a;
    a.instanceId = Settings::deviceId();
    a.name       = Settings::deviceName();
    a.version    = QCoreApplication::applicationVersion();
    a.port       = remotePort;
    return a;
}

// ---------------------------------------------------------------- this device as a TARGET ----------------
QString PlayOnHost::beginPairing()
{
    const QString code = pairing_.begin(QRandomGenerator::system()->generate());
    emit pairingChanged();
    return code;
}

void PlayOnHost::cancelPairing()
{
    pairing_.cancel();
    emit pairingChanged();
}

QString PlayOnHost::redeemPairing(const QString& enteredCode)
{
    const QString token = pairing_.redeem(enteredCode, freshEntropy());
    emit pairingChanged();
    if (token.isEmpty()) return QString();
    // Persist as ISSUED. Keyed by the token's own first 16 characters rather than by the peer, because at
    // this moment we do not know WHICH peer answered — only that whoever it was could see this screen. The
    // token is the identity; that is the whole of what it proves.
    store().setValue(QLatin1String(kIssuedGroup) + QStringLiteral("/") + token.left(16), token);
    store().sync();
    return token;
}

QSet<QString> PlayOnHost::issuedTokens() const
{
    QSet<QString> out;
    store().beginGroup(QLatin1String(kIssuedGroup));
    for (const QString& k : store().allKeys())
    {
        const QString v = store().value(k).toString();
        if (!v.isEmpty()) out.insert(v);
    }
    store().endGroup();
    return out;
}

// ---------------------------------------------------------------- this device as a SOURCE ----------------
QString PlayOnHost::tokenFor(const QString& peerInstanceId) const
{
    if (peerInstanceId.isEmpty()) return QString();
    return store().value(PlayOn::tokenKey(peerInstanceId)).toString();
}

void PlayOnHost::storeToken(const QString& peerInstanceId, const QString& token)
{
    if (peerInstanceId.isEmpty() || token.isEmpty()) return;
    store().setValue(PlayOn::tokenKey(peerInstanceId), token);
    store().sync();
    emit pairedPeersChanged();
}

void PlayOnHost::forgetPeer(const QString& peerInstanceId)
{
    if (peerInstanceId.isEmpty()) return;
    store().remove(PlayOn::tokenKey(peerInstanceId));
    store().sync();
    emit pairedPeersChanged();
}

QSet<QString> PlayOnHost::pairedPeerIds() const
{
    QSet<QString> out;
    store().beginGroup(QLatin1String(kPeersGroup));
    for (const QString& k : store().allKeys())
    {
        // Keys arrive as "<peerId>/token"; the peer id is everything before the last separator.
        const int slash = k.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0) continue;
        if (store().value(k).toString().isEmpty()) continue;
        out.insert(k.left(slash));
    }
    store().endGroup();
    return out;
}
