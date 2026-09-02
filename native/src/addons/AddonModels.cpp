#include "AddonModels.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <algorithm>

// -------------------------------------------------------------------------------- MediaArt

// A few provider spellings map onto one canonical role so themes bind a single name.
static QString canonRole(const QString& r)
{
    if (r == QStringLiteral("boxart") || r == QStringLiteral("cover") || r == QStringLiteral("boxfront"))
        return QStringLiteral("box");
    if (r == QStringLiteral("wheel") || r == QStringLiteral("marquee"))
        return QStringLiteral("logo");
    if (r == QStringLiteral("titlescreen") || r == QStringLiteral("snap") || r == QStringLiteral("screenshots"))
        return QStringLiteral("screenshot");
    if (r == QStringLiteral("fanarts")) return QStringLiteral("fanart");
    if (r == QStringLiteral("grid"))    return QStringLiteral("poster");
    return r;
}

void MediaArt::addImage(const QString& role, const QString& url)
{
    if (role.isEmpty() || url.isEmpty()) return;
    QStringList& l = images[canonRole(role)];
    if (!l.contains(url)) l << url;
}

void MediaArt::mergeLowerPriority(const MediaArt& other)
{
    for (auto it = other.images.constBegin(); it != other.images.constEnd(); ++it)
        for (const QString& u : it.value()) addImage(it.key(), u);
    for (const QString& v : other.videos) if (!videos.contains(v)) videos << v;
    for (const QString& a : other.audio)  if (!audio.contains(a))  audio  << a;
    for (auto it = other.meta.constBegin(); it != other.meta.constEnd(); ++it)
        if (!meta.contains(it.key())) meta.insert(it.key(), it.value());
}

// Read a value that may be a single string, an array of strings, or an array of { url } objects.
static QStringList urlList(const QJsonValue& jv)
{
    QStringList out;
    if (jv.isString()) { const QString s = jv.toString(); if (!s.isEmpty()) out << s; }
    else for (const QJsonValue& v : jv.toArray())
    {
        if (v.isString()) { const QString s = v.toString(); if (!s.isEmpty()) out << s; }
        else if (v.isObject())
        {
            const QString u = v.toObject().value(QStringLiteral("url")).toString();
            if (!u.isEmpty()) out << u;
        }
    }
    return out;
}

MediaArt MediaArt::fromJson(const QJsonObject& o)
{
    MediaArt a;
    // images: { role: "url" | ["url", ...] | [{url}] }
    const QJsonObject imgs = o.value(QStringLiteral("images")).toObject();
    for (auto it = imgs.constBegin(); it != imgs.constEnd(); ++it)
        for (const QString& u : urlList(it.value())) a.addImage(it.key(), u);
    // Convenience: a provider may also send flat single-role keys instead of nesting under "images".
    static const char* flatRoles[] = { "logo", "clearlogo", "wheel", "box", "boxart", "cover", "hero",
                                        "banner", "fanart", "background", "screenshot", "screenshots",
                                        "disc", "icon", "poster", "grid" };
    for (const char* r : flatRoles)
    {
        const QString role = QString::fromLatin1(r);
        for (const QString& u : urlList(o.value(role))) a.addImage(role, u);
    }
    // videos / audio: array (or single) of urls or { url } objects. Accept a couple of synonyms.
    a.videos = urlList(o.value(QStringLiteral("videos")));
    if (a.videos.isEmpty()) a.videos = urlList(o.value(QStringLiteral("video")));
    if (a.videos.isEmpty()) a.videos = urlList(o.value(QStringLiteral("trailers")));
    a.audio = urlList(o.value(QStringLiteral("audio")));
    if (a.audio.isEmpty()) a.audio = urlList(o.value(QStringLiteral("music")));
    // meta: arbitrary object copied verbatim (extensible; providers add their own fact keys).
    a.meta = o.value(QStringLiteral("meta")).toObject().toVariantMap();
    return a;
}

