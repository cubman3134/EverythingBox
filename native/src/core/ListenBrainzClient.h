// LISTENBRAINZ (issue #192, increment 1) — the first implementation of the ScrobbleProvider seam.
//
// WHY THIS SERVICE FIRST. It needs ONE user token, pasted once. No OAuth dance, no app registration, no
// distributed application key — so the whole feature (the threshold, the offline queue, the settings surface,
// the counter, the favourite hook) gets built and driven end to end without waiting on a credential that has
// to be shipped with the app. Last.fm, which does need one, follows in increment 2 behind the same seam.
//
// AND ITS CUSTOM API URL COVERS MORE THAN LISTENBRAINZ. Maloja and several other self-hosted trackers
// implement the same submit-listens endpoint, so "point this at your own server" is one setting rather than a
// second provider — which is why the URL is a plain setting here and not a per-deployment build flag.
//
// THE THREE CALLS, and how each maps onto the protocol:
//
//   nowPlaying  POST <root>/1/submit-listens   {"listen_type":"playing_now", payload:[{track_metadata}]}
//               No listened_at — the field is FORBIDDEN for playing_now, and sending one is a 400. Fired and
//               forgotten: the reply is not read, nothing is queued, nothing is retried. See ScrobbleProvider.h.
//   submit      POST <root>/1/submit-listens   {"listen_type":"import", payload:[{listened_at, track_metadata}…]}
//               `import` rather than `single` because this is a BATCH — the queue's whole point — and because
//               `single` rejects a payload of more than one listen. A batch of exactly one is still a valid
//               import, so there is no second shape to maintain.
//   love        GET  <root>/1/metadata/lookup?artist_name=…&recording_name=…   -> recording_mbid
//               POST <root>/1/feedback/recording-feedback  {"recording_mbid":…,"score":1|0}
//               Two calls because ListenBrainz's feedback is keyed on a MUSICBRAINZ RECORDING, not on an
//               artist/title pair. A local file's tags carry no MBID (this app's tag reader does not parse
//               one), so the lookup is what turns "the track that is playing" into something the service can
//               file feedback against. When the lookup finds nothing the love is REPORTED as unresolvable
//               rather than dropped silently — a favourite that quietly does not reach the service is the
//               exact complaint the confidence indicator exists to answer.
//
// ==================================================================================================
// THE TOKEN
// ==================================================================================================
// It is the user's own secret and this file is the only place in the app that reads it. Three rules, all
// enforced here rather than trusted to callers:
//
//   1. It is read straight out of the device-local settings carve-out at the moment a request is built, and is
//      never copied into a member, a log, a signal payload or an error string.
//   2. NOTHING logs the request. That is not the same as "nothing logs the token": a diagnostic that prints
//      "the request that failed" prints the Authorization header inside it, and that is how credentials end up
//      in a support log. Every failure message this file produces is built by one function (failureMessage)
//      from the SERVICE's own words and the transport error, and it is the only thing that ever reaches
//      ScrobbleResult::message.
//   3. It is never sent anywhere but the configured API root. A root that does not parse, or that is not
//      http(s), configures the provider OFF rather than falling back to the public service — falling back
//      would send a self-hoster's token to a server they did not choose.
#pragma once
#include "ScrobbleProvider.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;

class ListenBrainzClient : public QObject, public ScrobbleProvider
{
    Q_OBJECT
public:
    explicit ListenBrainzClient(QObject* parent = nullptr);

    // The public service. Used when the custom API URL setting is empty, which is the ordinary case.
    static QString defaultApiRoot();

    // The root actually in force: the trimmed custom URL when it is a usable http(s) URL, else the default.
    // Trailing slashes are removed here so every request below can concatenate without thinking about it.
    static QString apiRoot();

    // ---- ScrobbleProvider ----
    QString id() const override          { return QStringLiteral("listenbrainz"); }
    QString displayName() const override { return QStringLiteral("ListenBrainz"); }
    bool    configured() const override;
    void    nowPlaying(const Scrobble::Track& track) override;
    void    submit(const QVector<Scrobble::Play>& plays,
                   std::function<void(ScrobbleResult)> cb) override;
    bool    supportsLove() const override { return true; }
    void    love(const Scrobble::Track& track, bool loved,
                 std::function<void(ScrobbleResult)> cb) override;

private:
    QNetworkAccessManager* nam_ = nullptr;
};
