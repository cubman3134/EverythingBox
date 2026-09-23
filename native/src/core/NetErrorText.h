// WHAT A FAILED NETWORK REQUEST MAY SAY ON SCREEN, AND WHAT IT MAY SAY IN A LOG (issue #435).
// Pure, header-only; QtCore + QtNetwork.
//
// **QNetworkReply::errorString() is not a message, it is a copy of the request.** For an HTTP error Qt
// renders "Error transferring <url> - server replied: <reason>", with the url whole: its query and all. A
// Jellyfin url carries `api_key` there, a Subsonic one `u`/`t`/`s`, an Audiobookshelf one its token, a debrid
// stream link its signature. The Downloads panel used to show that string as a job's reason for failing, so a
// 404 on a server download put the user's credential on screen — and so into a screenshot, a screen share and
// a bug report.
//
// The rule this file enforces is therefore about the SOURCE of a message, not about spotting a secret in one:
//
//   * sentence() is the ONLY thing a user is shown about a failed request. It is built from the NetworkError
//     ENUM, the HTTP status and one flag, through a fixed table of our own sentences. Nothing it returns is
//     derived from the request, so there is nothing in it to leak, whatever the next provider decides to call
//     its token parameter. Its fallback names no url either.
//   * logText() is for a LOG LINE only. It keeps Qt's text, because Qt's text is the useful thing in a bug
//     report, but every url in it goes through LogSafeText::url() — the rule `logSafeUrl` has always applied
//     — first by exact match against the reply's own url (which catches a url Qt printed with a decoded space
//     in it, the case a prose scan alone would cut short), then by LogSafeText::scrub() for anything else. If
//     a query from either url could still be found afterwards, the text is withheld rather than trusted.
//
// Several clients (Jellyfin, Subsonic, Audiobookshelf, Jellyseerr) got here first with their own tables,
// worded for their own sign-in screens, and keep them. This is the table for everything else.
#pragma once
#include <QList>
#include <QStringList>
#include <algorithm>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>
#include "LogSafeText.h"

namespace NetErrorText
{
// The fixed table. Lower-case sentence fragments, because every caller embeds them: the Downloads panel as
// "Failed — <this>", the rest as "Couldn't download X: <this>" or "Couldn't reach Trakt (<this>)."
//
// A sign-in flow passes httpStatus 0, deliberately, so its message comes from the code alone: 401 arrives as
// AuthenticationRequiredError and 403 as ContentAccessDenied ("access was denied" — the wrong password), which
// stay distinct from ConnectionRefused / HostNotFound ("can't reach it").
inline QString sentence(QNetworkReply::NetworkError err, int httpStatus = 0, bool redirectRefused = false)
{
    // The two sentences DownloadManager already had, kept word for word. A refused cross-origin hop reaches
    // here as OperationCanceledError, which would otherwise read as the user having cancelled it.
    if (redirectRefused)
        return QObject::tr("this source sent the download on to a different site, and the HTTP headers it needs "
                           "are not sent there");
    if (httpStatus >= 400) return QObject::tr("the source returned HTTP %1").arg(httpStatus);

    switch (err)
    {
        case QNetworkReply::ConnectionRefusedError:
            return QObject::tr("the connection was refused");
        case QNetworkReply::RemoteHostClosedError:
            return QObject::tr("the connection was closed before the transfer finished");
        case QNetworkReply::HostNotFoundError:
            return QObject::tr("the server could not be found");
        case QNetworkReply::TimeoutError:
            return QObject::tr("the server took too long to answer");
        case QNetworkReply::OperationCanceledError:
            return QObject::tr("cancelled by you");
        case QNetworkReply::SslHandshakeFailedError:
            return QObject::tr("a secure (TLS/SSL) connection could not be established");
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::BackgroundRequestNotAllowedError:
            return QObject::tr("the network connection was lost");
        case QNetworkReply::TooManyRedirectsError:
            return QObject::tr("the server redirected too many times");
        case QNetworkReply::InsecureRedirectError:
            return QObject::tr("the server redirected to an insecure address");
        case QNetworkReply::ProxyConnectionRefusedError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyNotFoundError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::ProxyAuthenticationRequiredError:
        case QNetworkReply::UnknownProxyError:
            return QObject::tr("the proxy failed");
        case QNetworkReply::ContentNotFoundError:
        case QNetworkReply::ContentGoneError:
            return QObject::tr("the content was not found");
        case QNetworkReply::ContentAccessDenied:
        case QNetworkReply::AuthenticationRequiredError:
        case QNetworkReply::ContentOperationNotPermittedError:
            return QObject::tr("access was denied");
        case QNetworkReply::InternalServerError:
        case QNetworkReply::ServiceUnavailableError:
        case QNetworkReply::OperationNotImplementedError:
        case QNetworkReply::UnknownServerError:
            return QObject::tr("the server reported an error");
        default:
            // The fallback. Deliberately generic, and deliberately NOT Qt's text: the codes that land here
            // (protocol and content oddities) are exactly the ones whose Qt string is most likely to be the
            // "Error transferring <url>" form.
            return QObject::tr("an unknown network error");
    }
}

// The reply's HTTP status, or 0 when no response head ever arrived.
inline int httpStatusOf(const QNetworkReply* r)
{
    return r ? r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
}

// sentence() for a finished reply: its code and its status. Reads nothing else off the reply — in particular
// not errorString(), and not the url.
inline QString forReply(const QNetworkReply* r, bool redirectRefused = false)
{
    if (!r) return sentence(QNetworkReply::UnknownNetworkError);
    return sentence(r->error(), httpStatusOf(r), redirectRefused);
}

// Qt's error text for a reply, fit for a LOG LINE and nothing else. See the top of this file.
inline QString logText(const QNetworkReply* r)
{
    if (!r) return QString();
    QString t = r->errorString();

    QList<QUrl> urls{ r->url() };
    if (r->request().url() != r->url()) urls.append(r->request().url());

    // Pass 1: the reply's own urls, in every spelling Qt might have printed them, replaced by a marker that
    // contains no "://" (so pass 2 cannot re-read it as a url). Longest first, so a spelling that is a prefix
    // of another cannot replace half of it.
    QStringList spellings;
    for (const QUrl& u : urls)
        for (const QString& s : { u.toString(), u.toString(QUrl::FullyEncoded), u.toDisplayString(),
                                  u.toString(QUrl::FullyDecoded) })
            if (!s.isEmpty() && !spellings.contains(s)) spellings.append(s);
    std::sort(spellings.begin(), spellings.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    // '<' and '>' end a url run in scrub(), so a marker can never be swallowed into a neighbouring url.
    const auto marker = [](int i) {
        return QStringLiteral("<") + QChar(1) + QString::number(i) + QChar(1) + QStringLiteral(">");
    };
    for (int i = 0; i < spellings.size(); ++i) t.replace(spellings.at(i), marker(i));

    // Pass 2: any OTHER url in the text (a redirect target Qt named, say).
    t = LogSafeText::scrub(t);

    for (int i = 0; i < spellings.size(); ++i) t.replace(marker(i), LogSafeText::url(spellings.at(i)));

    // The last line: if a query from either url is still in there, in any spelling, the text is not trusted.
    for (const QUrl& u : urls)
        for (const QString& q : { u.query(QUrl::FullyEncoded), u.query(QUrl::PrettyDecoded),
                                  u.query(QUrl::FullyDecoded) })
            if (!q.isEmpty() && t.contains(q))
                return QStringLiteral("(error text withheld: it still held the request's query)");
    return t;
}
} // namespace NetErrorText
