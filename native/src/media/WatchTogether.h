// "Watch together" (issue #86): SyncPlay-style shared playback between two EverythingBox installs.
//
// This is the PURE half — every decision the feature makes, expressed as data in / data out, with no socket,
// no window, no QSettings and no player. It pulls in QtCore only, so probe_watchtogether drives the whole
// contract headlessly against a FAKE transport (a QByteArray the probe pipes between two Rooms) and never
// touches a relay. The live halves are deliberately thin: WatchTogetherSession (the QTcpSocket that speaks
// either straight to a LAN peer or through tools/netplay-relay.py) and MainWindowWatchTogether.cpp (the nav
// kit menus and the player wiring).
//
// THE ONE RULE THIS FILE EXISTS TO ENFORCE. A resolved stream url is a credential: it carries the user's
// debrid or provider token, which is why RecentStore keeps a re-mint RECIPE rather than a link and why #200
// had to sweep every ini in the field. So a room shares an IDENTITY — a catalogue id, an addon stream
// reference, a local-file path — and EACH PARTICIPANT RESOLVES ITS OWN STREAM with its own addons and its own
// accounts. No url ever goes on the wire, is ever logged, or is ever written into a room record. That is not
// a convention here: encode() runs every outgoing string through scrubForWire(), so a poisoned title or a
// mis-set id cannot reach the wire even if a caller hands one in, and roomRecord()/recordSafe() say the same
// thing about what is persisted. probe_watchtogether byte-scans both.
//
// The seven things that live here, and why each is here rather than in the caller:
//
//   1. THE ROOM CODE. The same alphabet netplay's online rooms use (no ambiguous 0/O/1/I), because the two
//      features share a relay and a user reading a code off a TV cannot be asked to know which is which.
//   2. THE CREDENTIAL PREDICATE. One place decides what "looks like a credential" means, so the wire, the
//      persisted record and any future surface cannot drift apart on the answer.
//   3. THE MESSAGE SET. Ten message types, one JSON object per line. Both sides read the same struct out of
//      the same parser, so a sender cannot produce a shape a receiver does not accept.
//   4. THE DRIFT DECISION. A PURE function: host position, host paused, local position, how old the beacon
//      is, and whether we are already nudging, in; nudge / hard-seek / nothing, out. It is pure because the
//      property that matters — that it converges and never oscillates — is a property of a SEQUENCE of
//      decisions, and nothing that owns an mpv handle can be run for a hundred beacons in a test.
//   5. BUFFERING, WHICH IS A SOCIAL QUESTION. "Wait for everyone" vs "keep going" is the user's answer, not
//      ours; the state machine that carries it out is here, including the case that makes it subtle — a
//      hold must never auto-resume a film the user paused on purpose.
//   6. THE ROOM. Membership, the host's authoritative transport, and the arbitration of a guest's requests,
//      as inbound-message-in / outbound-messages-out. No sockets, so a probe is two Rooms and a QByteArray.
//   7. THE PERSISTED RECORD. Exactly the map that is written to the ini, and the predicate that says it is
//      clean.
#pragma once
#include "../core/PlayOnDevice.h"   // PlayOn::ItemRef — #143's identity type, reused verbatim
#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantMap>

namespace WatchTogether
{
    // Bumped when the message set changes shape. A peer announces it in Hello; a mismatch is refused with a
    // reason rather than silently half-working.
    constexpr int kProtocol = 1;

    // ---- 1. the room code -------------------------------------------------------------------------------

    // Netplay's alphabet, deliberately: 0/O and 1/I are absent because these codes are read aloud and typed
    // on a controller keyboard.
    constexpr const char* kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    constexpr int kCodeLength = 5;

    QString makeRoomCode(quint32 entropy);              // pure in its entropy, so a probe can pin it
    QString normalizeCode(const QString& typed);        // upper-case, spaces/dashes stripped
    bool codeValid(const QString& code);                // right length, alphabet only

    // ---- 2. the credential rule -------------------------------------------------------------------------

    // Is this string something that must never leave this device? A url of any scheme (a resolved stream is
    // always one), a query carrying a token/key/signature, an Authorization value, or a long unbroken hex run
    // (a debrid token, an API key, a magnet infohash). Deliberately NOT "contains a slash": a local-library
    // reference is a filesystem path and sharing one is the whole point of the "local" kind — two boxes in one
    // house usually mount the same library.
    bool looksLikeCredential(const QString& s);

    // The value as it may appear on the wire: itself, or empty when it is a credential. Every string encode()
    // writes goes through this, so the rule is enforced by the encoder rather than by every call site.
    QString scrubForWire(const QString& s);

    // The reference as it may be shared. Each field scrubbed; refShareable() is false when scrubbing would
    // destroy the ID, because a reference with no id is one no peer could resolve and sending it would only
    // put a row on the other screen that fails when pressed.
    PlayOn::ItemRef shareableRef(const PlayOn::ItemRef& in);
    bool refShareable(const PlayOn::ItemRef& in);

