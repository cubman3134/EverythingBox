#include "StreamResolver.h"
#include "../core/AppBrand.h"

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

// Parse #EXTINF titles + entry URLs, resolving relative entries against the playlist's own location.
QVector<M3uEntry> StreamResolver::parseM3u(const QString& text, const QString& src)
{
    QVector<M3uEntry> out;
    const bool srcIsUrl = src.contains(QStringLiteral("://"));
    const int slash = src.lastIndexOf(QLatin1Char('/'));
    const QString base = srcIsUrl ? (slash >= 0 ? src.left(slash + 1) : src)
                                  : (QFileInfo(src).absolutePath() + QLatin1Char('/'));
    QString title;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("#EXTINF")))
        {
            const int c = line.indexOf(QLatin1Char(','));
            if (c >= 0) title = line.mid(c + 1).trimmed(); // text after the last comma is the display name
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) continue; // any other directive
        QString url;
        if (line.contains(QStringLiteral("://")))        url = line;                              // absolute
        else if (srcIsUrl)                               url = QUrl(base).resolved(QUrl(line)).toString();
        else if (QFileInfo(line).isAbsolute())           url = line;
        else                                             url = base + line;                       // relative to file
        out.push_back({ title.isEmpty() ? QFileInfo(line).fileName() : title, url });
        title.clear();
    }
    return out;
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
    // The source's own proxyHeaders, applied AFTER our default UA so a stream that specifies a User-Agent
    // replaces it rather than being appended to. Without this the manifest fetch is refused by exactly the
    // hosts this feature exists for, and the failure looks like a broken playlist rather than a missing header.
    for (auto it = headers.begin(); it != headers.end(); ++it)
        rq.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    // Redirects, when this request carries a source's own headers. Qt re-sends raw headers on the redirected
    // request, so a gated .m3u8 that 302s to a partner CDN hands host B the Referer declared for host A —
    // which routinely carries a token. That is the exact leak forPlayUrl exists to prevent, arriving by a
    // route forPlayUrl never sees, and it was verified on the wire: origin B received both the Referer and
    // the X-Token declared for A.
    //
    // Unlike the player's own redirect following, this one is interceptable in-process, so it is intercepted.
    // UserVerifiedRedirectPolicy holds the redirected request until we allow it, and we allow it only when
    // the target is the SAME origin the headers were declared for — asked of the same forPlayUrl every other
    // consumer asks, so there is one definition of "same origin" and probe_stremio already pins it. A
    // cross-origin redirect is aborted instead, landing on the fetch-failed path below, which hands the URL
    // to the player exactly as an auth failure does. Nothing is ever re-sent to B with A's headers.
    //
    // Only when there ARE headers: an ordinary playlist keeps the policy it has always had, so this can only
    // change the behaviour of the requests that carry a secret.
    if (headers.isEmpty())
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    else
        rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    QNetworkReply* reply = nam_->get(rq);
    if (!headers.isEmpty())
        connect(reply, &QNetworkReply::redirected, this, [this, reply, src, headers](const QUrl& to) {
            if (!StreamHeaders::forPlayUrl(headers, src, to.toString()).isEmpty())
            {
                srLog(QStringLiteral("m3u: same-origin redirect -> %1, headers still apply")
                          .arg(logSafeUrl(to.toString())));
                emit reply->redirectAllowed();
                return;
            }
            srLog(QStringLiteral("m3u: cross-origin redirect -> %1, refusing to carry this source's headers "
                                 "there -> player").arg(logSafeUrl(to.toString())));
            reply->abort();
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
    QStringList urls, titles;
    for (const M3uEntry& e : entries) { urls << e.url; titles << e.title; }
    emit playQueue(urls, titles, src, title);
}
