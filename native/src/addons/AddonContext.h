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
    // Returns "" when nothing was embedded (secrets file absent at build) or the (addon,key) pair is not
    // allow-listed. The obfuscation is best-effort only (NOT cryptography) — see AddonContext.cpp / native/secrets.
    QString builtinCredential(const QString& key) const;

    // The resolved credential a provider should USE for `key`: the user-set value when present, else the
    // app's embedded builtin, else "" (dormant). Exposed to JS as providerCredential(key). The selection
    // rule itself is the pure, testable selectCredential() below.
    QString resolvedCredential(const QString& key) const;

    // Pure credential-fallback selection (no I/O): user value wins when non-empty; else the builtin when
    // non-empty; else "". This is the single source of truth resolvedCredential() applies at runtime, so a
    // provider never issues a request with an empty key. Static + pure precisely so it can be probed in
    // isolation with independent fixtures.
    static QString selectCredential(const QString& userValue, const QString& builtinValue);

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
