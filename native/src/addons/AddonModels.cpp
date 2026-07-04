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
    m.accent      = o.value(QStringLiteral("accent")).toString();
    m.minAppVersion = o.value(QStringLiteral("minAppVersion")).toString();
    m.updateUrl   = o.value(QStringLiteral("updateUrl")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("permissions")).toArray())
        m.permissions << v.toString();

    for (const QJsonValue& v : o.value(QStringLiteral("settings")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject so = v.toObject();
        AddonSetting s;
        s.key          = so.value(QStringLiteral("key")).toString();
        s.label        = so.value(QStringLiteral("label")).toString();
        s.type         = so.value(QStringLiteral("type")).toString();
        s.defaultValue = so.value(QStringLiteral("default")).toVariant().toString(); // string/bool/number -> string
        s.description  = so.value(QStringLiteral("description")).toString();
        if (!s.key.isEmpty()) m.settings.push_back(s);
    }

    for (const QJsonValue& v : o.value(QStringLiteral("catalogs")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject co = v.toObject();
        AddonCatalog c;
        c.id   = co.value(QStringLiteral("id")).toString();
        c.name = co.value(QStringLiteral("name")).toString();
        c.type = co.value(QStringLiteral("type")).toString();
        if (!c.id.isEmpty()) m.catalogs.push_back(c);
    }

    for (const QJsonValue& v : o.value(QStringLiteral("mediaTypes")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject to = v.toObject();
        AddonMediaType t;
        t.type         = to.value(QStringLiteral("type")).toString();
        t.color        = to.value(QStringLiteral("color")).toString();
        t.icon         = to.value(QStringLiteral("icon")).toString();
        t.openKind     = to.value(QStringLiteral("openKind")).toString();
        t.detailLayout = to.value(QStringLiteral("detailLayout")).toString();
        if (!t.type.isEmpty()) m.mediaTypes.push_back(t);
    }

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
    it.expandable   = o.value(QStringLiteral("expandable")).toBool();
    const QJsonArray alt = o.value(QStringLiteral("altNames")).toArray();
    for (const QJsonValue& v : alt) { const QString s = v.toString().trimmed(); if (!s.isEmpty()) it.altNames << s; }
    return it;
}

MediaCatalog MediaCatalog::fromJson(const QByteArray& json)
{
    MediaCatalog c;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return c;
    const QJsonObject o = doc.object();
    c.title = o.value(QStringLiteral("title")).toString();
    c.hasMore = o.value(QStringLiteral("hasMore")).toBool();
    for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray())
        if (v.isObject()) c.items.push_back(itemFromJson(v.toObject()));
    // Optional filter declarations: each has a key/label and a list of {value,label} options.
    for (const QJsonValue& fv : o.value(QStringLiteral("filters")).toArray())
    {
        if (!fv.isObject()) continue;
        const QJsonObject fo = fv.toObject();
        CatalogFilter cf;
        cf.key = fo.value(QStringLiteral("key")).toString();
        cf.label = fo.value(QStringLiteral("label")).toString();
        for (const QJsonValue& ov : fo.value(QStringLiteral("options")).toArray())
        {
            const QJsonObject oo = ov.toObject();
            cf.options.push_back({ oo.value(QStringLiteral("value")).toString(),
                                   oo.value(QStringLiteral("label")).toString() });
        }
        if (!cf.key.isEmpty() && !cf.options.isEmpty()) c.filters.push_back(cf);
    }
    return c;
}

MediaDetail MediaDetail::fromJson(const QByteArray& json)
{
    MediaDetail d;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return d;
    const QJsonObject o = doc.object();
    d.title    = o.value(QStringLiteral("title")).toString();
    d.subtitle = o.value(QStringLiteral("subtitle")).toString();
    d.overview = o.value(QStringLiteral("overview")).toString();
    d.imageUrl = o.value(QStringLiteral("image")).toString();
    d.imdbStreamId = o.value(QStringLiteral("imdbStreamId")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("facts")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject fo = v.toObject();
        MediaFact f;
        f.label = fo.value(QStringLiteral("label")).toString();
        f.value = fo.value(QStringLiteral("value")).toVariant().toString();
        if (!f.value.isEmpty()) d.facts.push_back(f);
    }
    d.valid = !d.title.isEmpty() || !d.overview.isEmpty() || !d.facts.isEmpty();
    return d;
}
