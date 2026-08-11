#include "StreamResolver.h"
#include "../core/AppBrand.h"
#include "../core/NetHeaderApply.h"

#include "../core/AppPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

// One-line append to <app>/stream_debug.log, shared with the addon stream/manga resolution tracing.
static void srLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A log-safe rendering of a URL: scheme://host[:port]/…/<filename>. Drops the path's middle segments (which
// can carry an addon access token) and the query string (which can carry debrid keys), so logs never leak secrets.
static QString logSafeUrl(const QString& url)
{
    const QUrl u(url);
    if (u.scheme().isEmpty()) return QFileInfo(url).fileName(); // a local path
    const QString file = QFileInfo(u.path()).fileName();
    return u.scheme() + QStringLiteral("://") + u.host()
         + (u.port() > 0 ? QStringLiteral(":") + QString::number(u.port()) : QString())
         + QStringLiteral("/…/") + file;
}

// ---- .m3u / .m3u8 playlist support ------------------------------------------------------------
// Three flavours share this extension: an HLS manifest (segment list / master) which libmpv streams
// directly; an IPTV-style media playlist (a list of channel/track URLs) which we turn into a queue;
// and a PlayStation multi-disc list which the emulator loads. classify() tells them apart.

// True when the URL/path points at a playlist file (ignoring any ?query).
bool StreamResolver::isM3uRef(const QString& s)
{
    QString p = s;
    const int q = p.indexOf(QLatin1Char('?'));
    if (q >= 0) p = p.left(q);
    p = p.toLower();
    return p.endsWith(QStringLiteral(".m3u")) || p.endsWith(QStringLiteral(".m3u8"));
}

// HLS manifests carry #EXT-X-* tags (TARGETDURATION, STREAM-INF, MEDIA-SEQUENCE, …); a plain media
// playlist has only #EXTM3U/#EXTINF and full entry URLs. The former is one stream for libmpv to chew.
bool StreamResolver::isHlsManifest(const QString& text) { return text.contains(QStringLiteral("#EXT-X-")); }

// Pull one key="value" pair out of an #EXTINF attribute section (#75). Returns empty when the key is
// absent OR its value is not double-quoted — a malformed/unquoted attribute degrades to an empty field
// rather than throwing or corrupting the title. The value between the quotes is taken verbatim, so a
// comma inside it (group-title="News, US") is preserved; the caller has already stripped the title off.
static QString extinfAttr(const QString& attrs, const QString& key)
{
    const QString needle = key + QStringLiteral("=\"");
    const int k = attrs.indexOf(needle);
    if (k < 0) return QString();
    const int valStart = k + needle.size();
    const int end = attrs.indexOf(QLatin1Char('"'), valStart);
    if (end < 0) return QString();
    return attrs.mid(valStart, end - valStart);
}

// Parse #EXTINF titles + IPTV attributes + entry URLs, resolving relative entries against the playlist's
// own location. A real IPTV EXTINF line is
//   #EXTINF:-1 tvg-id="cnn.us" tvg-name="CNN" tvg-logo="http://x/cnn.png" group-title="News, US",CNN HD
// i.e. an attribute section of key="value" pairs, then a comma, then the display title. tvg-logo drives
// tile art and group-title sections the channel list; tvg-id/tvg-name are kept for a later EPG increment.
QVector<M3uEntry> StreamResolver::parseM3u(const QString& text, const QString& src)
{
    QVector<M3uEntry> out;
    const bool srcIsUrl = src.contains(QStringLiteral("://"));
    const int slash = src.lastIndexOf(QLatin1Char('/'));
    const QString base = srcIsUrl ? (slash >= 0 ? src.left(slash + 1) : src)
                                  : (QFileInfo(src).absolutePath() + QLatin1Char('/'));
    QString title, logo, group, tvgId, tvgName;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("#EXTINF")))
        {
            // The title is the text after the comma that ENDS the attribute section — NOT indexOf(',').
            // A quoted attribute value may itself contain a comma (group-title="News, US"), so the
            // delimiting comma is the first one OUTSIDE double quotes. Parse the key="value" pairs first
            // (a comma inside quotes belongs to the value), then split the title off at that comma.
            int c = -1;
            bool inQuotes = false;
            for (int i = 0; i < line.size(); ++i)
            {
                const QChar ch = line.at(i);
                if (ch == QLatin1Char('"'))                       inQuotes = !inQuotes;
                else if (ch == QLatin1Char(',') && !inQuotes)     { c = i; break; }
            }
            const QString attrs = c >= 0 ? line.left(c) : line;   // the #EXTINF:<dur> ...key="value"... part
            if (c >= 0) title = line.mid(c + 1).trimmed();        // text after the delimiting comma is the name
            logo    = extinfAttr(attrs, QStringLiteral("tvg-logo"));
            group   = extinfAttr(attrs, QStringLiteral("group-title"));
            tvgId   = extinfAttr(attrs, QStringLiteral("tvg-id"));
            tvgName = extinfAttr(attrs, QStringLiteral("tvg-name"));
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) continue; // any other directive
        QString url;
        if (line.contains(QStringLiteral("://")))        url = line;                              // absolute
        else if (srcIsUrl)                               url = QUrl(base).resolved(QUrl(line)).toString();
        else if (QFileInfo(line).isAbsolute())           url = line;
        else                                             url = base + line;                       // relative to file
        out.push_back({ title.isEmpty() ? QFileInfo(line).fileName() : title, url, logo, group, tvgId, tvgName });
        title.clear(); logo.clear(); group.clear(); tvgId.clear(); tvgName.clear();
    }
    return out;
}

