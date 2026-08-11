// Pure OPDS 1.2 (Atom XML) feed parser — the mutation-tested heart of the OPDS catalog feature (#146).
//
// OPDS is "Atom XML over plain HTTP" (RFC 4287 + the OPDS catalog profile): a self-hosted book server
// (Calibre-web, Kavita, Komga, Ubooquity, …) publishes a <feed> of <entry> elements, and each entry's
// <link> elements are classified by their rel/type into three kinds this file recognises:
//
//   * NAVIGATION — a link to ANOTHER feed (a shelf/section to drill into). Its type carries the OPDS
//     catalog profile ("application/atom+xml;profile=opds-catalog…"), or its rel is "subsection".
//   * ACQUISITION — a DOWNLOADABLE book. Its rel contains "http://opds-spec.org/acquisition" (plain,
//     open-access, borrow, buy — all acquisition variants). The href + mime type name the file.
//   * COVER/THUMBNAIL — the entry's cover art. Its rel contains "opds-spec.org/image" or "/thumbnail".
//
// Deliberately header-only and QtCore-only (QXmlStreamReader + QUrl): NO network, NO disk. The fetch lives
// in the UI layer; this parses bytes already in hand, so a headless probe pins the classification and href
// resolution exhaustively without a socket. parseManifest discipline: a malformed feed is best-effort and
// NEVER throws — you get whatever entries parsed cleanly, or an empty feed, never a crash.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QUrl>
#include <QXmlStreamReader>

// One <link> inside an entry, already classified and with its href resolved to absolute (see resolveHref).
struct OpdsLink
{
    QString rel;   // the Atom link relation ("http://opds-spec.org/acquisition", "subsection", …)
    QString href;  // ABSOLUTE url (resolved against the feed's base url at parse time)
    QString type;  // the mime type ("application/epub+zip", "application/atom+xml;profile=opds-catalog…")
};

struct OpdsEntry
{
    QString title;
    QString author;               // first <author><name>; empty if the entry declares none
    QString id;
    QString summary;              // <summary> or <content> plain text (best-effort; empty for xhtml content)
    QString coverHref;            // absolute cover url; a full image is preferred over a thumbnail
    QVector<OpdsLink> acquisition; // downloadable-book links (may be several formats)
    QVector<OpdsLink> navigation;  // drill-into-another-feed links
};

struct OpdsFeed
{
    QString title;                 // the feed's own <title> (the shelf name)
    QString id;                    // the feed's <id>
    QVector<OpdsEntry> entries;
};

// Resolve a link href against the feed's base url. An already-absolute href is returned unchanged; a
// relative one ("sub.xml", "/img/1.png", "../up.xml") is resolved against `base`. An empty base leaves a
// relative href as-is (we have nothing to resolve against). Pure — no I/O. Exposed so the probe pins it.
inline QString resolveHref(const QString& base, const QString& href)
{
    if (href.isEmpty())
        return href;
    const QUrl h(href);
    if (!h.isRelative())
        return href;                       // already absolute (has a scheme) — leave it alone
    if (base.isEmpty())
        return href;                       // nothing to resolve against
    return QUrl(base).resolved(h).toString();
}

// Build the HTTP Basic auth header VALUE ("Basic <base64(user:pass)>") for a catalog's credentials, or an
// empty string when there is no username. Pure (QByteArray::toBase64) so the probe pins the encoding with
// no network trip. SAFETY: the returned value embeds the password — it is a request header, NEVER a log line.
inline QString opdsBasicAuth(const QString& user, const QString& pass)
{
    if (user.isEmpty())
        return QString();
    const QByteArray token = (user + QLatin1Char(':') + pass).toUtf8().toBase64();
    return QStringLiteral("Basic ") + QString::fromLatin1(token);
}

