#include "JellyfinQuickConnect.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

// ---- Paths and bodies -------------------------------------------------------------------------------------

QString JellyfinQuickConnect::enabledPath()      { return QStringLiteral("/QuickConnect/Enabled"); }
QString JellyfinQuickConnect::initiatePath()     { return QStringLiteral("/QuickConnect/Initiate"); }
QString JellyfinQuickConnect::connectPath()      { return QStringLiteral("/QuickConnect/Connect"); }
QString JellyfinQuickConnect::authenticatePath() { return QStringLiteral("/Users/AuthenticateWithQuickConnect"); }

QString JellyfinQuickConnect::connectQuery(const QString& secret)
{
    // `secret` is the OpenAPI's own parameter name. A Jellyfin secret is hex, but it is percent-encoded all
    // the same: the value is the server's to choose, and a query built by concatenation is how a stray `&`
    // becomes a second parameter.
    return QStringLiteral("secret=") + QString::fromLatin1(QUrl::toPercentEncoding(secret));
}

QByteArray JellyfinQuickConnect::authenticateBody(const QString& secret)
{
    QJsonObject o;
    o.insert(QStringLiteral("Secret"), secret);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// ---- The pure decisions -----------------------------------------------------------------------------------

JellyfinQuickConnect::Route JellyfinQuickConnect::routeFor(bool transportOk, int httpStatus,
                                                           const QByteArray& body)
{
    if (!transportOk || httpStatus != 200) return Route::Password;
    // A BARE JSON LITERAL, which QJsonDocument will not parse at the top level — so it is compared as text.
    // Only the exact literal counts: `"true"` (a string), `1`, or an HTML page that happens to contain the
    // word are all "the server did not say yes".
    return body.trimmed() == "true" ? Route::QuickConnect : Route::Password;
}

JellyfinQuickConnect::Code JellyfinQuickConnect::readInitiate(const QByteArray& body)
{
    Code c;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    c.secret = o.value(QStringLiteral("Secret")).toString();
    c.code   = o.value(QStringLiteral("Code")).toString();
    // BOTH or neither: a secret with no code is a request the user cannot approve, and a code with no
    // secret is one we could never collect.
    c.ok = !c.secret.isEmpty() && !c.code.isEmpty();
    return c;
}

bool JellyfinQuickConnect::initiateRetryAsGet(int httpStatus)
{
    return httpStatus == 404 || httpStatus == 405;
}

JellyfinQuickConnect::State JellyfinQuickConnect::stateFor(bool transportOk, int httpStatus,
                                                           const QByteArray& body)
{
    // NO ANSWER IS NOT AN ANSWER. See the header: a missed poll keeps waiting, and the lifetime bounds it.
    if (!transportOk && httpStatus == 0) return State::Waiting;
    if (httpStatus >= 500)               return State::Waiting;
    if (httpStatus == 404)               return State::Expired;
    if (httpStatus != 200)               return State::Error;

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return State::Error;
    const QJsonValue a = doc.object().value(QStringLiteral("Authenticated"));
    // `Authenticated` MUST BE PRESENT AND A BOOLEAN. A 200 that does not carry it is not a QuickConnectResult
    // — a captive portal, a reverse proxy's landing page — and reading its absence as "not yet" would poll a
    // stranger's web page for five minutes.
    if (!a.isBool()) return State::Error;
    return a.toBool() ? State::Authorized : State::Waiting;
}

bool JellyfinQuickConnect::timedOut(qint64 elapsedMs, qint64 lifetimeMs)
{
    return elapsedMs >= lifetimeMs;
}

namespace {

int statusOf(QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// A per-request deadline. abort() finishes the reply with OperationCanceledError, so every request has one
// completion path however it ends.
void armDeadline(QNetworkReply* reply, int budgetMs)
{
    auto* t = new QTimer(reply);
    t->setSingleShot(true);
    QObject::connect(t, &QTimer::timeout, reply, [reply] { reply->abort(); });
    t->start(budgetMs > 0 ? budgetMs : JellyfinQuickConnect::kRequestBudgetMs);
}

} // namespace

void JellyfinQuickConnect::fetchRoute(QNetworkAccessManager* nam, const QString& root,
                                      const Decorate& decorate, int budgetMs, QObject* context,
                                      std::function<void(Route)> done)
{
    QPointer<QObject> guard(context);
    if (!nam || root.isEmpty())
    {
        if (done && guard) done(Route::Password);
        return;
    }
    QNetworkRequest req{ QUrl(root + enabledPath()) };
    if (decorate) decorate(req);
    QNetworkReply* reply = nam->get(req);
    armDeadline(reply, budgetMs);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, guard, done] {
        reply->deleteLater();
        if (!guard || !done) return;
        const bool ok = reply->error() == QNetworkReply::NoError;
        done(routeFor(ok, statusOf(reply), ok ? reply->readAll() : QByteArray()));
    });
}

// ---- The session ----------------------------------------------------------------------------------------

