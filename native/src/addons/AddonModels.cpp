#include "AddonModels.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

AddonManifest AddonManifest::fromJson(const QByteArray& json, bool* ok)
{
    AddonManifest m;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) { if (ok) *ok = false; return m; }
    const QJsonObject o = doc.object();

    m.id          = o.value(QStringLiteral("id")).toString();
    m.name        = o.value(QStringLiteral("name")).toString();
    m.version     = o.value(QStringLiteral("version")).toString();
    m.type        = o.value(QStringLiteral("type")).toString();
    m.entry       = o.value(QStringLiteral("entry")).toString();
    m.author      = o.value(QStringLiteral("author")).toString();
    m.description = o.value(QStringLiteral("description")).toString();
    m.minAppVersion = o.value(QStringLiteral("minAppVersion")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("permissions")).toArray())
        m.permissions << v.toString();

    if (ok) *ok = !m.id.isEmpty();
    return m;
}

static MediaItem itemFromJson(const QJsonObject& o)
{
    MediaItem it;
    it.id           = o.value(QStringLiteral("id")).toString();
    it.title        = o.value(QStringLiteral("title")).toString();
    it.subtitle     = o.value(QStringLiteral("subtitle")).toString();
    it.type         = o.value(QStringLiteral("type")).toString();
    it.thumbnailUrl = o.value(QStringLiteral("thumbnailUrl")).toString();
    it.url          = o.value(QStringLiteral("url")).toString();
    it.mime         = o.value(QStringLiteral("mime")).toString();
    return it;
}

MediaCatalog MediaCatalog::fromJson(const QByteArray& json)
{
    MediaCatalog c;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return c;
    const QJsonObject o = doc.object();
    c.title = o.value(QStringLiteral("title")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray())
        if (v.isObject()) c.items.push_back(itemFromJson(v.toObject()));
    return c;
}
