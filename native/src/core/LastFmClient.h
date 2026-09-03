// LAST.FM (issue #192, increment 2) — the SECOND implementation of the ScrobbleProvider seam, and the first
// test of whether that seam was the right shape.
//
// It was: nothing about the threshold, what counts as a track, the offline queue or the double-count
// coordination is repeated here. What IS here is everything Last.fm's protocol asks for that ListenBrainz's
// does not — an application identity, a signed request, a three-step authorisation, a batch shape with
// indexed parameter names, and one accept rule of its own.
//
// ==================================================================================================
// THE APPLICATION KEY, AND WHY THERE IS NO USER-SUPPLIED FALLBACK
// ==================================================================================================
// ListenBrainz needs ONE user token, pasted once, and that is the whole reason it went first. Last.fm needs
// an api_key + shared secret belonging to the APPLICATION before a user can be asked for anything at all:
// even auth.getToken, the first call of the authorisation flow, is signed with the secret. So the credential
// cannot come from the user — it is not theirs, and a per-user API key is not the shape their desktop-auth
// flow is built around.
//
// It therefore comes from the #81 build-time slot mechanism (native/secrets/lastfm.secrets ->
// GenerateSecrets.cmake -> BuiltinSecrets.h in the build tree). A build with no key embedded is the ordinary
// case for anybody who clones this repository, and it must not look broken: `availableInThisBuild()` is
// false, the settings row says so in one sentence, and NOTHING else about Last.fm is offered — no connect
// button that cannot work, no token field for a credential that is not a token. See statusFor().
//
// ==================================================================================================
// SIGNING — the one thing a naive implementation gets wrong silently
// ==================================================================================================
// Last.fm's spec (https://www.last.fm/api/authspec, "Signing Calls"): order every parameter of the call
// ALPHABETICALLY BY NAME, concatenate them as <name><value> with no separators, append the shared secret,
// and md5 the result. Two clauses do the damage when they are missed:
//
//   * `format` IS NOT SIGNED. It is a response-encoding hint, not a call parameter, and including it makes
//     every signed call fail with "Invalid method signature" — a message that names nothing useful and sends
//     you looking at the key. So `format=json` is added to the request AFTER the signature is computed, and
//     it never enters the signed map. Same for api_sig itself, for the obvious reason.
//   * THE ORDER IS OVER THE PARAMETER NAMES AS STRINGS, INDICES AND ALL. A batched track.scrobble sends
//     artist[0], artist[1], … timestamp[0] …, and a string sort puts artist[10] BEFORE artist[1] (']' is
//     0x5D, '0' is 0x30, so the longer name wins at the first differing character) rather than between
//     artist[1] and artist[2], which is where intuition puts it. That is what the service does; an
//     implementation that "helpfully" sorted the indices numerically would produce a signature it rejects,
//     and only for batches of eleven or more — so it would work in every hand test. probe_scrobble §7b
//     pins it, and pinned the author's own expectation being the wrong way round.
//
// signatureBase() is exposed separately from signature() so a probe can assert the SPEC'S OWN WORKED EXAMPLE
// string byte for byte, rather than only comparing one md5 to another md5 — a hash comparison tells you the
// two sides agree, not that either agrees with Last.fm.
//
// ==================================================================================================
// THE THREE-STEP AUTHORISATION, AND WHERE THE SESSION KEY LIVES
// ==================================================================================================
//   1. auth.getToken (signed)                         -> a request token, valid for 60 minutes
//   2. the USER approves it at www.last.fm/api/auth/?api_key=…&token=…, in a browser. On a desktop the app
//      opens it; on a TV the URL is shown to be typed, exactly as Trakt's device-code screen shows its own.
//   3. auth.getSession (signed, with that token)      -> a SESSION KEY, which does not expire
//
// Step 3 is POLLED, because there is no callback: until the user approves, Last.fm answers error 14
// ("unauthorised token") and the client simply asks again. That is the same shape as TraktClient's device
// poll, for the same reason, and it is bounded (kAuthPollAttempts) so an abandoned attempt stops on its own
// rather than talking to Last.fm for the rest of the session.
//
// ONLY the session key is stored — never the request token (it is spent), and there is no password anywhere
// in this flow by design. It goes in the device-local scrobble carve-out, per profile
// (scrobble/<profile>/lastfm/sk), which Scrobble::isDeviceLocalKey excludes from every sync bundle.
//
// It is also OUT of a settings transaction's scope, which is where it differs from the ListenBrainz token
// sitting under the same prefix. That token is TYPED, so pasting the wrong one and pressing Discard has to
// put the old one back. A session key is not typed: it arrives from a background poll reply after a browser
// round trip, so in scope it inflates the exit prompt with changes the user never made and a Discard
// silently unlinks an account they cannot simply re-type. Scrobble::isAuthorisedCredentialKey draws that
// line; SettingsTxn.cpp's "ra/user" / "ra/token" entry is the same decision for RetroAchievements.
//
// ==================================================================================================
// THE CREDENTIAL RULE, WHICH IS THE SAME ONE ListenBrainzClient.cpp ENFORCES
// ==================================================================================================
// Nothing here logs a request. Every message that can reach a status line is built by ONE function from the
// SERVICE's own words and the transport error — never from the request, which carries the api_key, the
// api_sig and the session key in its body. An error path that reports "the request that failed" reports the
// credential inside it, and there is no later stage that can take it back out.
#pragma once
#include "ScrobbleProvider.h"

#include <QMap>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QTimer;

