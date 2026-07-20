// The sandboxed host API handed to each addon's script. Every capability is permission-gated and
// addon-scoped: an addon can only touch its own storage and only reach the network if it declared
// "network" in its manifest. Exposed to JS as globals: log, httpGet, getStorage, setStorage.
#pragma once
#include "AddonModels.h"
#include <QString>
#include <QSet>
#include <QHash>

class AddonContext
{
public:
    AddonContext(const AddonManifest& manifest, const QString& storageDir);

    void log(const QString& message) const;
    QString httpGet(const QString& url) const;        // requires "network"; "" on denial/error
    // Flexible request: optionsJson = {"method","url","headers":{..},"body"}. Needed for POST APIs and
    // custom auth headers (IGDB/Twitch, SteamGridDB, ...). Requires "network"; "" on denial/error.
    QString httpRequest(const QString& optionsJson) const;
    QString getStorage(const QString& key) const;     // addon-writable scratch storage
    void setStorage(const QString& key, const QString& value) const;
    QString getConfig(const QString& key) const;      // user-set credential/option (or manifest default)
    // Embedded provider dev credential, de-obfuscated on demand from the build-time BuiltinSecrets.h.
    // Returns "" when nothing was embedded (secrets file absent at build) or the key is unknown. The
    // obfuscation is best-effort only (NOT cryptography) — see AddonContext.cpp / native/secrets.
    QString builtinCredential(const QString& key) const;

    const QString& id() const { return id_; }

    // Config is shared with the settings UI; one key scheme so both read/write the same place.
    static QString readConfig(const QString& addonId, const QString& key, const QString& defaultValue = {});
    static void writeConfig(const QString& addonId, const QString& key, const QString& value);

private:
    static QString sanitize(const QString& key);

    QString id_;
    QSet<QString> permissions_;
    QString storageDir_;
    QHash<QString, QString> configDefaults_; // key -> manifest default
};