    // ---- 3. the message set -----------------------------------------------------------------------------

    // One JSON object per line. Ten types and no more: a watch party is a control channel of roughly ten
    // messages a minute plus a position beacon, which is exactly what the Python relay was built to carry.
    //
    //   Hello        either -> either   I am here, this is my id/name/protocol         (a rejoin repeats it)
    //   Roster       host   -> guests   who is in the room and what each is doing
    //   Item         host   -> guests   WHAT to play — a reference, never a url
    //   Transport    host   -> guests   the authoritative paused flag + position (an answer, or a change)
    //   Beacon       host   -> guests   where the host is now, with the host's clock reading
    //   Buffering    guest  -> host     I have stalled / I have recovered
    //   RequestPause guest  -> host     please pause / resume (the host arbitrates)
    //   RequestSeek  guest  -> host     please seek here     (the host arbitrates)
    //   Unresolved   guest  -> host     I could not get this item; I am staying anyway
    //   Bye          either -> either   I am leaving
    enum class MsgType
    {
        Unknown = 0, Hello, Roster, Item, Transport, Beacon,
        Buffering, RequestPause, RequestSeek, Unresolved, Bye
    };

    QString typeId(MsgType t);          // the wire spelling
    MsgType typeFromId(const QString& id);

    struct RosterEntry
    {
        QString id;
        QString name;
        bool    host      = false;
        bool    buffering = false;
        bool    resolved  = true;   // false = in the room but could not get the item
    };

    // One struct for every type rather than a variant: the set is small, the fields that matter per type are
    // documented above, and a single shape means one encoder, one parser and one place for the scrub.
    struct Message
    {
        MsgType type = MsgType::Unknown;
        QString participantId;       // who this is about / from
        QString displayName;
        int     protocol = kProtocol;
        bool    host = false;
        PlayOn::ItemRef ref;         // Item
        double  positionSec = 0.0;   // Item / Transport / Beacon / Buffering / RequestSeek
        bool    paused = false;      // Item / Transport / Beacon / RequestPause
        qint64  clockMs = 0;         // Beacon: the host's monotonic reading when it was sent
        bool    buffering = false;   // Buffering
        QString reason;              // Unresolved / Bye — display only, scrubbed like everything else
        QList<RosterEntry> roster;   // Roster
    };

    // A single line, newline-terminated. EVERY string goes through scrubForWire on the way out.
    QByteArray encode(const Message& m);
    bool decode(const QByteArray& line, Message& out, QString& error);
    // Consume whole lines from `buf` (the socket's accumulator), leaving any partial tail behind. A line that
    // does not parse is DROPPED, not fatal: one malformed control message must not end a watch party.
    QList<Message> decodeStream(QByteArray& buf);

    // ---- 4. the drift decision --------------------------------------------------------------------------

    // The numbers, with the reasoning, in one place so the probe can pin the boundaries and nothing else can
    // spell them differently.
    struct DriftConfig
    {
        // Below a quarter second two rooms are in sync as far as anyone watching can tell, and correcting
        // beacon noise would mean nudging the rate for ever. Entering a correction needs this much.
        double deadbandSec = 0.25;
        // LEAVING a correction needs only this much — a second, smaller threshold. Two thresholds are the
        // whole reason this never oscillates: with one, a correction that lands exactly on the boundary
        // re-arms itself in the opposite direction on the next beacon and does so for ever.
        double releaseSec = 0.10;
        // Past this, nudging is the wrong tool: at the 3% ceiling a nudge closes 0.03 s per second, so five
        // seconds of drift would take nearly three minutes to walk off. Five seconds is also comfortably
        // beyond any network hiccup or seek settle, so an ordinary jitter never causes a visible jump.
        double hardSeekSec = 5.0;
        // Pitch-corrected, +-3% is imperceptible; it is the issue's own number.
        double maxNudge = 0.03;
        // The proportional term: aim to erase the drift over this many seconds. At just under the hard-seek
        // threshold it saturates the clamp; below about 0.9 s it eases off, so the approach is asymptotic
        // rather than bang-bang.
        double correctionSec = 30.0;
        // A beacon older than this — or one claiming to come from the future — is not evidence of anything.
        // Extrapolating from it would hard-seek the whole room off a single bad clock reading.
        double maxBeaconAgeSec = 30.0;
    };

    enum class DriftAction { None, Nudge, HardSeek };

    struct DriftDecision
    {
        DriftAction action = DriftAction::None;
        double speed  = 1.0;   // what to set the rate to (None restores 1.0)
        double seekTo = 0.0;   // HardSeek only
        double drift  = 0.0;   // local - host-now; positive = we are ahead. Reported for the UI and the probe
    };

    // hostPos/hostPaused as the last beacon reported them, beaconAgeSec how long ago that was, localPos where
    // we are now, `nudging` whether we are currently off 1.0. Pure and total: every input, including a
    // hostile clock, has a defined answer.
    DriftDecision decideDrift(double hostPos, bool hostPaused, double localPos, double beaconAgeSec,
                              bool nudging, const DriftConfig& cfg = DriftConfig());

