// "Play on device" (issue #143): EverythingBox instances as playback targets for one another.
//
// This is the PURE half — every decision the feature makes, expressed as data in / data out, with no socket,
// no window, no QSettings and no QtNetwork. It pulls in QtCore only, so probe_playon can drive the whole
// contract headlessly and mutate.py can pin each rule. The live halves are elsewhere and are deliberately
// thin: CastManager (the mDNS socket that both browses for and answers for `_everythingbox._tcp`),
// PlayOnClient (the QNetworkAccessManager that talks to a peer's #76 surface) and RemoteServer (the #76
// listener that now routes /open and /pair through here).
//
// The five things that live in this file, and why each is here rather than in the caller:
//
//   1. THE ADVERTISEMENT. An instance with the #76 server on publishes `_everythingbox._tcp` with TXT
//      `name=` / `ver=` / `id=`. The record layout is a wire format: it has to be byte-stable across
//      versions, so it is built and parsed in one place a probe can hold to exact bytes.
//   2. THE PICKER MERGE. One picker, three target kinds (Chromecast, DLNA, EverythingBox). The merge — which
//      order, which id namespace, and the rule that AN INSTANCE NEVER LISTS ITSELF — is a decision, not
//      presentation, so it does not live in the menu-building code where nothing can test it.
//   3. THE HAND-OFF CONTRACT. A reference plus a position, NEVER bytes. `Handoff` is the whole payload;
//      `decideOpen` is the whole answer, including the 409 an unresolvable reference gets. Both sides of the
//      wire read the same struct out of the same parser, so a source cannot send a shape a target does not
//      accept.
//   4. PAIRING. The target shows a code, the source enters it, the target issues a token. `Pairing` is that
//      state machine with nothing else attached — the attempt budget, the one-shot burn, and the refusal are
//      all here. A TOKEN IS A CREDENTIAL: it is minted from entropy the caller supplies, stored under the
//      device-local `playon/` carve-out, and never logged, never put in an error string, never returned by
//      any describe()-shaped function in this file.
//   5. REMOTE MODE AND THE PULL. `/state` JSON in, a view out (remote mode); `/state` JSON in, an open out
//      ("Continue on this device"). They are inverses over the same bytes, which is exactly why they belong
//      side by side: the probe asserts that a pull re-derives the hand-off the peer would have sent.
#pragma once
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>

namespace PlayOn
{
    // ---- 1. the advertisement -----------------------------------------------------------------------------

    // The service type, and the fully-qualified name a PTR question asks for. Two spellings of one fact; both
    // are named so no caller re-types the string and drifts.
    constexpr const char* kServiceType   = "_everythingbox._tcp";
    constexpr const char* kServiceDomain = "_everythingbox._tcp.local";

    // What THIS instance publishes. `port` is the #76 remote-control port — the advertisement exists to say
    // "there is a controllable EverythingBox here, on this port", so an advert with no port is not valid.
    struct Advert
    {
        QString instanceId;   // TXT id= — Settings::deviceId(), stable for the life of the install
        QString name;         // TXT name= — the device name the user set
        QString version;      // TXT ver= — the app version
        quint16 port = 0;     // SRV port
    };

    bool advertValid(const Advert& a);

    // The TXT strings, in a PINNED order: name, ver, id. Order is part of the wire format because a byte-exact
    // fixture is the only way to catch a silent record-layout change.
    QList<QByteArray> txtRecords(const Advert& a);

    // ---- 2. the wire --------------------------------------------------------------------------------------

    // An mDNS query: one PTR question for kServiceDomain. No compression, no additional records.
    QByteArray mdnsQuery();

    // Does this datagram ASK for our service (rather than answer for it)? Used by the responder half: an
    // instance answers only questions, and only questions about `_everythingbox._tcp`.
    bool isServiceQuery(const QByteArray& pkt);

    // The answer: PTR + SRV + TXT + A, all uncompressed, TTL 120. `ipv4` is host-order (0 omits the A record,
    // in which case a peer falls back to the sender address of the datagram).
    QByteArray mdnsResponse(const Advert& a, quint32 ipv4);

    // A discovered peer. `host` is dotted-quad IPv4.
    struct Peer
    {
        QString id;
        QString name;
        QString version;
        QString host;
        quint16 port = 0;
    };

