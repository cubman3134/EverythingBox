#include "HomebrewClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QUrl>

namespace {

const QLatin1String kLevelPrefix("homebrew:");
const QLatin1String kMorePrefix("homebrewmore:");

// Trailing slashes are common in a stored base URL and must not become "//homebrew".
QString trimmedBase(const QString& base)
{
    QString b = base;
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

QString pct(const QString& s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QString unpct(const QString& s)
{
    return QUrl::fromPercentEncoding(s.toLatin1());
}

} // namespace

QString HomebrewTitle::subtitle() const
{
    QStringList bits;
    if (!author.isEmpty())  bits << author;
    if (!version.isEmpty()) bits << QStringLiteral("v") + version;
    return bits.join(QStringLiteral(" · "));
}

namespace HomebrewClient
{

HomebrewPage parseList(const QByteArray& json)
{
    HomebrewPage page;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    // The romhack listing is a bare array and this one is an object; anything else — an error page, a
    // challenge body, `[]`, `null` — is not a page of homebrew and yields none.
    if (!doc.isObject()) return page;

    const QJsonObject o = doc.object();
    const QJsonArray arr = o.value(QStringLiteral("items")).toArray();
    page.items.reserve(arr.size());
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject row = v.toObject();
        HomebrewTitle t;
        t.id = row.value(QStringLiteral("id")).toString();
        if (t.id.isEmpty()) continue;   // a row we could never play is not worth offering
        t.title = row.value(QStringLiteral("title")).toString();
        t.author = row.value(QStringLiteral("author")).toString();
        t.version = row.value(QStringLiteral("version")).toString();
        t.description = row.value(QStringLiteral("description")).toString();
        t.imageUrl = row.value(QStringLiteral("imageUrl")).toString();
        page.items.push_back(t);
    }

    // Opaque, and only ever a string. A number or an object here is no cursor at all — paging on it would
    // build a URL nothing can answer — so it reads as "no more", which is the safe end of the mistake.
    const QJsonValue next = o.value(QStringLiteral("nextCursor"));
    if (next.isString()) page.nextCursor = next.toString();
    return page;
}

QString listUrl(const QString& base, const QString& systemId, const QString& cursor)
{
    // Percent-encoded as ONE path segment, ':' and '/' included. An id shaped like "https://evil/x" becomes a
    // segment on our own server rather than a request to somewhere else.
    QString u = trimmedBase(base) + QStringLiteral("/homebrew/") + pct(systemId);
    if (!cursor.isEmpty()) u += QStringLiteral("?cursor=") + pct(cursor);
    return u;
}

QString levelMime(const QString& system)
{
    return kLevelPrefix + system;
}

QString levelSystem(const QString& mime)
{
    if (!mime.startsWith(kLevelPrefix)) return QString();
    return mime.mid(QString(kLevelPrefix).size());
}

QString moreMime(const QString& system, const QVector<HomebrewMore>& more)
{
    // One record per line, base and cursor separated by a tab — and BOTH percent-encoded, so a cursor holding
    // a tab, a newline or anything else survives the round trip untouched. The cursor is opaque: a marker
    // format that could mangle one would be this client quietly interpreting it after all.
    QString s = kMorePrefix + system;
    for (const HomebrewMore& m : more)
    {
        if (m.base.isEmpty() || m.cursor.isEmpty()) continue;
        s += QLatin1Char('\n') + pct(m.base) + QLatin1Char('\t') + pct(m.cursor);
    }
    return s;
}

QString moreSystem(const QString& mime)
{
    if (!mime.startsWith(kMorePrefix)) return QString();
    return mime.mid(QString(kMorePrefix).size()).section(QLatin1Char('\n'), 0, 0);
}

QVector<HomebrewMore> moreCursors(const QString& mime)
{
    QVector<HomebrewMore> out;
    if (!mime.startsWith(kMorePrefix)) return out;
    const QStringList lines = mime.mid(QString(kMorePrefix).size()).split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size(); ++i)   // line 0 is the system
    {
        const QString& line = lines.at(i);
        const int tab = line.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        HomebrewMore m;
        m.base = unpct(line.left(tab));
        m.cursor = unpct(line.mid(tab + 1));
        if (m.base.isEmpty() || m.cursor.isEmpty()) continue;
        out.push_back(m);
    }
    return out;
}

} // namespace HomebrewClient
