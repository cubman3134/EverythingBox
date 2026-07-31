#include "StreamHeaders.h"

#include <QJsonValue>
#include <QUrl>

#include <cstring>

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

// RFC 7230 `token` — the complete set of characters a header field-name may contain: ALPHA / DIGIT /
// "!#$%&'*+-.^_`|~". A name is either entirely made of these or refused; nothing is stripped.
//
// This is the NAME half of the injection guard, and it is the sharper half. A value carrying CRLF is
// refused below, but a NAME carrying it used to sail through: QString::trimmed() removes only LEADING and
// TRAILING whitespace, so `X-A\r\nRange: bytes=0-1` kept its embedded CRLF, was not in blocked() (the
// blocklist sees the whole mangled string, not the `Range` hiding inside it), and reached applyTo, which
// emits `name + ": " + value` verbatim into mpv's field list. libmpv/ffmpeg does no sanitising of its own —
// verified on the wire — so the bytes go onto the socket as written: that smuggles both the blocked fields
// the list exists to stop AND, with a doubled CRLF, a whole second attacker-chosen request pipelined onto
// the connection. Rejecting outright (rather than deleting the offending characters) is deliberate: a
// stripping rule has to be right about every character it rewrites, while this one only has to be right
// about which characters are legal.
//
// Deliberately NOT relying on QHttpHeaders refusing it on the StreamResolver leg: the mpv leg has no such
// backstop, and a guard that only one of two consumers enforces is not a guard.
bool isToken(const QString& name)
{
    if (name.isEmpty()) return false;
    static const char kPunct[] = "!#$%&'*+-.^_`|~";
    for (const QChar ch : name)
    {
        const char16_t c = ch.unicode();
        // ASCII ranges spelled out rather than QChar::isLetterOrNumber(), which also accepts non-ASCII
        // letters and digits — those are not `token` characters and have no business in a field-name.
        if ((c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) continue;
        // c != 0 guards strchr's documented match on the terminating NUL — a name containing U+0000 would
        // otherwise be accepted, and a NUL is exactly the sort of byte a smuggling attempt is made of.
        if (c != 0 && c < 0x80 && std::strchr(kPunct, char(c)) != nullptr) continue;
        return false;
    }
    return true;
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
    // Charset FIRST, before any folding or title-casing: everything below rewrites the string, and a rule
    // that validates a rewritten name is a rule about the rewriter, not about what the addon sent. An
    // illegal name is not repaired into a legal one — it is refused, and the caller drops the field.
    if (!isToken(t)) return QString();
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
        // ONE rule refuses both an empty value and a non-string one: QJsonValue::toString() is documented to
        // return a null QString for every type that is not a string, so a number/array/object arrives here
        // as "" and is dropped by the same line. An extra `isString()` guard above would read as defence in
        // depth while being unkillable — removing it changes no behaviour, so no test could pin it, and the
        // assertion that non-strings are refused would have quietly become inert. (Found by mutating it.)
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
