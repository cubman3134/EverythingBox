#include "StreamHeaders.h"

#include <QJsonValue>
#include <QUrl>

namespace
{

// Fields we refuse to take from an addon. Lowercase, matched against the canonical name lowercased.
// Host/Content-Length/Connection/Transfer-Encoding/Upgrade shape the connection itself and belong to the
// HTTP client. Range belongs to the player: it re-issues one per seek, and a pinned value would freeze
// playback inside a single byte window.
bool blocked(const QString& lowerName)
{
    static const QStringList kBlocked = {
        QStringLiteral("host"), QStringLiteral("content-length"), QStringLiteral("connection"),
        QStringLiteral("transfer-encoding"), QStringLiteral("upgrade"), QStringLiteral("range"),
    };
    return kBlocked.contains(lowerName);
}

// scheme://host:port, lowercased, with the default port made explicit so "http://h" and "http://h:80"
// compare equal. Empty for anything that is not http(s) or has no host — an origin we cannot compare is
// never treated as matching.
QString origin(const QString& url)
{
    const QUrl u(url);
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return QString();
    const QString host = u.host().toLower();
    if (host.isEmpty()) return QString();
    const int port = u.port(scheme == QLatin1String("https") ? 443 : 80);
    return scheme + QLatin1String("://") + host + QLatin1Char(':') + QString::number(port);
}

} // namespace

QString StreamHeaders::canonicalName(const QString& raw)
{
    const QString t = raw.trimmed().toLower();
    if (t.isEmpty()) return QString();
    // The HTTP header is spelled `Referer`; mpv's option (and therefore some addons) spells it `referrer`.
    // Fold to one so a stream declaring both cannot end up with two disagreeing values.
    if (t == QLatin1String("referrer")) return QStringLiteral("Referer");

    QString out = t;
    bool upper = true;
    for (int i = 0; i < out.size(); ++i)
    {
        if (upper) out[i] = out.at(i).toUpper();
        upper = (out.at(i) == QLatin1Char('-'));
    }
    return out;
}

StreamHeaders::Headers StreamHeaders::parseProxyHeaders(const QJsonObject& behaviorHints)
{
    Headers out;
    const QJsonObject req = behaviorHints.value(QStringLiteral("proxyHeaders"))
                                .toObject()
                                .value(QStringLiteral("request"))
                                .toObject();
    for (auto it = req.begin(); it != req.end(); ++it)
    {
        const QString name = canonicalName(it.key());
        if (name.isEmpty() || blocked(name.toLower())) continue;
        if (!it.value().isString()) continue;   // objects/arrays/numbers are not a header value
        const QString value = it.value().toString().trimmed();
        if (value.isEmpty()) continue;
        // Header injection: one field carrying a newline would otherwise append arbitrary fields (or a body)
        // to the request we build from it.
        if (value.contains(QLatin1Char('\r')) || value.contains(QLatin1Char('\n'))) continue;
        out.insert(name, value);
    }
    return out;
}

StreamHeaders::Headers StreamHeaders::forPlayUrl(const Headers& declared, const QString& declaredUrl,
                                                 const QString& playUrl)
{
    if (declared.isEmpty()) return {};
    const QString a = origin(declaredUrl);
    if (a.isEmpty()) return {};
    if (a != origin(playUrl)) return {};
    return declared;
}

void StreamHeaders::applyTo(const Headers& h, const Sink& sink)
{
    if (!sink) return;

    // Dedicated properties first. Taking these OUT of the field list is what stops mpv sending each twice.
    const QString ua  = h.value(QStringLiteral("User-Agent"));
    const QString ref = h.value(QStringLiteral("Referer"));
    sink(QStringLiteral("user-agent"), ua.isEmpty()  ? QStringList() : QStringList{ ua });
    sink(QStringLiteral("referrer"),   ref.isEmpty() ? QStringList() : QStringList{ ref });

    QStringList fields;
    for (auto it = h.begin(); it != h.end(); ++it)
    {
        if (it.key() == QLatin1String("User-Agent") || it.key() == QLatin1String("Referer")) continue;
        fields << it.key() + QLatin1String(": ") + it.value();
    }
    sink(QStringLiteral("http-header-fields"), fields);
}

QString StreamHeaders::logSummary(const Headers& h)
{
    if (h.isEmpty()) return QString();
    // NAMES ONLY. Never a value: a proxyHeader is routinely an Authorization/Cookie/signed-URL token, and
    // this string lands in stream_debug.log.
    return QStringLiteral("%1 header(s): %2").arg(h.size()).arg(QStringList(h.keys()).join(QLatin1String(", ")));
}

StreamHeaders::ExternalRoute StreamHeaders::externalRoute(const Headers& h)
{
    return h.isEmpty() ? ExternalRoute::HandOff : ExternalRoute::FallBackToBuiltin;
}
