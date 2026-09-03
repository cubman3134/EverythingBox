// Per-stream HTTP request headers: the `behaviorHints.proxyHeaders.request` half of the Stremio addon
// protocol, and everything we do with it.
//
// Why this is its own unit rather than a few lines inside the translator and a few more inside MpvWidget:
// the interesting rules here are all HYGIENE rules, and hygiene rules that live inline in a widget are rules
// nothing can test. Direct-HTTP and embed hosts gate their CDNs on a `Referer`/`User-Agent`, so these headers
// are per-source secrets-adjacent state that must reach exactly one request and then be gone. The failure
// mode is silent: a `Referer` from host A that survives into host B's request leaks where the user came from
// and can make B refuse for a reason nobody will guess. So the three decisions that carry that risk —
//   * which headers a stream is even allowed to declare,
//   * whether a given URL may receive them at all (forPlayUrl),
//   * and what is written to the player, INCLUDING the explicit clears (applyTo),
// — are pure functions here, and probe_stremio asserts them.
//
// QtCore only (no libmpv, no widgets, no network) so the probe links it in a few lines.
#pragma once
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>

namespace StreamHeaders
{
    // Header name -> value. QMap (not QHash) so iteration is deterministic: the mpv field list and every
    // log line derived from it are then stable, which is what makes them assertable.
    // Names are stored canonicalised ("User-Agent", "Referer"), so lookups need no case dance.
    using Headers = QMap<QString, QString>;

    // "user-agent"/"USER-AGENT" -> "User-Agent". Lowercase, then capitalise each dash-separated part.
    // `Referrer` (the two-r spelling addons pick up from mpv's own option name) canonicalises to the HTTP
    // spelling `Referer`, so the two can never both be present and disagree.
    //
    // "" for any name that is not an RFC 7230 `token` (ALPHA / DIGIT / "!#$%&'*+-.^_`|~") — a space, a
    // colon, a CR or LF, a control byte, anything non-ASCII. Refused, never repaired: see the comment on
    // isToken in the .cpp for why a name carrying CRLF is a request-smuggling primitive and not a typo.
    QString canonicalName(const QString& raw);

    // The `request` half of behaviorHints.proxyHeaders. `response` is deliberately dropped: it describes
    // headers the addon expects to see COMING BACK, which is not something a player can act on, and carrying
    // it would only create a second thing to accidentally send.
    //
    // Rejected on the way in, because a stream is untrusted input:
    //   * any name that is not an RFC 7230 `token` (see canonicalName) — a space, a colon, a CR/LF. This
    //     is the name half of the injection guard: without it a name like "X-A\r\nRange: bytes=0-1" both
    //     defeats the blocklist below (it is not spelled `range`) and writes two fields where one was
    //     declared. Empty names fall out of the same rule;
    //   * empty values — which is also what a number/array/object value becomes;
    //   * CR/LF anywhere in a value — otherwise a single addon field becomes header injection;
    //   * hop-by-hop and request-shaping fields (Host, Content-Length, Connection, Transfer-Encoding,
    //     Upgrade, Range). Range is the sharp one: the player issues its OWN Range for every seek, and a
    //     fixed one from the addon would pin playback to one byte window forever.
    Headers parseProxyHeaders(const QJsonObject& behaviorHints);

    // The same rules, applied to a FLAT name->value object. #188's `pages` resource declares a page's
    // fetch headers as `"headers": { "Referer": "…" }` — the identical vocabulary, one nesting level up
    // from behaviorHints.proxyHeaders.request — and there must be exactly one implementation of "which
    // headers may a source declare": a second copy is a second place for `Range` or a CRLF to get through.
    // parseProxyHeaders is this function applied to the nested object.
    Headers parseHeaderMap(const QJsonObject& flat);

    // The headers that may accompany a request to `playUrl`, given that the addon declared them for
    // `declaredUrl`. Empty unless the two are the SAME http(s) origin (scheme + host + port).
    //
    // This is the guard that makes cross-source leakage structurally impossible rather than merely intended.
    // The play path does not always fetch the URL the addon handed us: a torrent candidate is resolved
    // through a debrid service and comes back as a completely different host, and that host must not be sent
    // the original CDN's Referer. Rather than remembering which call sites re-point the URL, every consumer
    // asks this, and a re-pointed URL loses the headers automatically.
    Headers forPlayUrl(const Headers& declared, const QString& declaredUrl, const QString& playUrl);