    // Pull a peer out of an mDNS response. `senderIpv4` (host order, may be 0) is used when the packet carries
    // no A record. Returns false unless the packet yields BOTH an `id=` and a port — a half-parsed peer is a
    // peer no hand-off could reach, and listing it would be a row that always fails.
    bool parseAdvert(const QByteArray& pkt, quint32 senderIpv4, Peer& out);

    // ---- 3. the picker merge ------------------------------------------------------------------------------

    enum class TargetKind { Chromecast, Dlna, EverythingBox };

    struct Target
    {
        TargetKind kind = TargetKind::Dlna;
        QString id;        // "cc:<host>" / "dlna:<host>" / "eb:<instanceId>"
        QString name;      // the raw device / peer name
        QString label;     // exactly what the picker shows
        QString host;
        quint16 port = 0;
        bool    paired = false;   // EverythingBox only: do we already hold a token for it
    };

    QString peerTargetId(const QString& instanceId);   // "eb:" + id
    QString peerLabel(const QString& name);            // "EverythingBox on <name>"

    // One picker out of three kinds. EverythingBox peers come first (they take a reference and resolve their
    // own stream, so they are the better target whenever they are present), then the cast targets in the order
    // the caller supplied. Deduplicated by id. A peer whose id equals `selfInstanceId` is DROPPED — an
    // instance never lists itself — as is a peer with no id at all.
    QList<Target> mergeTargets(const QList<Target>& castTargets,
                               const QList<Peer>& peers,
                               const QString& selfInstanceId,
                               const QSet<QString>& pairedIds);

    // ---- 4. the hand-off contract -------------------------------------------------------------------------

    // A reference to a thing to play. NEVER a URL, never bytes: the target resolves its own stream.
    //   kind == "catalog" — a catalogue id (an addon/Cinemeta id such as "tt1375666", or "<imdb>:S:E")
    //   kind == "addon"   — an addon stream reference, `source` naming the addon that owns it
    //   kind == "server"  — a server-qualified id from a configured EverythingBox server
    //   kind == "local"   — a local-library / ROM-library id, resolvable only where that library exists
    struct ItemRef
    {
        QString kind;
        QString id;
        QString type;     // "movie" | "episode" | "series" | "music" | "game" | ... (display + routing hint)
        QString title;    // display only; never used to resolve
        QString source;   // addon / server key, when the kind needs one
    };

    // The whole payload of a hand-off: a reference, a position, and the selected track ids. Track ids are
    // opaque strings (a language code, an index, ""), because what identifies a track differs per source and
    // the target re-selects with its own player.
    struct Handoff
    {
        ItemRef ref;
        double  positionSec = 0.0;
        QString audioTrack;
        QString subtitleTrack;
    };

    QByteArray handoffJson(const Handoff& h);
    bool parseHandoff(const QByteArray& json, Handoff& out, QString& error);

    // What the TARGET knows about itself when a hand-off arrives. Deliberately a flat answer-sheet rather than
    // a pointer to the library: the decision below is then pure, and the caller does the looking-up.
    struct OpenEnv
    {
        bool localIdKnown     = false;   // kind=="local": this device's own library has that id
        bool addonsAvailable  = false;   // kind=="catalog"/"addon": this device has a source that could resolve it
        bool serverConfigured = false;   // kind=="server": this device is signed in to that server
        bool profileBlocks    = false;   // the profile active HERE refuses this item (passcode / rating wall)
        QString blockReason;             // shown on the SOURCE as the reason; must never name a credential
    };

    enum class OpenOutcome { Accepted, BadRequest, Gated, Unresolvable };

    struct OpenResult
    {
        OpenOutcome outcome = OpenOutcome::BadRequest;
        int         httpStatus = 400;    // 200 / 400 / 403 / 409
        QString     reason;              // human-readable; "" when accepted
    };

    // The order matters and is deliberate: malformed first (400), THEN the profile gate (403), THEN
    // resolvability (409). The gate runs before the lookup so a walled-off target does not answer "I could
    // have played that" — a hand-off cannot be used to probe what is behind a passcode wall.
    OpenResult decideOpen(const Handoff& h, const OpenEnv& env);

    QByteArray openResultJson(const OpenResult& r);

    // The source's side of the same answer: what the user is shown for a refusal, with the target named.
    QString describeRefusal(const OpenResult& r, const QString& deviceName);

    // ---- 5. pairing ---------------------------------------------------------------------------------------

    // Six digits, zero-padded, derived from caller-supplied entropy. Pure so the probe can pin it; the live
    // caller passes QRandomGenerator::system().
    QString pairingCode(quint32 entropy);

