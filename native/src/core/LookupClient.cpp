#include "LookupClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

// Twelve seconds. A dictionary lookup is a thing you wait for with your finger still on the pad, so the
// budget is much shorter than a download's — and it is what turns "the endpoint accepted the socket and then
// said nothing" into the same readable sentence being offline produces.
static int g_timeoutMs = 12000;

void LookupClient::setTimeoutMs(int ms) { g_timeoutMs = (ms > 0) ? ms : 12000; }
int  LookupClient::timeoutMs()          { return g_timeoutMs; }

LookupClient::LookupClient(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this))
{
}

LookupClient::~LookupClient() { cancel(); }

bool LookupClient::busy() const { return !reply_.isNull(); }

void LookupClient::cancel()
{
    // THE DISCONNECT IS WHAT DROPS THE ANSWER, and it has to come before the abort: abort() makes the reply
    // emit finished() synchronously, and a handler still attached at that moment would deliver a cancelled
    // lookup's answer onto whatever the reader is looking at now. (Probing this: removing the cb_ clear alone
    // leaves the probe GREEN, because the disconnect already stops finish() from running at all; removing the
    // disconnect is what turns it red. The cb_ clear stays as the belt to that brace — it also releases
    // whatever the callback captured, which for the reader is a card and a book anchor.)
    if (QNetworkReply* r = reply_.data())
    {
        reply_.clear();
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    cb_ = Callback();
}

void LookupClient::start(LookupRequest::Verb verb, const QString& term, const QString& srcLang,
                         const QString& dstLang, const QString& endpoint, Callback cb)
{
    cancel();   // one lookup at a time: the previous question is no longer the one being asked

    verb_ = verb;
    term_ = term;
    lang_ = srcLang;

    QNetworkRequest req;
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(LookupRequest::userAgent()));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);   // Wikimedia redirects a title to its canonical form
    req.setTransferTimeout(g_timeoutMs);

    QNetworkReply* r = nullptr;
    if (verb == LookupRequest::Verb::Translate)
    {
        const QUrl u = LookupRequest::translateUrl(endpoint);
        if (!u.isValid())
        {
            // Should not be reachable — the verb is not offered without a configured instance — but a caller
            // that gets here still gets an Outcome rather than a card that never resolves.
            LookupRequest::Outcome o;
            o.text = QStringLiteral("No translation service is set up. Add one in Settings ▸ Reading.");
            if (cb) cb(o);
            return;
        }
        req.setUrl(u);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        r = nam_->post(req, LookupRequest::translateBody(term, srcLang, dstLang));
    }
    else
    {
        req.setUrl(verb == LookupRequest::Verb::Wikipedia ? LookupRequest::summaryUrl(term, srcLang)
                                                          : LookupRequest::defineUrl(term, srcLang));
        r = nam_->get(req);
    }

    cb_ = std::move(cb);
    reply_ = r;
    connect(r, &QNetworkReply::finished, this, [this, r] { finish(r); });
}

void LookupClient::finish(QNetworkReply* reply)
{
    if (!reply) return;
    if (reply_.data() != reply) { reply->deleteLater(); return; }   // a cancelled reply's late signal

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    // A transport failure is one where no HTTP status came back at all: refused, DNS, TLS, timeout. A 404 is
    // NOT one of these — the server answered, and "no entry for this word" is a different sentence from "we
    // could not reach the dictionary".
    const bool transportFailed = (status == 0) && (reply->error() != QNetworkReply::NoError);

    reply_.clear();
    reply->deleteLater();

    LookupRequest::Outcome out;
    switch (verb_)
    {
    case LookupRequest::Verb::Wikipedia:
        out = LookupRequest::readSummary(body, status, transportFailed, term_);
        break;
    case LookupRequest::Verb::Translate:
        out = LookupRequest::readTranslation(body, status, transportFailed);
        break;
    case LookupRequest::Verb::Define:
        out = LookupRequest::readDefinition(body, status, transportFailed, term_, lang_);
        break;
    }

    // Take the callback before running it: a callback that starts the NEXT lookup would otherwise be cleared
    // by this line after having installed its own.
    Callback cb;
    cb.swap(cb_);
    if (cb) cb(out);
}
