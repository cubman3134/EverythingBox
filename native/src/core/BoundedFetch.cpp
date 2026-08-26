#include "BoundedFetch.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>

namespace BoundedFetch
{

Result get(const QString& url, int timeoutMs, qint64 ceilingBytes)
{
    Result r;

    QNetworkAccessManager nam;
    QNetworkRequest rq{ QUrl(url) };
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QScopedPointer<QNetworkReply> reply(nam.get(rq));

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);

    // WE aborted, so the OperationCanceledError this produces is an answer and not a failure. The same
    // distinction DownloadManager has to draw with redirectRefused_, and for the same reason: Qt reports a
    // deliberate abort with the identical error the user's own Cancel produces.
    bool overCeiling = false;
    bool headRead = false;

    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply.data(), &QNetworkReply::readyRead, reply.data(), [&] {
        if (overCeiling) return;                       // already abandoned; drain nothing further
        const QByteArray chunk = reply->readAll();
        r.read += chunk.size();

        // Read the declaration ONCE, and here rather than in metaDataChanged. metaDataChanged fires once per
        // redirect hop, so a 3xx head can be mistaken for the real one; body bytes only ever arrive on the
        // final response, so by the first readyRead the head being read is the head that describes these
        // bytes. An absent Content-Length leaves `declared` at -1 — toLongLong() on an invalid QVariant is
        // 0, which would read as "the server declared an empty body" and let anything through.
        if (!headRead)
        {
            headRead = true;
            const QVariant cl = reply->header(QNetworkRequest::ContentLengthHeader);
            if (cl.isValid()) r.declared = cl.toLongLong();
        }

        // One predicate for both cases. A declared length answers at byte zero; an undeclared one answers
        // when the bytes themselves cross the line. Nothing about a chunked or connection-delimited response
        // needs a branch of its own.
        if (r.declared > ceilingBytes || r.read > ceilingBytes)
        {
            overCeiling = true;
            r.body.clear();          // hand back nothing we refused, so a caller cannot half-use it
            reply->abort();
            return;
        }
        r.body.append(chunk);
    });

    deadline.start(timeoutMs);
    loop.exec();

    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (overCeiling) { r.verdict = Result::TooBig; return r; }

    if (!reply->isFinished())
    {
        reply->abort();
        r.body.clear();
        r.error = QStringLiteral("deadline");
        r.verdict = Result::Failed;
        return r;
    }
    if (reply->error() != QNetworkReply::NoError)
    {
        r.body.clear();          // a 4xx still has a body, and it is an error page, not the file
        r.error = reply->errorString();
        r.verdict = Result::Failed;
        return r;
    }

    r.verdict = Result::Ok;
    return r;
}

} // namespace BoundedFetch
