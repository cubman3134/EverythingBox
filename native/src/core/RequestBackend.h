// THE REQUEST SEAM (issue #109) — the whole of what a request service has to be able to do, and nothing
// that is common to all of them.
//
// SHAPED FOR TWO IMPLEMENTATIONS WHILE ONE IS BEING WRITTEN, the way ScrobbleProvider.h and SyncBackend.h
// are. Jellyseerr lands here; EverythingBoxServer#16 grows an equivalent queue (indexers → ranking →
// debrid) and the issue is explicit that it must EXTEND this surface rather than grow a parallel one. The
// way to make that true is not to guess at that server's API — it is to keep off this interface everything
// that is not a per-service act:
//
//   * WHICH IDS AN ITEM HAS and how they are read — requests::refFor. Written once, applied to whichever
//     backend is configured. Note what this buys: the seam speaks a MediaRef carrying BOTH id kinds, so a
//     backend that keys on IMDB (ours will) and one that keys on TMDB (Jellyseerr does) implement the same
//     signature. A seam that took a tmdb id would have had to change shape for the second one.
//   * WHAT THE STATES ARE CALLED and what a failure says — requests::Availability / requests::Failure.
//     A backend translates its own codes and stops; nothing above the seam sees a service's numbers or a
//     service's words.
//   * WHAT IS REMEMBERED. RequestStore holds the profile's own rows; a backend is asked about a title and
//     answers, and never decides what to keep.
//   * WHEN to ask. The surface's — status is fetched ON VIEW, never polled, and a submission happens only
//     under an explicit press. A backend has no timer and no queue.
//
// What is left is two verbs, and they are two because they are two different acts with two different
// consequences:
//
//   lookup   READ-ONLY, and safe to run on every view. It answers "what does this service say about this
//            title" — including "already in the library", which is the anti-duplicate check that turns the
//            action into "In your library" instead of a button that would create a duplicate.
//   submit   THE ONE THAT COSTS SOMEBODY BANDWIDTH. It makes a server go and acquire content, so it is
//            called from exactly one place, under an explicit press, with no retry and no queue behind it.
//            THAT IS ENFORCED BY SHAPE as far as an interface can: there is no batch form, no "ensure"
//            form, and nothing in this header that a refresh path could call by accident.
//
// WHY NOT A QObject. A backend is used through callbacks the surface owns and each implementation owns its
// own QNetworkAccessManager; making the seam a QObject would buy signals nobody wants and cost probe_requests
// the ability to substitute a short fake — which is exactly how "the UI never learns which backend answered"
// is asserted.
#pragma once
#include "Requests.h"

#include <QString>
#include <QVector>
#include <functional>

// What a backend knows about one title right now.
struct RequestLookup
{
    bool                   ok = false;      // did we get an answer at all
    requests::Availability availability = requests::Availability::Unknown;
    requests::Failure      failure = requests::Failure::None;
    // The reason, for an answer we could not get. ALWAYS one of requests::failureSentence's fixed
    // sentences — never a service's own text, never Qt's errorString(), and never anything built out of the
    // request (which carries the API key in its headers).
    QString                message;

    // What the backend resolved this reference to in ITS OWN key space, carried back so a submission does
    // not have to resolve it a second time. Opaque above the seam: nothing renders it and nothing stores it
    // as an identity — requests::MediaRef::key() is the identity.
    QString                resolvedId;

    // A QUALIFIED id playback can open, for a title the service says is already in the library, or "" when
    // it cannot be named unambiguously (requests::libraryRefFor states that rule). The deep link behind
    // "In your library".
    QString                libraryRef;

    // For a series: which seasons are already there, and how many there are. Both empty/0 for a film and
    // for a backend that does not break a series down. Drives the per-season picker, which offers only the
    // seasons that would actually change something.
    QVector<int>           availableSeasons;
    int                    seasonCount = 0;
};

// One submission. `seasons` empty means the whole thing — a film, or a series asked for entire. The
// surface builds this; a backend never invents one.
struct RequestSubmission
{
    requests::MediaRef ref;
    QString            resolvedId;   // RequestLookup::resolvedId, when the lookup that preceded this had one
    QVector<int>       seasons;
};

// How a submission ended. Three arms and no fourth, because there is no retry: a failed request is reported
// to the person who pressed the button and stops there. Silently re-submitting would be the one bug this
// feature must not have — it makes somebody's server fetch content twice.
struct RequestAck
{
    bool                   ok = false;
    requests::Availability availability = requests::Availability::Unknown;
    requests::Failure      failure = requests::Failure::None;
    QString                message;   // requests::failureSentence, same rule as RequestLookup::message
};

class RequestBackend
{
public:
    virtual ~RequestBackend() = default;

    // Stable id ("jellyseerr"). What a stored row is filed under, so a row created against one backend is
    // never refreshed against another — the same discipline a scrobble queue keeps.
    virtual QString id() const = 0;
    // What the settings surface calls it. The ONLY string in this interface a user ever reads that names a
    // particular service.
    virtual QString displayName() const = 0;

    // Is there enough credential here to try at all? False means the surface shows no Request action —
    // rather than one that fails on press.
    virtual bool configured() const = 0;

    // READ-ONLY. `cb` is called exactly once, on the GUI thread, including when nothing could be reached
    // (with ok=false and a failure). Safe on every view; it changes nothing on the service.
    virtual void lookup(const requests::MediaRef& ref, int budgetMs,
                        std::function<void(const RequestLookup&)> cb) = 0;

    // THE ONE THAT ASKS FOR SOMETHING. Called only from an explicit press. `cb` is called exactly once, on
    // the GUI thread. A backend does not retry, does not queue, and does not submit anything it was not
    // handed.
    virtual void submit(const RequestSubmission& sub, int budgetMs,
                        std::function<void(const RequestAck&)> cb) = 0;
};

namespace requests
{
    // THE CHOOSER — the one place that knows which implementations exist. The UI asks for "the backend"
    // and gets one or none; it never names a service, which is what lets a second one be added here and
    // nowhere else. Owned by this module and valid for the life of the process; never delete it.
    //
    // Today: the Jellyseerr client when it is configured, otherwise nullptr. When EverythingBoxServer#16
    // lands, this is where "both configured — ask once, per media type or by user default" is decided, and
    // the call sites do not move.
    RequestBackend* configuredBackend();

    // Test seam. probe_requests installs a fake here to assert that the surface never learns which backend
    // answered; production code cannot call it (the symbol only exists under the define), so there is no
    // way to redirect the live app's backend by accident.
#ifdef EB_REQUESTS_TEST_SEAM
    void setBackendForTesting(RequestBackend* backend);
#endif
}