QVariantMap MediaArt::toVariant() const
{
    QVariantMap m;
    QVariantMap imgs;
    for (auto it = images.constBegin(); it != images.constEnd(); ++it)
        imgs.insert(it.key(), QVariant(it.value()));
    if (!imgs.isEmpty()) m.insert(QStringLiteral("images"), imgs);
    if (!videos.isEmpty()) m.insert(QStringLiteral("videos"), QVariant(videos));
    if (!audio.isEmpty())  m.insert(QStringLiteral("audio"),  QVariant(audio));
    if (!meta.isEmpty())   m.insert(QStringLiteral("meta"),   meta);
    return m;
}

void MediaArt::writeInto(QVariantMap& row) const
{
    if (!images.isEmpty())
    {
        QVariantMap imgs;
        for (auto it = images.constBegin(); it != images.constEnd(); ++it)
        {
            imgs.insert(it.key(), QVariant(it.value()));
            // scalar alias (selected.logo / selected.box / ...) = that role's best url, but never overwrite a
            // key the row already carries (title/type/image/accent/...), so reserved fields stay intact.
            if (!it.value().isEmpty() && !row.contains(it.key()))
                row.insert(it.key(), it.value().first());
        }
        row.insert(QStringLiteral("images"), imgs);
    }
    if (!videos.isEmpty()) row.insert(QStringLiteral("videos"), QVariant(videos));
    if (!audio.isEmpty())  row.insert(QStringLiteral("audio"),  QVariant(audio));
    if (!meta.isEmpty())   row.insert(QStringLiteral("meta"),   meta);
}

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
    for (const QJsonValue& v : o.value(QStringLiteral("metaFor")).toArray())
    {
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) m.metaFor << s;
    }

    // Optional protocol resources (#188). Unknown names are kept verbatim rather than filtered against a
    // list of the ones this build understands: declares() asks for a name, so an unknown one is inert here,
    // and a whitelist would mean a client older than a resource DROPS an addon's declaration of it — which
    // is a silent capability loss the addon author cannot see. A resource with no `name` is skipped.
    for (const QJsonValue& v : o.value(QStringLiteral("resources")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject ro = v.toObject();
        AddonResource r;
        r.name = ro.value(QStringLiteral("name")).toString().trimmed();
        for (const QJsonValue& tv : ro.value(QStringLiteral("types")).toArray())
        {
            const QString t = tv.toString().trimmed();
            if (!t.isEmpty()) r.types << t;
        }
        if (!r.name.isEmpty()) m.resources.push_back(r);
    }

    if (ok) *ok = !m.id.isEmpty();
    return m;
}

// -------------------------------------------------------------------- chapters / pages (issue #188)