namespace OpdsDetail {

// Fold a single classified link into the entry. Precedence is deliberate and disjoint in practice:
//   1. a cover (rel names an image/thumbnail) — a full image wins over a thumbnail for coverHref;
//   2. an acquisition link (rel names acquisition) — a downloadable book;
//   3. a navigation link (type carries the OPDS catalog profile, or rel is "subsection") — a sub-feed.
// Anything else (self/alternate/start/related/search) is intentionally ignored.
inline void classifyLink(OpdsEntry& e, const OpdsLink& lk, bool& haveFullCover)
{
    if (lk.rel.contains(QLatin1String("opds-spec.org/image")) || lk.rel.contains(QLatin1String("/thumbnail")))
    {
        const bool isThumb = lk.rel.contains(QLatin1String("/thumbnail"));
        if (!isThumb)            { e.coverHref = lk.href; haveFullCover = true; }  // full image: always preferred
        else if (!haveFullCover) { e.coverHref = lk.href; }                        // thumbnail: only as a fallback
        return;
    }
    if (lk.rel.contains(QLatin1String("opds-spec.org/acquisition")))
    {
        e.acquisition.push_back(lk);
        return;
    }
    if (lk.type.contains(QLatin1String("profile=opds-catalog")) || lk.rel == QLatin1String("subsection"))
    {
        e.navigation.push_back(lk);
        return;
    }
    // otherwise ignored
}

} // namespace OpdsDetail

// Parse an OPDS Atom feed. `baseUrl` is the url the feed was fetched from; relative link/cover hrefs are
// resolved against it. Best-effort and exception-free: on malformed XML you get whatever parsed, or empty.
inline OpdsFeed parseOpds(const QByteArray& xml, const QString& baseUrl = QString())
{
    OpdsFeed feed;
    QXmlStreamReader r(xml);
    OpdsEntry cur;
    bool inEntry = false;
    bool inAuthor = false;
    bool haveFullCover = false;
    QString text;   // char data accumulated for the currently-open leaf element

    while (!r.atEnd())
    {
        r.readNext();
        if (r.isStartElement())
        {
            const QStringView name = r.name();
            if (name == QLatin1String("entry"))
            {
                inEntry = true;
                cur = OpdsEntry();
                haveFullCover = false;
            }
            else if (name == QLatin1String("author"))
            {
                inAuthor = true;
            }
            else if (name == QLatin1String("link") && inEntry)
            {
                const QXmlStreamAttributes a = r.attributes();
                OpdsLink lk;
                lk.rel  = a.value(QLatin1String("rel")).toString();
                lk.type = a.value(QLatin1String("type")).toString();
                lk.href = resolveHref(baseUrl, a.value(QLatin1String("href")).toString());
                OpdsDetail::classifyLink(cur, lk, haveFullCover);
            }
            text.clear();
        }
        else if (r.isCharacters() && !r.isWhitespace())
        {
            text += r.text();
        }
        else if (r.isEndElement())
        {
            const QStringView name = r.name();
            if (name == QLatin1String("entry"))
            {
                if (inEntry)
                    feed.entries.push_back(cur);
                inEntry = false;
            }
            else if (name == QLatin1String("author"))
            {
                inAuthor = false;
            }
            else if (name == QLatin1String("title"))
            {
                if (inEntry)                 cur.title  = text.trimmed();
                else if (feed.title.isEmpty()) feed.title = text.trimmed();
            }
            else if (name == QLatin1String("id"))
            {
                if (inEntry)              cur.id   = text.trimmed();
                else if (feed.id.isEmpty()) feed.id = text.trimmed();
            }
            else if (name == QLatin1String("name"))
            {
                if (inEntry && inAuthor && cur.author.isEmpty())
                    cur.author = text.trimmed();
            }
            else if (name == QLatin1String("summary") || name == QLatin1String("content"))
            {
                if (inEntry && cur.summary.isEmpty())
                    cur.summary = text.trimmed();
            }
            text.clear();
        }
    }
    return feed;
}
