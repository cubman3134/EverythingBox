// THE JELLYSEERR CLIENT (issue #109) — the one object that talks to the configured request service, and
// the only implementation of RequestBackend in this build.
//
// Everything that could be decided without a socket is in Jellyseerr.h (paths, bodies, readers, the status
// mapping) and Requests.h (the id path, the vocabulary, the sentences). What is left here is the part that
// genuinely needs the world: which service, which requests, how long to wait, and what the user is told
// when it does not answer.
//
// ==========================================================================================================
// TWO VERBS, AND THE DIFFERENCE BETWEEN THEM IS THE WHOLE OF THE POLICY
// ==========================================================================================================
//   lookup()  is READ-ONLY and runs on every view. For an item that carries only an IMDB id it is TWO round
//             trips — /search to bridge the id, then the title's own endpoint — and that is deliberate: the
//             bridge is what makes the action work on an addon-catalogue title, which is the whole point of
//             the feature. Both legs share one budget.
//   submit()  MAKES SOMEBODY'S SERVER GO AND FETCH SOMETHING. One POST, no retry, no queue, and no code
//             path in this file reaches it except the one the caller asked for. A transport failure is
//             reported and stops; a resubmission is a second press by a person.
//
// ==========================================================================================================
// NOTHING BUILDS A MESSAGE OUT OF A REQUEST
// ==========================================================================================================
// The rule JellyfinClient.h and SubsonicClient.h state, and it applies here with the same force: the API
// key rides a header on every single call, and QNetworkReply::errorString() embeds the url. There is
// therefore NO call to errorString() in this file. Transport failures are classified from the NetworkError
// ENUM, HTTP failures from the STATUS CODE, and every sentence the user reads comes from
// requests::failureSentence — a fixed table that has never seen a request.
//
// Requests use SameOriginRedirectPolicy for the same reason Subsonic's and Jellyfin's do: a redirect to
// another host would carry the key to a service the user never configured.
#pragma once
#include "RequestBackend.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;

class JellyseerrClient : public QObject, public RequestBackend
{
    Q_OBJECT
public:
    // One per process. Two would be two identities on the wire for one install, and would double every
    // lookup a detail view makes.
    static JellyseerrClient& instance();

    explicit JellyseerrClient(QObject* parent = nullptr);

    QString id() const override          { return QStringLiteral("jellyseerr"); }
    QString displayName() const override { return QStringLiteral("Jellyseerr"); }
    bool    configured() const override;

    void lookup(const requests::MediaRef& ref, int budgetMs,
                std::function<void(const RequestLookup&)> cb) override;
    void submit(const RequestSubmission& sub, int budgetMs,
                std::function<void(const RequestAck&)> cb) override;

    // The settings surface's "does this address and key work at all" check — a GET of /api/v1/status with
    // the key attached, which answers 401 for a bad key and a body for a good one. `error` is empty on
    // success and is one of our own sentences otherwise. It exists so the setup flow can refuse to store a
    // credential that cannot work, rather than storing it and failing later on somebody's detail page.
    using VerifyDone = std::function<void(bool ok, const QString& error)>;
    void verify(const QString& url, const QString& apiKey, bool allowPlainHttp, int budgetMs,
                VerifyDone done);

private:
    // The second leg of a lookup, once a tmdb id is in hand however it was obtained.
    void fetchMediaStatus(const QString& root, const QString& apiKey, const QString& mediaType,
                          const QString& tmdbId, int budgetMs,
                          std::function<void(const RequestLookup&)> cb);

    QNetworkAccessManager* nam();
    QNetworkAccessManager* nam_ = nullptr;
};