AddonChapterList AddonChapterList::fromJson(const QByteArray& json)
{
    AddonChapterList out;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    for (const QJsonValue& v : o.value(QStringLiteral("chapters")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject co = v.toObject();
        AddonChapter c;
        c.id        = co.value(QStringLiteral("id")).toString();
        // A number arrives as a string ("9.5") or, from a source that stored it as one, as a JSON number.
        // toVariant().toString() takes both; the model keeps it as text either way.
        c.number    = co.value(QStringLiteral("number")).toVariant().toString().trimmed();
        c.volume    = co.value(QStringLiteral("volume")).toVariant().toString().trimmed();
        c.title     = co.value(QStringLiteral("title")).toString();
        c.language  = co.value(QStringLiteral("language")).toString();
        c.group     = co.value(QStringLiteral("group")).toString();
        c.published = co.value(QStringLiteral("published")).toString();
        c.pageCount = co.contains(QStringLiteral("pageCount"))
                          ? co.value(QStringLiteral("pageCount")).toInt(-1) : -1;
        if (!c.id.isEmpty()) out.chapters.push_back(c);   // an entry with no id cannot be opened
    }
    out.hasMore = o.value(QStringLiteral("hasMore")).toBool();
    return out;
}

AddonPageList AddonPageList::fromJson(const QByteArray& json)
{
    AddonPageList out;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    for (const QJsonValue& v : o.value(QStringLiteral("pages")).toArray())
    {
        AddonPage p;
        // A page may be a bare url string — the shortest thing a static addon can write — or an object.
        if (v.isString()) p.url = v.toString();
        else if (v.isObject())
        {
            const QJsonObject po = v.toObject();
            p.url     = po.value(QStringLiteral("url")).toString();
            p.width   = po.value(QStringLiteral("width")).toInt();
            p.height  = po.value(QStringLiteral("height")).toInt();
            p.headers = StreamHeaders::parseHeaderMap(po.value(QStringLiteral("headers")).toObject());
        }
        if (!p.url.isEmpty()) out.pages.push_back(p);
    }
    return out;
}

namespace
{
    // The leading digit run of `s` from `i`, as a (value, length) pair. Values are compared as text with
    // leading zeros stripped — longer means bigger — so a chapter number longer than any integer type does
    // not wrap. Nothing publishes one, but a source is untrusted input and "9999999999999999999" costing an
    // ordering is not a trade worth making for a std::stoll.
    QString digitRun(const QString& s, int& i)
    {
        const int start = i;
        while (i < s.size() && s.at(i).isDigit()) ++i;
        QString run = s.mid(start, i - start);
        int z = 0;
        while (z + 1 < run.size() && run.at(z) == QLatin1Char('0')) ++z;
        return run.mid(z);
    }
}

bool AddonChapters::numberLess(const QString& a, const QString& b)
{
    // No number sorts LAST, and two no-numbers are equal (so a stable sort keeps source order between them).
    if (a.isEmpty() || b.isEmpty()) return !a.isEmpty() && b.isEmpty();
    int i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const bool da = a.at(i).isDigit(), db = b.at(j).isDigit();
        if (da && db)
        {
            const QString ra = digitRun(a, i), rb = digitRun(b, j);
            if (ra.size() != rb.size()) return ra.size() < rb.size();
            const int c = QString::compare(ra, rb);
            if (c != 0) return c < 0;
            continue;
        }
        // A digit sorts before a letter, so "12" precedes "12a" precedes "Extra".
        if (da != db) return da;
        const int c = QString::compare(a.at(i), b.at(j), Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        ++i; ++j;
    }
    return (a.size() - i) < (b.size() - j);
}

void AddonChapters::sortNaturally(QVector<AddonChapter>& chapters)
{
    std::stable_sort(chapters.begin(), chapters.end(),
                     [](const AddonChapter& x, const AddonChapter& y) { return numberLess(x.number, y.number); });
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
    it.parentId     = o.value(QStringLiteral("parentId")).toString();
    it.parentTitle  = o.value(QStringLiteral("parentTitle")).toString();
    const QJsonArray alt = o.value(QStringLiteral("altNames")).toArray();
    for (const QJsonValue& v : alt) { const QString s = v.toString().trimmed(); if (!s.isEmpty()) it.altNames << s; }
    // Rich artwork/videos/audio/meta (optional). Keep thumbnailUrl the canonical grid poster; also register
    // it as a "thumb" candidate so a theme binding selected.images.thumb works even without a richer provider.
    it.art = MediaArt::fromJson(o);
    if (!it.thumbnailUrl.isEmpty()) it.art.addImage(QStringLiteral("thumb"), it.thumbnailUrl);
    else if (!it.art.image(QStringLiteral("poster")).isEmpty())
        it.thumbnailUrl = it.art.image(QStringLiteral("poster")); // provider gave art but no thumbnailUrl
    else if (!it.art.image(QStringLiteral("box")).isEmpty())
        it.thumbnailUrl = it.art.image(QStringLiteral("box"));
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
    d.parentId     = o.value(QStringLiteral("parentId")).toString();
    d.parentTitle  = o.value(QStringLiteral("parentTitle")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("facts")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject fo = v.toObject();
        MediaFact f;
        f.label = fo.value(QStringLiteral("label")).toString();
        f.value = fo.value(QStringLiteral("value")).toVariant().toString();
        if (!f.value.isEmpty()) d.facts.push_back(f);
    }
    // Rich artwork/videos/audio/meta (optional). Register the primary cover as a "poster" candidate too, so
    // it participates in role-based bindings and the aggregator's precedence merge.
    d.art = MediaArt::fromJson(o);
    if (!d.imageUrl.isEmpty()) d.art.addImage(QStringLiteral("poster"), d.imageUrl);
    else if (!d.art.image(QStringLiteral("poster")).isEmpty())
        d.imageUrl = d.art.image(QStringLiteral("poster"));
    d.valid = !d.title.isEmpty() || !d.overview.isEmpty() || !d.facts.isEmpty() || !d.art.isEmpty();
    return d;
}