    // THE ONE ROUTE THIS GUARD DOES NOT COVER, and why it is a limitation rather than a bug (#59).
    //
    // forPlayUrl is asked before every request the APP makes, so a URL that changed host loses the headers.
    // It is asked once more, on the redirect itself, for every request the app makes through
    // NetHeaderApply — Qt re-sends raw headers across a 302, and that hop is interceptable in-process, so it
    // is intercepted and a cross-origin one is refused.
    //
    // The player's own transfers are NOT interceptable. Once a URL is handed to libmpv, ffmpeg's HTTP layer
    // follows redirects itself, with the headers we set, and neither the hop nor its result is visible to us.
    // Measured against the libmpv this app links (mpv v0.41.0-769-g2d5dfb343, 2026-07), with two loopback
    // servers where the second was deliberately wide open so that anything arriving there was a leak rather
    // than a rejection:
    //
    //   * a cross-origin 302 IS followed, and the second host received BOTH the Referer and the custom
    //     token declared for the first. The leak is real, not theoretical;
    //   * no option constrains it. `max-redirects`, `http-max-redirects` and `follow-redirects` do not exist
    //     as mpv options (option-info reports them unavailable). Passing ffmpeg's own `max_redirects=0` or
    //     `follow_redirects=0` down through `stream-lavf-o` / `demuxer-lavf-o` is ACCEPTED — mpv does not
    //     validate that key/value list — and changes nothing: the second host was still contacted, still
    //     with the headers. That is the trap worth writing down, because it looks like a fix and reports
    //     success;
    //   * nothing observes it either. After the redirect was followed, `path`, `stream-path`, `filename`,
    //     `stream-open-filename` and `media-title` all still reported the ORIGINAL url, so we cannot even
    //     detect after the fact that a hop happened, let alone which host it went to.
    //
    // Dropping the headers for the mpv leg does not help: host A gated the stream, so a bare request 403s
    // and costs the user playback to protect them from a hop that may not happen.
    //
    // Two things WOULD close it, both design changes rather than fixes, both deliberately not taken here:
    //   1. resolve the redirect ourselves first and hand mpv the final url. We have the network stack and
    //      this guard to judge the result. It costs an extra round trip before every gated play, and it is
    //      unsafe against the one-time / single-use signed urls that some of these gated sources issue —
    //      the pre-flight would spend the url and the player would get a dead one;
    //   2. mpv's stream_cb API (mpv/stream_cb.h, which ships with the libmpv we link): register a custom
    //      protocol and do the I/O ourselves, which gives total control over every request the player makes.
    //      It also means reimplementing a seekable HTTP stream — ranges, reconnects, timeouts — that ffmpeg
    //      already does well, for one property.
    //
    // Until one of those is chosen, the exposure is: a stream whose declaring host redirects to another host
    // hands that host this source's request headers, on the player leg only. Everything the app fetches
    // itself is covered.

    // Receives one player property assignment: a property name and the values it should take (an EMPTY list
    // means "clear this property" — i.e. put it back to the PLAYER'S OWN DEFAULT for that property, which is
    // not always the empty string; mpv's user-agent defaults to "libmpv". See MpvHeaderApply::setProperty).
    using Sink = std::function<void(const QString& property, const QStringList& values)>;

    // Write `h` to a player through `sink`. ALWAYS emits exactly three assignments, in this order:
    //   user-agent, referrer, http-header-fields
    // — even when `h` is empty, in which case all three are emitted EMPTY. That is not a detail: emitting
    // nothing for an absent header is precisely the bug where the previous source's Referer stays live for
    // the next stream. Clearing is not a separate step a caller could forget; it is what applying nothing is.
    //
    // User-Agent and Referer are lifted out of the field list into mpv's dedicated `user-agent`/`referrer`
    // properties (which is where mpv itself looks) so they cannot be sent twice.
    void applyTo(const Headers& h, const Sink& sink);

    // Log-safe rendering: a count and the header NAMES, never a value. Some of these carry tokens, and the
    // stream trace log is a file users paste into issues. "" when there are none.
    QString logSummary(const Headers& h);

    // What to do when playback is routed to an external player and the stream needs headers.
    enum class ExternalRoute
    {
        HandOff,            // no headers involved — the external player gets the URL as always
        FallBackToBuiltin   // headers involved — keep it in-app, and say so
    };
    // We do NOT hand a header-gated URL to an external player. We cannot make the headers follow: VLC would
    // need them on its command line (where they would sit in the process table for any local process to
    // read), MPC-HC has no equivalent at all, and the Android ACTION_VIEW intent has nowhere to put them. The
    // honest outcomes are therefore "refuse and explain" or "play it here and explain"; the second is
    // strictly better for the user, because the built-in player CAN satisfy the gate, so refusing would cost
    // them the stream to protect a preference. The caller is expected to say why it overrode the routing.
    ExternalRoute externalRoute(const Headers& h);
}