namespace LastFm
{
    // Last.fm ignores anything 30 seconds or shorter, and says so in its API docs rather than in a reply you
    // can act on. THIS IS DELIBERATELY NOT IN Scrobble.h: it is a property of ONE service's contract, and
    // ListenBrainz has no such rule — putting it in the shared rules would make this app refuse to send
    // ListenBrainz a listen ListenBrainz would have accepted. Scrobble.h says so in as many words.
    constexpr int kMinTrackSec = 31;       // "longer than 30 seconds"

    // How many listens go in one track.scrobble call. Last.fm's documented maximum is 50, which is also
    // ScrobbleQueue::kBatchSize — the queue hands over at most that many, so no second split is needed here.
    constexpr int kMaxBatch = 50;

    // A track of UNKNOWN length is sent. Last.fm's duration parameter is optional, and refusing to submit
    // everything we could not read a duration for would silently drop whole streaming sources.
    inline bool longEnough(int durationSec) { return durationSec <= 0 || durationSec >= kMinTrackSec; }

    // THE SPEC'S CONCATENATION, exactly: parameters sorted by name, each rendered <name><value>, nothing
    // between them. The secret is NOT included — signature() appends it — so this string is safe to assert
    // in a probe and safe to reason about, and the one thing that must never be printed stays in one place.
    QString signatureBase(const QMap<QString, QString>& params);

    // md5(signatureBase(params) + secret), lower-case hex. What goes in `api_sig`.
    QString signature(const QMap<QString, QString>& params, const QString& secret);

    // Last.fm answers MOST errors with HTTP 200 and an {"error":N,"message":"…"} body, so an implementation
    // that only reads the status code treats a refused session key as a success and drops the listens. Both
    // inputs are taken; `lastFmError` is 0 when the body carried no error object.
    ScrobbleResult::Outcome outcomeFor(int httpStatus, int lastFmError);
}

class LastFmClient : public QObject, public ScrobbleProvider
{
    Q_OBJECT
public:
    explicit LastFmClient(QObject* parent = nullptr);
    ~LastFmClient() override;

    // ---- the build-time application identity (#81) ----
    static QString appKey();
    static QString appSecret();
    // Both halves, or nothing. A key without a secret cannot sign a single call, so a half-filled slot is
    // treated as an empty one rather than as a feature that fails at the first request.
    static bool    availableInThisBuild();

    // ---- the user's link to their own account ----
    static bool    connected();            // a session key is stored for the active profile
    static QString accountName();          // the Last.fm username the session belongs to (may be empty)

    // The one sentence both settings builders show. PURE and static in its inputs so a probe can assert
    // every arm — including "not available in this build" — in a binary that has exactly one of them baked in.
    static QString statusFor(bool available, bool linked, const QString& user);
    // WHAT THE CONNECT/DISCONNECT ACTION SAYS. Not simply "linked?": a session key stored by a build that
    // HAD an application key survives in the ini when the same install is rebuilt without one, so `linked` is
    // true while the provider is not installed at all. Pure in its inputs for the same reason statusFor is.
    static QString connectActionLabel(bool available, bool linked);
    static QString connectActionLabel();          // ...for this build and this profile

    // The same sentence for THIS build and THIS profile. Static because both settings builders read it from
    // places that have no provider object to hand — a build with no application key does not construct one
    // at all, and that is exactly the case whose row has the most to say.
    static QString statusText();

    // Begin the desktop authorisation. Emits authUrl() with the page the user must approve, then polls
    // auth.getSession until they have (or until the attempt times out). Safe to call while one is running:
    // the running one is cancelled first, so a double press does not leave two polls talking to Last.fm.
    void connectAccount();
    // Forget the session key and the username, for the ACTIVE profile only. Does not touch the queue: listens
    // waiting for Last.fm keep waiting, and land if the user links the account again.
    void disconnectAccount();

    // ---- ScrobbleProvider ----
    QString id() const override          { return QStringLiteral("lastfm"); }
    QString displayName() const override { return QStringLiteral("Last.fm"); }
    bool    configured() const override;
    void    nowPlaying(const Scrobble::Track& track) override;
    void    submit(const QVector<Scrobble::Play>& plays,
                   std::function<void(ScrobbleResult)> cb) override;
    bool    supportsLove() const override { return true; }
    void    love(const Scrobble::Track& track, bool loved,
                 std::function<void(ScrobbleResult)> cb) override;

    // ---- test seam ----
    // Point the client at an in-process fake. THE PROBE'S ONLY WAY IN, and deliberately NOT a setting and
    // NOT an environment variable: this decides where an application key and a user's session key are sent,
    // and neither a settings file nor a process environment is a thing this app should let choose that.
    // Enforced, not merely documented — a root that is not http on loopback is IGNORED, so even a mistaken
    // call in shipping code cannot send the credential to a third party. An empty string restores the real
    // service.
    static void setApiRootForTests(const QString& root);
    static QString apiRoot();          // the endpoint in force
    static QString authPageRoot();     // the page the USER approves at (loopback-redirected with apiRoot)

signals:
    // Show this URL to the user (and, on a desktop, it has already been opened for them).
    void authUrl(const QString& url);
    // The link came up or went away. Both settings builders re-render on it.
    void connectedChanged(bool linked);
    // The authorisation could not be completed. Carries the SERVICE's words, never the request.
    void connectError(const QString& message);

private:
    void pollSession(const QString& requestToken, int attemptsLeft);
    void stopAuth();

    QNetworkAccessManager* nam_ = nullptr;
    QTimer* authPoll_ = nullptr;
};
