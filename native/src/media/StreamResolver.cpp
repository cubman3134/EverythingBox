#include "StreamResolver.h"

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
void StreamResolver::resolve(const QString& src, const QString& title)
{
    if (!src.contains(QStringLiteral("://")))
    {
        QFile f(src);
        QString text;
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) text = QString::fromUtf8(f.readAll());
        classify(src, text, title.isEmpty() ? QFileInfo(src).completeBaseName() : title);
        return;
    }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    emit status(tr("Loading playlist…"));
    srLog(QStringLiteral("m3u: GET %1").arg(logSafeUrl(src)));
    QNetworkRequest rq{ QUrl(src) };
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, src, title] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            // Couldn't fetch the manifest text (auth, headers, live-only) - let libmpv try the URL itself.
            srLog(QStringLiteral("m3u: fetch failed (%1) -> player").arg(reply->errorString()));
            emit playDirect(src, title);
            return;
        }
        classify(src, QString::fromUtf8(reply->readAll()), title);
    });
}

void StreamResolver::classify(const QString& src, const QString& text, const QString& title)
{
    if (isHlsManifest(text))                       // a single adaptive stream: libmpv handles the segments
    {
        srLog(QStringLiteral("m3u: HLS manifest -> player"));
        emit playDirect(src, title);
        return;
    }
    const QVector<M3uEntry> entries = parseM3u(text, src);
    if (entries.isEmpty())                         // not a recognisable list - best effort: play the URL
    {
        srLog(QStringLiteral("m3u: no entries -> player"));
        emit playDirect(src, title);
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
