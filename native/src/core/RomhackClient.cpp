#include "RomhackClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

QString RomhackEntry::menuLabel() const
{
    QString s = title.isEmpty() ? QStringLiteral("(untitled)") : title;
    if (!version.isEmpty()) s += QStringLiteral("  v") + version;

    // The trailing bracket is the "where from / what kind / who by" line. Each part is dropped when the
    // source did not supply it, so a sparse listing reads as a plain title rather than "Hack ( ·  · )".
    QStringList bits;
    if (!category.isEmpty()) bits << category;
    if (!language.isEmpty()) bits << language;
    if (!releasedBy.isEmpty()) bits << releasedBy;
    if (!source.isEmpty()) bits << source;
    if (!bits.isEmpty()) s += QStringLiteral("   —   ") + bits.join(QStringLiteral(" · "));
    return s;
}

namespace RomhackClient
{

QVector<RomhackEntry> parseList(const QByteArray& json)
{
    QVector<RomhackEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;   // an error page, a challenge body, anything unexpected: no hacks

    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RomhackEntry e;
        e.id = o.value(QStringLiteral("id")).toString();
        if (e.id.isEmpty()) continue; // a row we could never fetch is not worth offering
        e.source = o.value(QStringLiteral("source")).toString();
        e.title = o.value(QStringLiteral("title")).toString();
        e.releasedBy = o.value(QStringLiteral("releasedBy")).toString();
        e.version = o.value(QStringLiteral("version")).toString();
        e.category = o.value(QStringLiteral("category")).toString();
        e.language = o.value(QStringLiteral("language")).toString();
        e.genre = o.value(QStringLiteral("genre")).toString();
        e.date = o.value(QStringLiteral("date")).toString();
        out.push_back(e);
    }
    return out;
}

RomhackFetch parseFetch(const QByteArray& json)
{
    RomhackFetch f;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return f;

    const QJsonObject o = doc.object();
    f.id = o.value(QStringLiteral("id")).toString();
    f.version = o.value(QStringLiteral("version")).toString();
    f.targetNote = o.value(QStringLiteral("targetNote")).toString();

    for (const QJsonValue& v : o.value(QStringLiteral("patches")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject p = v.toObject();
        RomhackPatchFile pf;
        pf.name = p.value(QStringLiteral("name")).toString();
        pf.format = p.value(QStringLiteral("patchFormat")).toString();
        // Base64 is how JSON carries bytes. A patch that does not decode is dropped rather than passed on as
        // an empty buffer, which the applier would refuse anyway with a less useful message.
        pf.bytes = QByteArray::fromBase64(p.value(QStringLiteral("bytes")).toString().toLatin1());
        if (pf.bytes.isEmpty()) continue;
        f.patches.push_back(pf);
    }

    // A fetch with no usable patch is not a valid fetch: there is nothing to install, and saying so here
    // saves every caller from having to check the list separately.
    f.valid = !f.patches.isEmpty();
    return f;
}

QString listUrl(const QString& base, const QString& systemId, const QString& title)
{
    QString b = base;
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b + QStringLiteral("/romhacks/") + QString::fromLatin1(QUrl::toPercentEncoding(systemId))
         + QStringLiteral("?title=") + QString::fromLatin1(QUrl::toPercentEncoding(title));
}

QString fetchUrl(const QString& base, const QString& id)
{
    QString b = base;
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    // Percent-encoded as ONE path segment, ':' and '/' included. An id shaped like "https://evil/x" becomes a
    // segment on our own server rather than a request to somewhere else.
    return b + QStringLiteral("/romhack/") + QString::fromLatin1(QUrl::toPercentEncoding(id));
}

} // namespace RomhackClient
