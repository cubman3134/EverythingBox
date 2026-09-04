// The device-local state "Play on device" (issue #143) keeps between launches: the pairing tokens, in both
// directions, and the one-offer-at-a-time pairing state machine that mints them.
//
// TWO SETS OF TOKENS, AND THEY ARE NOT THE SAME THING. Confusing them is the security bug this file exists
// to make impossible to write:
//
//   * ISSUED — credentials THIS device minted for peers that stood in front of its screen and read a code.
//     Presenting one authorises /open HERE. They are what RemoteServer checks.
//   * HELD — credentials another device minted for THIS one. They authorise /open THERE. They are what
//     PlayOnClient presents.
//
// Both live under "playon/", which CloudSync's device-local carve-out and SettingsTxn's out-of-scope list
// both exclude: a token is meaningful only between the two machines that performed the pairing, and a synced
// one would hand every install on the account the right to start playback on a device it never paired with.
//
// Nothing here logs a token, returns one in an error string, or writes one anywhere but its ini key.
#pragma once
#include <QObject>
#include <QSet>
#include <QString>
#include "PlayOnDevice.h"

class PlayOnHost : public QObject
{
    Q_OBJECT
public:
    explicit PlayOnHost(QObject* parent = nullptr);

    // ---- this device as a TARGET ----
    // Arm one pairing offer and return the six-digit code to put on screen. Replaces any offer already
    // pending: the code on screen is always the only one that works.
    QString beginPairing();
    void    cancelPairing();
    bool    pairingPending() const { return pairing_.pending(); }
    QString pairingCode() const { return pairing_.code(); }
    // Redeem a code for a token, mint it, persist it as ISSUED, and return it. Empty string on refusal.
    QString redeemPairing(const QString& enteredCode);
    QSet<QString> issuedTokens() const;

    // ---- this device as a SOURCE ----
    QString tokenFor(const QString& peerInstanceId) const;      // "" when not paired with that peer
    void    storeToken(const QString& peerInstanceId, const QString& token);
    void    forgetPeer(const QString& peerInstanceId);
    QSet<QString> pairedPeerIds() const;

    // This install's advertised identity. Empty port means "not advertising" — the caller supplies the #76
    // port only while that server is actually listening.
    static PlayOn::Advert advertFor(quint16 remotePort);

signals:
    void pairingChanged();      // an offer was armed, consumed or burned
    void pairedPeersChanged();

private:
    PlayOn::Pairing pairing_;
};
