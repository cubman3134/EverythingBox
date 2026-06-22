// The sandboxed host API handed to each addon's script. Every capability is permission-gated and
// addon-scoped: an addon can only touch its own storage and only reach the network if it declared
// "network" in its manifest. Exposed to JS as globals: log, httpGet, getStorage, setStorage.
#pragma once
#include "AddonModels.h"
#include <QString>
#include <QSet>

class AddonContext
{
public:
    AddonContext(const AddonManifest& manifest, const QString& storageDir);

    void log(const QString& message) const;
    QString httpGet(const QString& url) const;        // requires "network"; "" on denial/error
    QString getStorage(const QString& key) const;
    void setStorage(const QString& key, const QString& value) const;

    const QString& id() const { return id_; }

private:
    static QString sanitize(const QString& key);

    QString id_;
    QSet<QString> permissions_;
    QString storageDir_;
};
