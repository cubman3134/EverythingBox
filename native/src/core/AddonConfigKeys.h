// WHERE AN ADD-ON'S SETTINGS ARE STORED (issue #77, increment 3). Header-only and QtCore-only, so CloudSync's
// carve-out and AddonContext's reader/writer share one spelling of each key without a new link dependency.
//
// An add-on declares its settings in its manifest (AddonSetting: key, label, type). Every value used to live at
//     addoncfg/<addon id>/<key>
// which rides the heavy settings bundle to every device on the account: set an API key once, have it
// everywhere. That stays true for every field EXCEPT one the add-on declares `type: password`. A password is a
// credential for somebody's account, and the synced bundle is a zip on a third party's disk — so a password
// field lives at
//     addonsecret/<addon id>/<key>
// instead, which CloudSync::isDeviceLocalKey carves out in both directions: it is never exported, never
// imported, and never part of the sync fingerprint. Each device holds its own.
//
// MIGRATION. A value an earlier build stored under addoncfg/ is MOVED once, per field: copied to the
// device-local key (unless this device already holds one there — a local credential is never overwritten by a
// synced one), removed from the synced key, and stamped at
//     addonsecretmig/<addon id>/<key>          (epoch seconds of the move)
// After the stamp, a synced copy that re-arrives — an older peer still sends it in its bundle — is removed from
// the synced key and never read. The stamp is device-local too: it is a fact about this device's migration.
#pragma once
#include <QString>

namespace AddonConfigKeys
{
    inline QString syncedKey(const QString& addonId, const QString& key)
    { return QStringLiteral("addoncfg/%1/%2").arg(addonId, key); }

    inline QString secretKey(const QString& addonId, const QString& key)
    { return QStringLiteral("addonsecret/%1/%2").arg(addonId, key); }

    inline QString migratedStampKey(const QString& addonId, const QString& key)
    { return QStringLiteral("addonsecretmig/%1/%2").arg(addonId, key); }

    // The key a setting of this TYPE is stored under. "password" is the manifest's own word for it (AddonSetting
    // documents "text" | "password" | "checkbox" | "number").
    inline bool isSecretType(const QString& type) { return type == QLatin1String("password"); }
    inline QString keyFor(const QString& addonId, const QString& key, const QString& type)
    { return isSecretType(type) ? secretKey(addonId, key) : syncedKey(addonId, key); }

    // Both device-local families, for CloudSync::isDeviceLocalKey.
    inline bool isDeviceLocal(const QString& iniKey)
    {
        return iniKey.startsWith(QLatin1String("addonsecret/")) || iniKey.startsWith(QLatin1String("addonsecretmig/"));
    }
}