// The playlist's own declared EPG url, off the `#EXTM3U` header line (#75 inc 3). A real header is
//   #EXTM3U url-tvg="http://host/epg.xml.gz" x-tvg-url="http://host/epg2.xml"
// url-tvg is the common spelling; x-tvg-url is the older synonym — either wins, url-tvg first. Only the FIRST
// #EXTM3U line is consulted (a stray later line is not a header). Reuses extinfAttr's quoted key="value" pull.
QString StreamResolver::m3uHeaderTvgUrl(const QString& text)
{
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString& raw : lines)
    {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (!line.startsWith(QStringLiteral("#EXTM3U"))) return QString();   // header is the first line or not there
        const QString u = extinfAttr(line, QStringLiteral("url-tvg"));
        if (!u.isEmpty()) return u;
        return extinfAttr(line, QStringLiteral("x-tvg-url"));
    }
    return QString();
}

// A PlayStation multi-disc list: every entry is a disc image the libretro core can swap between.
bool StreamResolver::looksLikeDiscPlaylist(const QVector<M3uEntry>& entries)
{
    if (entries.isEmpty()) return false;
    static const QStringList disc = { "cue", "chd", "bin", "iso", "pbp", "img", "ccd" };
    for (const M3uEntry& e : entries)
    {
        const QString path = QUrl(e.url).path().isEmpty() ? e.url : QUrl(e.url).path();
        if (!disc.contains(QFileInfo(path).suffix().toLower())) return false;
    }
    return true;
}

// One forPlayUrl call per entry — the same question every other consumer of a stream's headers asks, so
// there is one definition of "same origin" and probe_stremio already pins it. Deliberately NOT "does the
// playlist have headers at all, and if so give them to everyone": an IPTV list routinely mixes a provider's
// own gated channels with third-party links, and the second kind must leave with nothing.
QVector<StreamHeaders::Headers> StreamResolver::entryHeaders(const QVector<M3uEntry>& entries,
                                                             const QString& src,
                                                             const StreamHeaders::Headers& headers)
{
    QVector<StreamHeaders::Headers> out;
    out.reserve(entries.size());
    for (const M3uEntry& e : entries) out << StreamHeaders::forPlayUrl(headers, src, e.url);
    return out;
}

StreamResolver::StreamResolver(QObject* parent) : QObject(parent) {}

// Read an .m3u/.m3u8 (local file or remote URL), then hand its text to classify() for dispatch.
void StreamResolver::resolve(const QString& src, const QString& title, const StreamHeaders::Headers& headers)
{
    if (!src.contains(QStringLiteral("://")))
    {
        QFile f(src);
        QString text;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) text = QString::fromUtf8(f.readAll());
        classify(src, text, title.isEmpty() ? QFileInfo(src).completeBaseName() : title, {});
        return;
    }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    emit status(tr("Loading playlist…"));
    srLog(QStringLiteral("m3u: GET %1%2").arg(logSafeUrl(src),
          headers.isEmpty() ? QString() : QStringLiteral(" + ") + StreamHeaders::logSummary(headers)));
    QNetworkRequest rq{ QUrl(src) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    // The source's own proxyHeaders, and the redirect gate that has to come with them, both in NetHeaderApply
    // (which is where the m3u fetch's version of this used to live, until the download queue needed the same
    // three rules and got none of them — #59). Applied AFTER our default UA so a stream that specifies a
    // User-Agent replaces it rather than being appended to. Without them the manifest fetch is refused by
    // exactly the hosts this feature exists for, and the failure looks like a broken playlist rather than a
    // missing header.
    //
    // Unlike the player's own redirect following, this one is interceptable in-process, so it is intercepted:
    // a cross-origin hop is aborted, landing on the fetch-failed path below, which hands the URL to the
    // player exactly as an auth failure does.
    QNetworkReply* reply = NetHeaderApply::get(nam_, rq, headers, src, [](bool allowed, const QUrl& to) {
        srLog(allowed ? QStringLiteral("m3u: same-origin redirect -> %1, headers still apply")
                            .arg(logSafeUrl(to.toString()))
                      : QStringLiteral("m3u: cross-origin redirect -> %1, refusing to carry this source's "
                                       "headers there -> player").arg(logSafeUrl(to.toString())));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, src, title, headers] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            // Couldn't fetch the manifest text (auth, headers, live-only) - let libmpv try the URL itself.
            srLog(QStringLiteral("m3u: fetch failed (%1) -> player").arg(reply->errorString()));
            emit playDirect(src, title, headers);
            return;
        }
        classify(src, QString::fromUtf8(reply->readAll()), title, headers);
    });
}

void StreamResolver::classify(const QString& src, const QString& text, const QString& title,
                              const StreamHeaders::Headers& headers)
{
    if (isHlsManifest(text))                       // a single adaptive stream: libmpv handles the segments
    {
        srLog(QStringLiteral("m3u: HLS manifest -> player"));
        emit playDirect(src, title, headers);
        return;
    }
    const QVector<M3uEntry> entries = parseM3u(text, src);
    if (entries.isEmpty())                         // not a recognisable list - best effort: play the URL
    {
        srLog(QStringLiteral("m3u: no entries -> player"));
        emit playDirect(src, title, headers);
        return;
    }
    if (looksLikeDiscPlaylist(entries))            // PlayStation multi-disc: the emulator swaps discs itself
    {
        srLog(QStringLiteral("m3u: %1-disc playlist -> emulator").arg(entries.size()));
        emit openDisc(src, title);
        return;
    }
    // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
    srLog(QStringLiteral("m3u: %1 entries -> queue").arg(entries.size()));
    QStringList urls, titles, groups, logos;
    for (const M3uEntry& e : entries) { urls << e.url; titles << e.title; groups << e.group; logos << e.logo; }
    emit playQueue(urls, titles, groups, logos, src, title, entryHeaders(entries, src, headers));
}