    // Spaces and dashes are what a user types when reading six digits off a TV; strip them before comparing.
    QString normalizeCode(const QString& code);
    bool codeMatches(const QString& expected, const QString& entered);

    // Mint a token from caller-supplied entropy. A CREDENTIAL: 64 hex characters, never logged, never
    // embedded in a reason string, never returned by any describe() in this file.
    QString mintToken(const QByteArray& entropy);

    // Where a peer's token lives. Under "playon/", which CloudSync's device-local carve-out excludes, so a
    // token minted for THIS device never rides the synced settings bundle to another one.
    //
    // BOTH ARE INLINE, and deliberately, for the reason ProfilePasscode's ini-group predicate next door is:
    // CloudSync::isDeviceLocalKey and SettingsTxn::inScope consult the carve-out, and a dozen probes compile
    // CloudSync.cpp. A predicate in a .cpp would make every one of them link this unit -- twelve CMake edits
    // for one string comparison, and a thirteenth probe added later would fail to link for reasons that read
    // as unrelated. Header-only, the carve-out is still asserted through the WRITER'S OWN key builder
    // (probe_cloudmerge), which is the property that matters: the two cannot drift.
    inline QString tokenKey(const QString& peerInstanceId)
    {
        return QStringLiteral("playon/peers/") + peerInstanceId + QStringLiteral("/token");
    }
    inline bool isDeviceLocalKey(const QString& key)     // the "playon/" family, for the carve-out
    {
        return key.startsWith(QLatin1String("playon/"));
    }

    // The TARGET's pairing state machine. begin() arms exactly one offer and returns the code to put on
    // screen; redeem() answers with a token on a match and an empty string otherwise. Three wrong answers
    // burn the offer — the code has to be re-shown, which needs someone in front of the target's screen.
    class Pairing
    {
    public:
        static constexpr int kMaxAttempts = 3;

        QString begin(quint32 entropy);      // returns the code to display
        void    cancel();

        bool    pending() const { return pending_; }
        QString code() const { return code_; }
        int     attemptsLeft() const { return pending_ ? kMaxAttempts - attempts_ : 0; }

        // A match issues and returns a token; anything else returns an empty string. The offer is consumed on
        // success and burned after kMaxAttempts failures.
        QString redeem(const QString& entered, const QByteArray& tokenEntropy);

    private:
        bool    pending_ = false;
        QString code_;
        int     attempts_ = 0;
    };

    // ---- 6. auth ------------------------------------------------------------------------------------------

    // Which routes need a paired token. /open does — it starts playback on someone else's screen. /pair
    // cannot (it is how a token is obtained), and /state / /player / /input keep #76's posture so the phone
    // remote that shipped with that issue still works untouched.
    bool routeNeedsToken(const QString& path);

    bool authorized(const QString& presented, const QSet<QString>& issuedTokens);

    QByteArray unauthorizedJson();

    // ---- 7. remote mode -----------------------------------------------------------------------------------

    struct RemoteView
    {
        bool    reachable = false;         // did /state answer at all
        bool    hasMedia  = false;
        bool    playing   = false;
        QString title;
        double  positionSec = 0.0;
        double  durationSec = 0.0;
        int     volume = 0;
        bool    volumeControllable = false;
    };

    RemoteView remoteView(const QByteArray& stateJson, bool httpOk);

    // The /player bodies the native remote sends. Small enough to inline at the call site, which is exactly
    // why they are here: three call sites spelling `{"action":"playpause"}` slightly differently is how a
    // transport button silently stops working.
    QByteArray playerCommandBody(const QString& action);
    QByteArray seekCommandBody(double positionSec);
    QByteArray volumeCommandBody(int level);

    // ---- 8. "Continue on this device" ---------------------------------------------------------------------

    // The inverse of the hand-off: a peer's /state, read back into the open THIS device should perform. Not
    // valid when the peer is unreachable, playing nothing, or playing something whose reference it did not
    // report (a source that cannot name what it is playing cannot be taken over).
    struct Pull
    {
        bool    valid = false;
        QString reason;
        ItemRef ref;
        double  positionSec = 0.0;
        QString audioTrack;
        QString subtitleTrack;
    };

    Pull continueHere(const QByteArray& stateJson, bool httpOk);

    // The hand-off a Pull is equivalent to. Exposed so the symmetry is a single expression the probe can
    // assert against handoffJson(): a pull re-derives exactly the payload the peer would have sent.
    Handoff handoffFromPull(const Pull& p);
}
