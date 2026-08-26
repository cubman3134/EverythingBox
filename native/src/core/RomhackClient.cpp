#include "RomhackClient.h"

#include "AppPaths.h"

#include <QDateTime>
#include <QFile>
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

// One-line append to <app>/stream_debug.log, the same file StreamResolver (srLog), DownloadManager (dlLog)
// and MainWindow (mwLog) write to, and reached the same way. Local and named `…Log(` on purpose: the
// proxy-header log-discipline gate matches log calls by that SHAPE, so a helper spelled otherwise would be a
// hole in it. QtCore and a header-only AppPaths and nothing else — this file is a pure parser and is linked
// into probes that have Qt6::Core alone, so anything heavier would break them at the link rather than here.
static void rhLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A REFUSED reference, rendered so it is safe to write down. Refusing it is exactly the statement that we do
// not trust it, so it is not repeated verbatim: what came back can be anything at all, including an absolute
// url whose query carries a signed-URL token, and stream_debug.log is a file people paste into bug reports.
// So everything from the first '?' or '#' is cut (that is where a token rides), an absolute reference is
// reduced to its SCHEME — that it has one is the entire diagnosis, and the host is the other half of what
// must never land in this file — and the rest is length-capped. What survives is the shape of the reference,
// which is the thing anybody reading this line needs.
static QString logSafeRef(const QString& ref)
{
    QString s = ref.trimmed();
    if (s.isEmpty()) return QStringLiteral("(empty)");
    int cut = s.indexOf(QLatin1Char('?'));
    const int hash = s.indexOf(QLatin1Char('#'));
    if (hash >= 0 && (cut < 0 || hash < cut)) cut = hash;
    if (cut >= 0) s = s.left(cut) + QStringLiteral("…");
    // RFC 3986's own test for a scheme, the same one isSafeRelativeFileUrl applies: a ':' before the first
    // '/'. Everything after it can carry a host, so nothing after it is kept.
    const int colon = s.indexOf(QLatin1Char(':'));
    const int slash = s.indexOf(QLatin1Char('/'));
    if (colon >= 0 && (slash < 0 || colon < slash))
        return s.left(colon + 1) + QStringLiteral("… (absolute reference, rest withheld)");
    return s.left(200);
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

    // Hex is lowercased on the way in so no comparison downstream ever turns on case — the source's own
    // casing is not a fact about the ROM.
    const QJsonObject t = o.value(QStringLiteral("target")).toObject();
    f.target.fileName = t.value(QStringLiteral("fileName")).toString().trimmed();
    f.target.crc32 = t.value(QStringLiteral("crc32")).toString().trimmed().toLower();
    f.target.sha1 = t.value(QStringLiteral("sha1")).toString().trimmed().toLower();
    f.target.region = t.value(QStringLiteral("region")).toString().trimmed();

    for (const QJsonValue& v : o.value(QStringLiteral("patches")).toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject p = v.toObject();
        RomhackPatchFile pf;
        pf.name = p.value(QStringLiteral("name")).toString();
        pf.format = p.value(QStringLiteral("patchFormat")).toString();
        pf.url = p.value(QStringLiteral("url")).toString().trimmed();
        // A url we will not follow is a patch we cannot fetch, so the row is dropped here rather than
        // offered: the alternative is a menu entry that can only ever fail, chosen after someone read it.
        //
        // Traced, because the drop is otherwise invisible from every side. This guard enforces a contract
        // belonging to the SERVER, which this repo can neither see nor test — that every reference is
        // "<route>/<id>", an id whose alphabet contains none of the characters refused here. Let a route
        // start escaping a file name or appending a query and EVERY row goes at once, `valid` goes false,
        // and the only thing anyone is told is "couldn't get that hack's patch": word for word what a server
        // being down produces, with nothing to pull on. One line naming what was refused is the difference
        // between that and a five-minute diagnosis. Written HERE rather than at the call site because here
        // is the only place that still holds the reference — a count handed upwards would say how many
        // vanished but never which shape they had, which is the whole question.
        if (!isSafeRelativeFileUrl(pf.url))
        {
            rhLog(QStringLiteral("romhack: dropped patch \"%1\" of fetch %2 — url is not a relative "
                                 "reference: %3").arg(pf.name.left(120), f.id.left(120),
                                                      logSafeRef(pf.url)));
            continue;
        }
        f.patches.push_back(pf);
    }

    // A fetch with no usable patch is not a valid fetch: there is nothing to install, and saying so here
    // saves every caller from having to check the list separately.
    f.valid = !f.patches.isEmpty();
    return f;
}

bool isSafeRelativeFileUrl(const QString& url)
{
    const QString u = url.trimmed();
    if (u.isEmpty()) return false;
    // "/x" is rooted on the host and "//host/x" is protocol-relative; both leave the path we were given.
    if (u.startsWith(QLatin1Char('/'))) return false;
    // A backslash is not a url separator. It is a Windows path, and reading it as one segment would let
    // "..\\..\\secrets" past the segment check below.
    if (u.contains(QLatin1Char('\\'))) return false;
    // Every reference this client is ever handed is "romhack-file/<id>", where the id is the file's path
    // base64'd, padding stripped, '+' and '/' swapped for '-' and '_' — so its alphabet is [A-Za-z0-9_-]
    // and none of these three can occur in a real one. That is a statement about the SERVER, which this
    // repo cannot see or test, so it is written down here to be checked against rather than assumed: if a
    // route ever escapes a name or appends a query instead, every patch is dropped at parse time and the
    // failure reads as "the source has nothing". '%' goes because the ".." test below
    // reads the raw string, and an encoded "%2e%2e" would walk straight past it. '#' and '?' go because they
    // truncate what actually reaches the server — nothing after them is part of the path asked for — so a
    // reference carrying one fetches something other than the file it names.
    if (u.contains(QLatin1Char('%')) || u.contains(QLatin1Char('#')) || u.contains(QLatin1Char('?')))
        return false;
    // RFC 3986's own test for a scheme: a ':' before the first '/'. That catches "https:", "file:" and
    // "javascript:" — and "C:", which is why a drive path needs no rule of its own.
    const int colon = u.indexOf(QLatin1Char(':'));
    const int slash = u.indexOf(QLatin1Char('/'));
    if (colon >= 0 && (slash < 0 || colon < slash)) return false;
    // Any ".." segment climbs out of the route — and, on the far side, out of the staging root.
    const QStringList segments = u.split(QLatin1Char('/'));
    for (const QString& seg : segments)
        if (seg == QStringLiteral("..")) return false;
    return true;
}

QString fileUrl(const QString& base, const QString& relative)
{
    if (!isSafeRelativeFileUrl(relative)) return QString();
    QString b = base;
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    if (b.isEmpty()) return QString();
    return b + QLatin1Char('/') + relative.trimmed();
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
