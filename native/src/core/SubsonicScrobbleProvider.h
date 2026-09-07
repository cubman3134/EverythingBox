// A SUBSONIC SERVER AS A SCROBBLE DESTINATION (issue #193, increment 6) — the third implementation of
// #192's seam, and the first one that is not a public service.
//
// The issue asks for two things that sound like one: report plays back to the server, and coordinate with
// the Last.fm/ListenBrainz reporting so nothing is counted twice. This file is the first half; Scrobble.h
// owns the second, and the whole of what this file adds to that coordination is answering `ownsSource()`
// honestly. There is no second threshold here, no second queue and no second retry rule — Scrobble.h decides
// what a play is, ScrobbleQueue.h decides what is kept, and the orchestrator decides when to try again, for
// this destination exactly as for the other two.
//
// ==================================================================================================
// ONE PROVIDER PER SERVER, AND THAT IS NOT A DETAIL
// ==================================================================================================
// `id()` is "subsonic:" + the server's uuid, so two configured servers are two destinations with two queues,
// two delivered counters, two backoffs and two last-errors. Three consequences, and each of them is a bug
// avoided rather than a nicety:
//
//   * A BATCH IS HOMOGENEOUS. ScrobbleQueue hands over the oldest N and the orchestrator drops exactly the
//     rows the destination accepted. One provider covering several servers would be handed a batch spanning
//     them, and any mixed outcome — one server up, one asleep — would have to answer "accepted" or "keep"
//     for ALL of them: keep, and the plays the first server already took are sent to it again; accept, and
//     the plays the second server never saw are dropped. With one queue per server the question cannot arise.
//   * ONE SERVER BEING DOWN DOES NOT HOLD UP THE OTHER, which is exactly why Scrobbler's backoff is per
//     provider rather than shared (see Scrobbler.h). A music server on a home LAN is asleep far more often
//     than a public service is down, so this is the ordinary case rather than the exotic one.
//   * THE STATUS LINE NAMES THE SERVER. "Scrobbled 412 tracks to Navidrome" answers the question the
//     confidence indicator exists for; "…to your music servers" would not.
//
// A server the user has removed makes its provider `configured() == false` — the store no longer answers for
// that id — so it stops pumping, stops being offered listens, and stops appearing in the status line,
// without anything having to hunt it down and delete it.
//
// ==================================================================================================
// WHAT THE THREE VERBS BECOME
// ==================================================================================================
//   nowPlaying   scrobble.view?id=…&submission=false. Fire and forget, no callback, never queued and never
//                retried — the seam's shape forbids it and the reason is in ScrobbleProvider.h.
//   submit       scrobble.view with repeated id/time pairs and submission=true. The whole batch in one
//                request, which the spec allows and which is what the offline queue is shaped around.
//   love         star.view / unstar.view. This is the favourite verb the app already has, not a second one:
//                FavoritesStore's love hook is the only caller, so a star reaches the server from every one
//                of the five surfaces that can star something, or from none of them.
//
// A LOVE HERE IS A LIBRARY EDIT, NOT A BROADCAST (`loveIsLibraryEdit()`), and Scrobble::loveVerdictFor says
// why that matters: starring a track on the server that is already holding it is the same act the button
// visibly performed, so it must not be gated on the listening-history switch or on the double-count
// coordination. A user who has never wanted a scrobbling account still expects their own server to remember
// what they starred.
//
// ==================================================================================================
// THE CREDENTIAL RULE IS THE SAME ONE, AND IT BITES HERE TOO
// ==================================================================================================
// Every request below carries `u`, `t` and `s` in its QUERY STRING, so `QNetworkReply::errorString()` — whose
// text embeds the url — is a credential. There is no call to it in this file for exactly the reason
// SubsonicClient.h sets out at length; transport failures are rendered from the NetworkError enum into fixed
// sentences, and every other message is the server's own `message` field out of the failure envelope.
// ScrobbleResult::message goes straight into ScrobbleQueue's persisted `lastError`, so a message built from a
// url here would write the token into the ini and into every settings screenshot after it.
#pragma once
#include "ScrobbleProvider.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

class QNetworkAccessManager;

class SubsonicScrobbleProvider : public QObject, public ScrobbleProvider
{
    Q_OBJECT
public:
    // `serverId` is SubsonicServer::id — the uuid every qualified id from that server carries. Held rather
    // than the server itself because the store is the source of truth: a url or a password changed in
    // Settings must take effect on the next request, not on the next launch.
    explicit SubsonicScrobbleProvider(const QString& serverId, QObject* parent = nullptr);

    // The prefix every one of these providers' ids starts with, so the one place that installs them and the
    // one place that looks for an existing one cannot spell it differently.
    static QString idPrefix();
    static QString idFor(const QString& serverId);

    QString id() const override;
    QString displayName() const override;
    bool    configured() const override;

    // TRUE only for a play of a track this very server served. See ScrobbleProvider::accepts — a play of a
    // local file cannot be described to a Subsonic server at all, and queueing one here would jam the queue.
    bool accepts(const Scrobble::Track& track) const override;
    // ...and the same predicate answering the coordination question, which is a different question with the
    // same answer for this destination: a play this server served, reported back to it, is the FIRST count
    // of that play rather than a second one. Scrobble::verdictForDestination is where it is used.
    bool ownsSource(const Scrobble::Track& track) const override;

    bool supportsLove() const override { return true; }
    bool loveIsLibraryEdit() const override { return true; }

    void nowPlaying(const Scrobble::Track& track) override;
    void submit(const QVector<Scrobble::Play>& plays, std::function<void(ScrobbleResult)> cb) override;
    void love(const Scrobble::Track& track, bool loved, std::function<void(ScrobbleResult)> cb) override;

private:
    // One request against this provider's server. `cb` is optional: nowPlaying passes none, which is what
    // "fire and forget" means here — the reply is read for nothing and discarded.
    void call(const QString& method, const QList<QPair<QString, QString>>& extra,
              std::function<void(ScrobbleResult)> cb);

    QString                serverId_;
    QNetworkAccessManager* nam_ = nullptr;
};
