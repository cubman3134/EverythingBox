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

    // "user-agent"/"USER_AGENT-ish" -> "User-Agent". Lowercase, then capitalise each dash-separated part.
    // `Referrer` (the two-r spelling addons pick up from mpv's own option name) canonicalises to the HTTP
    // spelling `Referer`, so the two can never both be present and disagree.
    QString canonicalName(const QString& raw);

    // The `request` half of behaviorHints.proxyHeaders. `response` is deliberately dropped: it describes
    // headers the addon expects to see COMING BACK, which is not something a player can act on, and carrying
    // it would only create a second thing to accidentally send.
    //
    // Rejected on the way in, because a stream is untrusted input:
    //   * empty names, and names/values that are not strings;
    //   * CR/LF anywhere in a value — otherwise a single addon field becomes header injection;
    //   * hop-by-hop and request-shaping fields (Host, Content-Length, Connection, Transfer-Encoding,
    //     Upgrade, Range). Range is the sharp one: the player issues its OWN Range for every seek, and a
    //     fixed one from the addon would pin playback to one byte window forever.
    Headers parseProxyHeaders(const QJsonObject& behaviorHints);

    // The headers that may accompany a request to `playUrl`, given that the addon declared them for
    // `declaredUrl`. Empty unless the two are the SAME http(s) origin (scheme + host + port).
    //
    // This is the guard that makes cross-source leakage structurally impossible rather than merely intended.
    // The play path does not always fetch the URL the addon handed us: a torrent candidate is resolved
    // through a debrid service and comes back as a completely different host, and that host must not be sent
    // the original CDN's Referer. Rather than remembering which call sites re-point the URL, every consumer
    // asks this, and a re-pointed URL loses the headers automatically.
    Headers forPlayUrl(const Headers& declared, const QString& declaredUrl, const QString& playUrl);

    // Receives one player property assignment: a property name and the values it should take (an EMPTY list
    // means "clear this property").
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