    // ---- 5. buffering -----------------------------------------------------------------------------------

    enum class BufferPolicy { WaitForEveryone, KeepGoing };
    QString policyId(BufferPolicy p);                    // "wait" / "keepgoing"
    BufferPolicy policyFromId(const QString& id);        // anything unknown -> WaitForEveryone (the default)

    enum class BufferAction { Nothing, HoldForBuffering, ResumeAfterBuffering };

    // `heldForBuffering` is the flag that keeps this honest: it distinguishes a pause THIS machine applied
    // because someone stalled from a pause the user pressed. Without it, a guest recovering would resume a
    // film the host deliberately paused.
    BufferAction decideBuffering(BufferPolicy policy, bool anyoneBuffering, bool hostPlaying,
                                 bool heldForBuffering);

    // ---- 6. the room ------------------------------------------------------------------------------------

    struct Participant
    {
        QString id;
        QString name;
        bool    host = false;
        bool    buffering = false;
        bool    resolved = true;
        double  positionSec = 0.0;
        QString note;    // why it could not resolve; display only
    };

    class Room
    {
    public:
        void open(const QString& code, const QString& selfId, const QString& selfName, bool asHost);
        void close();

        bool active() const { return active_; }
        bool isHost() const { return host_; }
        QString code() const { return code_; }
        QString selfId() const { return selfId_; }

        // Host first, then join order. Stable, so a UI list does not reshuffle under the cursor.
        QList<Participant> participants() const { return people_; }
        int indexOf(const QString& id) const;
        bool anyoneBuffering() const;

        // ---- the host's authoritative state
        void setItem(const PlayOn::ItemRef& ref, double positionSec, bool paused);
        PlayOn::ItemRef item() const { return item_; }
        double hostPosition() const { return hostPos_; }
        bool hostPaused() const { return hostPaused_; }
        qint64 hostClockMs() const { return hostClockMs_; }
        // The local user pressed play/pause/seek. A HUMAN decision, so it also clears the automatic
        // buffering hold — otherwise a guest recovering from a stall would undo what the user just did.
        void setHostTransport(bool paused, double positionSec);
        // The clock ticked. Position only: deliberately NOT setHostTransport, because an ordinary position
        // tick is not a decision and must not clear the hold.
        void noteHostPosition(double positionSec);

        void setBufferPolicy(BufferPolicy p) { policy_ = p; }
        BufferPolicy bufferPolicy() const { return policy_; }
        bool heldForBuffering() const { return held_; }

        // This side's own resolution state (a guest that could not get the item stays in the room).
        //
        // BOTH RETURN "did this actually CHANGE anything", and the caller must send a message only when it
        // did. That is not tidiness, it is the fix for a message loop found on live hardware: a guest reports
        // "I resolved it" by saying hello, the host answers a hello with the room's item, and a guest that
        // reports resolution every time an item arrives has built a loop that re-opens the film for ever.
        bool setSelfUnresolved(const QString& why);
        bool setSelfResolved();
        bool selfResolved() const;

        // ---- pure I/O: one message in, the messages this side must send out.
        QList<Message> apply(const Message& in);

        // ---- what this side sends unprompted
        Message helloMessage() const;
        Message rosterMessage() const;                       // host
        Message itemMessage() const;                         // host
        Message transportMessage() const;                    // host
        Message beaconMessage(qint64 clockMs);               // host (records the clock it reported)
        Message requestPauseMessage(bool paused) const;      // guest
        Message requestSeekMessage(double positionSec) const;// guest
        Message bufferingMessage(bool buffering, double positionSec) const;
        Message unresolvedMessage(const QString& why) const;
        Message byeMessage() const;

    private:
        Participant* findMut(const QString& id);
        QList<Message> hostBufferingSweep();   // re-run decideBuffering and answer with a Transport if it moved

        bool active_ = false, host_ = false;
        QString code_, selfId_, selfName_;
        QList<Participant> people_;
        PlayOn::ItemRef item_;
        double hostPos_ = 0.0;
        bool hostPaused_ = true;      // a room opens paused: nothing plays until the host says so
        qint64 hostClockMs_ = 0;
        BufferPolicy policy_ = BufferPolicy::WaitForEveryone;
        bool held_ = false;
    };

    // ---- 7. the persisted record ------------------------------------------------------------------------

    // EXACTLY what is written under "watchtogether/" — the last room code (so a rejoin need not be retyped)
    // and the identity of what the room was watching. No url, no position-bearing link, nothing a resolver
    // produced. The ini group is named here so the writer and the probe cannot drift.
    constexpr const char* kSettingsGroup = "watchtogether";

    QVariantMap roomRecord(const Room& r);
    // Every value in a record must survive looksLikeCredential. The probe asserts this against a Room that
    // was deliberately handed a signed url.
    bool recordSafe(const QVariantMap& record);
}