JellyfinQuickConnectSession::JellyfinQuickConnectSession(QNetworkAccessManager* nam, const QString& root,
                                                         Decorate decorate, QObject* parent)
    : QObject(parent), nam_(nam), root_(root), decorate_(std::move(decorate))
{
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &JellyfinQuickConnectSession::poll);
}

JellyfinQuickConnectSession::~JellyfinQuickConnectSession()
{
    finish();
}

QNetworkReply* JellyfinQuickConnectSession::track(QNetworkReply* reply)
{
    inFlight_ = reply;
    armDeadline(reply, JellyfinQuickConnect::kRequestBudgetMs);
    return reply;
}

void JellyfinQuickConnectSession::start()
{
    finish();                          // a restart is a fresh attempt: a new secret, a new clock
    if (!nam_ || root_.isEmpty())
    {
        emit failed(tr("Quick Connect could not be started on that server."));
        return;
    }
    active_ = true;
    polls_  = 0;
    initiate(/*asGet*/ false);
}

void JellyfinQuickConnectSession::cancel()
{
    finish();
}

void JellyfinQuickConnectSession::finish()
{
    active_ = false;
    secret_.clear();                   // the credential half does not outlive the attempt
    if (timer_) timer_->stop();
    if (QNetworkReply* r = inFlight_.data())
    {
        inFlight_.clear();
        // Disconnect FIRST, so the abort's own finished() reaches no handler of ours — an aborted request's
        // answer is ignored by construction, not by a flag somebody has to remember to check.
        QObject::disconnect(r, nullptr, this, nullptr);
        r->abort();
        r->deleteLater();
    }
}

void JellyfinQuickConnectSession::initiate(bool asGet)
{
    QNetworkRequest req{ QUrl(root_ + JellyfinQuickConnect::initiatePath()) };
    if (decorate_) decorate_(req);
    QNetworkReply* reply = track(asGet ? nam_->get(req) : nam_->post(req, QByteArray()));
    connect(reply, &QNetworkReply::finished, this, [this, reply, asGet] {
        reply->deleteLater();
        if (inFlight_ == reply) inFlight_.clear();
        if (!active_) return;
        const int status = statusOf(reply);
        if (!asGet && JellyfinQuickConnect::initiateRetryAsGet(status))
        {
            initiate(/*asGet*/ true);   // 10.8 spells it GET
            return;
        }
        const JellyfinQuickConnect::Code c = reply->error() == QNetworkReply::NoError
            ? JellyfinQuickConnect::readInitiate(reply->readAll()) : JellyfinQuickConnect::Code{};
        if (!c.ok)
        {
            finish();
            emit failed(status == 401 ? tr("Quick Connect is switched off on that server.")
                                      : tr("Quick Connect could not be started on that server."));
            return;
        }
        secret_ = c.secret;
        clock_.start();
        emit codeReady(c.code);
        if (active_) schedulePoll();
    });
}

void JellyfinQuickConnectSession::schedulePoll()
{
    // ONE REQUEST AT A TIME, and the interval runs from the previous ANSWER — a slow server is polled more
    // slowly rather than having a queue of Connect calls pile up behind it.
    timer_->start(pollMs_);
}

void JellyfinQuickConnectSession::poll()
{
    if (!active_) return;
    if (JellyfinQuickConnect::timedOut(clock_.elapsed(), lifetimeMs_))
    {
        finish();
        emit expired();
        return;
    }
    QUrl u(root_ + JellyfinQuickConnect::connectPath());
    u.setQuery(JellyfinQuickConnect::connectQuery(secret_), QUrl::StrictMode);
    QNetworkRequest req{ u };
    if (decorate_) decorate_(req);
    ++polls_;
    QNetworkReply* reply = track(nam_->get(req));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (inFlight_ == reply) inFlight_.clear();
        if (!active_) return;
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int status = statusOf(reply);
        switch (JellyfinQuickConnect::stateFor(ok, status, ok ? reply->readAll() : QByteArray()))
        {
        case JellyfinQuickConnect::State::Authorized:
            exchange();
            return;
        case JellyfinQuickConnect::State::Waiting:
            schedulePoll();
            return;
        case JellyfinQuickConnect::State::Expired:
            finish();
            emit expired();
            return;
        case JellyfinQuickConnect::State::Error:
            finish();
            emit failed(status == 401 || status == 403
                            ? tr("Quick Connect was switched off on that server.")
                            : tr("That server stopped answering Quick Connect."));
            return;
        }
    });
}

void JellyfinQuickConnectSession::exchange()
{
    QNetworkRequest req{ QUrl(root_ + JellyfinQuickConnect::authenticatePath()) };
    if (decorate_) decorate_(req);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = track(nam_->post(req, JellyfinQuickConnect::authenticateBody(secret_)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (inFlight_ == reply) inFlight_.clear();
        if (!active_) return;
        const Jellyfin::AuthResult r = reply->error() == QNetworkReply::NoError
            ? Jellyfin::readAuthResult(reply->readAll()) : Jellyfin::AuthResult{};
        finish();
        if (r.ok) emit authorized(r);
        else      emit failed(tr("That server approved the code but refused the sign-in."));
    });
}
