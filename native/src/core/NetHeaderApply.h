// Sending a stream's HTTP headers on one of OUR OWN requests: the QNetworkAccessManager half of what
// MpvHeaderApply does for libmpv. One inline function in its own header for the same reason that one is —
// the EXACT code that puts these bytes on the socket can then be driven by an out-of-tree harness against a
// real server, and a harness that re-implemented it would only prove its own copy works.
//
// The rules it implements live in StreamHeaders (pure, probe-covered); this is only the Qt spelling of them.
//
// Three places in the tree fetch a URL a stream declared headers for — the playlist fetch (StreamResolver),
// the download-for-keeps queue (DownloadManager) and the remote document/ROM pull (MainWindow) — and until
// #59 exactly one of them sent the headers at all. They share this now, because the interesting part is not
// "set some raw headers": it is the REDIRECT, and getting that wrong is silent.
#pragma once
#include "StreamHeaders.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <functional>

namespace NetHeaderApply
{

// The two halves of the rule, in a Detail namespace so they stay UNAVAILABLE to callers: the invariant
// get() states below — that nobody can apply the headers without also gating the redirect — is the whole
// safety property here, and it survives the factoring only because these two are not part of the API.
// They are declared ahead of get() because get() is written in terms of them, and get() stays first,
// where it has always been, because it is what nearly every caller wants.
namespace Detail
{
inline StreamHeaders::Headers applyHeaders(QNetworkRequest& rq,
                                           const StreamHeaders::Headers& declaredHeaders,
                                           const QString& declaredUrl);
inline QNetworkReply* guardRedirects(QNetworkReply* reply, const StreamHeaders::Headers& appliedHeaders,
                                     const QString& declaredUrl,
                                     const std::function<void(bool allowed, const QUrl& to)>& onRedirect);
}

// GET `rq`, carrying whatever of `declaredHeaders` (declared by the stream for `declaredUrl`) this request's
// URL is entitled to. Returns the reply, already guarded.
//
// Everything here is one call on purpose. Applying the headers and gating the redirect are not two steps
// a caller composes: UserVerifiedRedirectPolicy is set exactly when headers were applied, and that policy
// HOLDS a redirected request until something calls redirectAllowed() — so a caller who applied headers and
// forgot to gate would not leak, it would hang, which is a worse bug and one no reviewer spots. There is no
// way to get one without the other from here.
//
// Call it AFTER setting your own default User-Agent on `rq`: a stream that specifies one must REPLACE ours
// rather than be appended to it, and Qt stores UserAgentHeader as the raw "User-Agent" field, so the later
// write wins. Anything else you set first (a resume Range, say) survives — parseProxyHeaders refuses Range
// and the other request-shaping fields, so a stream cannot overwrite them.
//
// `onRedirect(allowed, to)` is a notification hook, not a decision: the decision is forPlayUrl's. It exists
// because each caller logs to a different file and this header must not know about any of them. It is handed
// the redirect TARGET and a verdict — never header data.
inline QNetworkReply* get(QNetworkAccessManager* nam, QNetworkRequest& rq,
                          const StreamHeaders::Headers& declaredHeaders, const QString& declaredUrl,
                          const std::function<void(bool allowed, const QUrl& to)>& onRedirect = {})
{
    if (!nam) return nullptr;
    const StreamHeaders::Headers appliedHeaders = Detail::applyHeaders(rq, declaredHeaders, declaredUrl);
    QNetworkReply* reply = nam->get(rq);
    return Detail::guardRedirects(reply, appliedHeaders, declaredUrl, onRedirect);
}

namespace Detail
{

// The header half, pulled out of get() verbatim so a non-GET request can be given the SAME rules rather
// than a second copy of them. Returns what this url was actually entitled to, which is what the redirect
// gate then measures against.
inline StreamHeaders::Headers applyHeaders(QNetworkRequest& rq,
                                           const StreamHeaders::Headers& declaredHeaders,
                                           const QString& declaredUrl)
{
    // What THIS url may receive, which is not always what the stream declared: a debrid/CDN substitute is a
    // different host, and it gets none of them. Asked here rather than trusted from the caller so that every
    // one of the three fetch sites is scoped by the same rule whether or not its author thought about it.
    const StreamHeaders::Headers appliedHeaders =
        StreamHeaders::forPlayUrl(declaredHeaders, declaredUrl, rq.url().toString());
    for (auto it = appliedHeaders.begin(); it != appliedHeaders.end(); ++it)
        rq.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

    // Redirects, when this request carries a source's own headers. Qt re-sends raw headers on the redirected
    // request, so a gated URL that 302s to a partner CDN hands host B the Referer (routinely a token)
    // declared for host A. That is the exact leak forPlayUrl exists to prevent, arriving by a route
    // forPlayUrl never sees — verified on the wire in #43, where origin B received both the Referer and the
    // X-Token declared for A.
    //
    // So: UserVerifiedRedirectPolicy holds the redirected request until we allow it, and we allow it only
    // when the target is the SAME origin the headers were declared for. A cross-origin hop is aborted, which
    // lands the caller on its own fetch-failed path — the same place an auth failure lands it. Nothing is
    // ever re-sent to B with A's headers.
    //
    // ONLY when there are headers: an ordinary fetch keeps the policy it has always had, so this can change
    // the behaviour of exactly the requests that carry a secret and no others.
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                    appliedHeaders.isEmpty() ? QNetworkRequest::NoLessSafeRedirectPolicy
                                             : QNetworkRequest::UserVerifiedRedirectPolicy);
    return appliedHeaders;
}

// The redirect half, likewise verbatim: hold a redirect that carries someone's headers until the target
// is checked, allow it only back to the origin they were declared for, and abort otherwise.
inline QNetworkReply* guardRedirects(QNetworkReply* reply, const StreamHeaders::Headers& appliedHeaders,
                                     const QString& declaredUrl,
                                     const std::function<void(bool allowed, const QUrl& to)>& onRedirect)
{
    if (!reply || appliedHeaders.isEmpty()) return reply;

    // `reply` is the connection context as well as the sender, so this dies with the request rather than
    // outliving it on some longer-lived receiver.
    QObject::connect(reply, &QNetworkReply::redirected, reply,
                     [reply, appliedHeaders, declaredUrl, onRedirect](const QUrl& to) {
        const bool allowed =
            !StreamHeaders::forPlayUrl(appliedHeaders, declaredUrl, to.toString()).isEmpty();
        if (onRedirect) onRedirect(allowed, to);
        if (allowed) emit reply->redirectAllowed();
        else         reply->abort();
    });
    return reply;
}

} // namespace Detail

// The same two rules for a request that is not a GET. #153 reports a comic's reading position back to the
// server that owns it, which is a PATCH (Komga) or a POST (Kavita) carrying the very same device-local
// Basic auth the feed and page fetches carry — so it needs the same scoping and the same redirect gate,
// and getting them by calling the same two functions is the only way to be sure it has them.
//
// `verb` is the HTTP method; `body` is sent as-is and its content type is the caller's to set on `rq`.
inline QNetworkReply* send(QNetworkAccessManager* nam, QNetworkRequest& rq, const QByteArray& verb,
                           const QByteArray& body, const StreamHeaders::Headers& declaredHeaders,
                           const QString& declaredUrl,
                           const std::function<void(bool allowed, const QUrl& to)>& onRedirect = {})
{
    if (!nam) return nullptr;
    const StreamHeaders::Headers appliedHeaders = Detail::applyHeaders(rq, declaredHeaders, declaredUrl);
    QNetworkReply* reply = nam->sendCustomRequest(rq, verb, body);
    return Detail::guardRedirects(reply, appliedHeaders, declaredUrl, onRedirect);
}

} // namespace NetHeaderApply
