// Headless check of "Watch together" (issue #86) — src/media/WatchTogether.{h,cpp}, the pure half of the
// SyncPlay-style shared-playback feature.
//
// NO SOCKET AND NO RELAY. Everything below runs against a FAKE transport: a `Wire` that encodes a message to
// bytes, appends them to a transcript, and hands them to the other Room's inbox. That is not a convenience —
// it is what lets the last section byte-scan EVERY BYTE THAT WOULD HAVE CROSSED, which is the assertion this
// whole feature exists to be able to make.
//
// What this pins:
//
//   1. THE ROOM CODE. Netplay's alphabet (no ambiguous 0/O/1/I), deterministic in its entropy, and the
//      normalisation a user's typing needs (case, spaces, dashes).
//   2. THE CREDENTIAL PREDICATE. A url of any scheme, a token/key/signature query, a long hex run — and the
//      things it must NOT refuse, because refusing them would break the feature: a catalogue id, a Windows
//      path, an ordinary film title.
//   3. THE MESSAGE SET AND ITS ENCODING. Every type round-trips; the wire spellings are pinned as literals
//      (they are a wire format); a partial line is held back and completed; a malformed line is dropped
//      rather than fatal; and the ENCODER SCRUBS — a poisoned title or id handed to encode() comes out empty.
//   4. THE DRIFT FUNCTION over its whole range: the deadband, EXACTLY at each threshold, the release
//      hysteresis, the +-3% clamp, the hard seek, a paused host that does not extrapolate, a hostile clock
//      (negative, absurd, NaN), and NON-OSCILLATION — a 400-beacon simulation in both directions in which the
//      correction never once changes sign and the drift never grows.
//   5. THE ROOM, over the fake wire: join-in-progress landing at the host's position, WHAT arriving before
//      WHERE, a guest that cannot resolve staying in the room and being unable to steer it, the host's
//      arbitration of pause and seek requests, BOTH buffering policies including the case that makes the
//      feature honest (an automatic hold must never resume a film the user paused), and a participant
//      leaving and rejoining without turning into two rows.
//   6. THE CREDENTIAL BYTE-SCAN. The whole transcript, and the persisted room record, scanned for the token
//      and for any url at all — after a drive in which the host was deliberately handed a resolved,
//      account-bearing stream url as the thing to share.
//
// Prints WATCHTOGETHER-OK on success; any failure prints WATCHTOGETHER-FAIL <cond> (line) and exits non-zero.
#include "WatchTogether.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantMap>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace WatchTogether;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "WATCHTOGETHER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool nearly(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// ---- the fake transport ------------------------------------------------------------------------------------
// Two Rooms and one byte buffer each way. `transcript` accumulates EVERY byte either side would have written
// to a socket, in order, and is never cleared: section 6 scans it whole.
struct Wire
{
    QByteArray transcript;
    QByteArray toGuest, toHost;

    void put(bool fromHost, const QList<Message>& ms)
    {
        for (const Message& m : ms)
        {
            const QByteArray line = encode(m);
            transcript += line;
            if (fromHost) toGuest += line; else toHost += line;
        }
    }
    void put(bool fromHost, const Message& m) { put(fromHost, QList<Message>{ m }); }

    // Deliver everything queued for one side, feeding whatever it answers back into the other direction.
    // Bounded, so a protocol that answered its own answers would fail the test rather than hang it.
    void pump(Room& host, Room& guest)
    {
        for (int spin = 0; spin < 8; ++spin)
        {
            if (toHost.isEmpty() && toGuest.isEmpty()) return;
            QList<Message> hostOut, guestOut;
            for (const Message& m : decodeStream(toHost))  hostOut  << host.apply(m);
            for (const Message& m : decodeStream(toGuest)) guestOut << guest.apply(m);
            put(true, hostOut);
            put(false, guestOut);
        }
        CHECK(toHost.isEmpty() && toGuest.isEmpty());   // the pump did not settle: a message loop
    }
};

int main()
{
    // A resolved, account-bearing stream url — exactly the artefact that must never leave a device. The 40-hex
    // run in the middle is the account's own credential; nothing below ever prints it.
    const QString kSecret = QStringLiteral("9f2c4be1a7d34e5f8b0c1d2e3f405162738495a6");
    const QString kStreamUrl = QStringLiteral("https://cdn.example-debrid.test/dl/") + kSecret
                             + QStringLiteral("/Inception.2010.mkv?apikey=") + kSecret;

    // =========================================================== 1. the room code ===========================
    {
        const QString a = makeRoomCode(0u);
        const QString b = makeRoomCode(123456789u);
        CHECK(a.size() == kCodeLength);
        CHECK(b.size() == kCodeLength);
        CHECK(makeRoomCode(123456789u) == b);        // deterministic in its entropy
        CHECK(a != b);
        CHECK(codeValid(a) && codeValid(b));
        // The alphabet netplay uses, and the reason it uses it.
        const QString alphabet = QString::fromLatin1(kCodeAlphabet);
        CHECK(alphabet.size() == 32);
        CHECK(!alphabet.contains(QLatin1Char('0')));
        CHECK(!alphabet.contains(QLatin1Char('O')));
        CHECK(!alphabet.contains(QLatin1Char('1')));
        CHECK(!alphabet.contains(QLatin1Char('I')));

        CHECK(normalizeCode(QStringLiteral(" a b-c d e ")) == QStringLiteral("ABCDE"));
        CHECK(normalizeCode(QStringLiteral("kj7-2m")) == QStringLiteral("KJ72M"));
        CHECK(codeValid(QStringLiteral("KJ72M")));
        CHECK(!codeValid(QStringLiteral("KJ72")));            // too short
        CHECK(!codeValid(QStringLiteral("KJ72MM")));          // too long
        CHECK(!codeValid(QStringLiteral("KJ7OM")));           // O is not in the alphabet
        CHECK(!codeValid(QString()));
    }

    // =========================================================== 2. the credential predicate ================
    {
        CHECK(looksLikeCredential(kStreamUrl));
        CHECK(looksLikeCredential(kSecret));                                        // the bare token
        CHECK(looksLikeCredential(QStringLiteral("http://box.lan/file.mkv")));      // any url at all
        CHECK(looksLikeCredential(QStringLiteral("magnet://x")));
        CHECK(looksLikeCredential(QStringLiteral("Bearer abc")));
        CHECK(looksLikeCredential(QStringLiteral("x?token=abc")));
        CHECK(looksLikeCredential(QStringLiteral("x?apikey=abc")));
        CHECK(looksLikeCredential(QStringLiteral("x?api_key=abc")));
        CHECK(looksLikeCredential(QStringLiteral("x&signature=abc")));
        CHECK(looksLikeCredential(QStringLiteral("X-Amz-Date=1")));

        // And the far more dangerous half: what it must NOT refuse. A predicate that swallowed these would
        // silently break the feature rather than break the rule.
        CHECK(!looksLikeCredential(QStringLiteral("tt1375666")));
        CHECK(!looksLikeCredential(QStringLiteral("tt0903747:1:3")));
        CHECK(!looksLikeCredential(QStringLiteral("C:/Films/Inception (2010)/Inception.mkv")));
        CHECK(!looksLikeCredential(QStringLiteral("Inception")));
        CHECK(!looksLikeCredential(QStringLiteral("com.linvo.cinemeta")));
        CHECK(!looksLikeCredential(QString()));
        // A 31-hex run is not long enough to be a token; 32 is the line.
        CHECK(!looksLikeCredential(QStringLiteral("0123456789abcdef0123456789abcde")));
        CHECK(looksLikeCredential(QStringLiteral("0123456789abcdef0123456789abcdef")));

        CHECK(scrubForWire(kStreamUrl).isEmpty());
        CHECK(scrubForWire(QStringLiteral("tt1375666")) == QStringLiteral("tt1375666"));

        PlayOn::ItemRef good;
        good.kind = QStringLiteral("catalog"); good.id = QStringLiteral("tt1375666");
        good.type = QStringLiteral("movie");   good.title = QStringLiteral("Inception");
        CHECK(refShareable(good));
        PlayOn::ItemRef poisoned = good;
        poisoned.id = kStreamUrl;
        CHECK(!refShareable(poisoned));                       // no id a peer could resolve -> do not send it
        CHECK(shareableRef(poisoned).id.isEmpty());
        CHECK(shareableRef(poisoned).title == QStringLiteral("Inception"));
        PlayOn::ItemRef noKind = good; noKind.kind.clear();
        CHECK(!refShareable(noKind));
    }

    // =========================================================== 3. the message set =========================
    {
        // The wire spellings are a FORMAT: pinned as literals, not round-tripped through the table.
        CHECK(typeId(MsgType::Hello) == QStringLiteral("hello"));
        CHECK(typeId(MsgType::Roster) == QStringLiteral("roster"));
        CHECK(typeId(MsgType::Item) == QStringLiteral("item"));
        CHECK(typeId(MsgType::Transport) == QStringLiteral("xport"));
        CHECK(typeId(MsgType::Beacon) == QStringLiteral("beacon"));
        CHECK(typeId(MsgType::Buffering) == QStringLiteral("buf"));
        CHECK(typeId(MsgType::RequestPause) == QStringLiteral("reqpause"));
        CHECK(typeId(MsgType::RequestSeek) == QStringLiteral("reqseek"));
        CHECK(typeId(MsgType::Unresolved) == QStringLiteral("unres"));
        CHECK(typeId(MsgType::Bye) == QStringLiteral("bye"));
        CHECK(typeFromId(QStringLiteral("xport")) == MsgType::Transport);
        CHECK(typeFromId(QStringLiteral("nope")) == MsgType::Unknown);
        CHECK(encode(Message()).isEmpty());                   // an Unknown message encodes to nothing

        Message item;
        item.type = MsgType::Item;
        item.participantId = QStringLiteral("host-1");
        item.ref.kind = QStringLiteral("catalog");
        item.ref.id = QStringLiteral("tt1375666");
        item.ref.type = QStringLiteral("movie");
        item.ref.title = QStringLiteral("Inception");
        item.ref.source = QStringLiteral("com.linvo.cinemeta");
        item.positionSec = 812.5;
        item.paused = true;
        const QByteArray line = encode(item);
        CHECK(line.endsWith('\n'));
        CHECK(line.count('\n') == 1);
        Message back; QString err;
        CHECK(decode(line, back, err));
        CHECK(back.type == MsgType::Item);
        CHECK(back.ref.id == item.ref.id);
        CHECK(back.ref.source == item.ref.source);
        CHECK(back.ref.title == item.ref.title);
        CHECK(nearly(back.positionSec, 812.5));
        CHECK(back.paused);
        CHECK(back.protocol == kProtocol);

        Message beacon;
        beacon.type = MsgType::Beacon;
        beacon.positionSec = 61.25; beacon.clockMs = 1234567890123LL; beacon.paused = false;
        CHECK(decode(encode(beacon), back, err));
        CHECK(back.type == MsgType::Beacon);
        CHECK(nearly(back.positionSec, 61.25));
        CHECK(back.clockMs == 1234567890123LL);

        Message roster;
        roster.type = MsgType::Roster;
        RosterEntry e1; e1.id = QStringLiteral("host-1"); e1.name = QStringLiteral("Living room"); e1.host = true;
        RosterEntry e2; e2.id = QStringLiteral("g-2"); e2.name = QStringLiteral("Sam"); e2.buffering = true; e2.resolved = false;
        roster.roster << e1 << e2;
        CHECK(decode(encode(roster), back, err));
        CHECK(back.roster.size() == 2);
        CHECK(back.roster.at(0).host);
        CHECK(back.roster.at(1).buffering);
        CHECK(!back.roster.at(1).resolved);

        // A stream: two whole lines consumed, the partial third held back until it completes.
        QByteArray buf = encode(item) + encode(beacon);
        const QByteArray third = encode(roster);
        buf += third.left(6);
        QList<Message> got = decodeStream(buf);
        CHECK(got.size() == 2);
        CHECK(!buf.isEmpty());                                 // the partial line is still there
        buf += third.mid(6);
        got = decodeStream(buf);
        CHECK(got.size() == 1);
        CHECK(got.at(0).type == MsgType::Roster);
        CHECK(buf.isEmpty());

        // A malformed line is DROPPED, not fatal — one bad control message must not end a watch party.
        QByteArray mixed = QByteArray("not json at all\n") + encode(beacon)
                         + QByteArray("{\"t\":\"whatever\"}\n") + encode(item);
        got = decodeStream(mixed);
        CHECK(got.size() == 2);
        CHECK(got.at(0).type == MsgType::Beacon);
        CHECK(got.at(1).type == MsgType::Item);

        // THE ENCODER SCRUBS. Hand it a poisoned reference and the bytes come out without it.
        Message poisoned = item;
        poisoned.ref.id = kStreamUrl;
        poisoned.ref.title = QStringLiteral("Inception ") + kStreamUrl;
        poisoned.participantId = kSecret;
        poisoned.reason = kStreamUrl;
        const QByteArray poisonedLine = encode(poisoned);
        CHECK(!poisonedLine.contains(kSecret.toUtf8()));
        CHECK(!poisonedLine.contains("://"));
        CHECK(!poisonedLine.contains("apikey"));
    }

    // =========================================================== 4. the drift function ======================
    {
        const DriftConfig cfg;
        CHECK(nearly(cfg.deadbandSec, 0.25));
        CHECK(nearly(cfg.releaseSec, 0.10));
        CHECK(nearly(cfg.hardSeekSec, 5.0));
        CHECK(nearly(cfg.maxNudge, 0.03));
        // The hysteresis only works if leaving costs LESS than entering.
        CHECK(cfg.releaseSec < cfg.deadbandSec);

        // In sync: nothing, and the rate restored to 1.0.
        DriftDecision d = decideDrift(100.0, true, 100.0, 0.0, false);
        CHECK(d.action == DriftAction::None);
        CHECK(nearly(d.speed, 1.0));
        CHECK(nearly(d.drift, 0.0));

        // EXACTLY at the deadband is "do nothing": a boundary belongs to the quiet side.
        d = decideDrift(100.0, true, 100.25, 0.0, false);
        CHECK(d.action == DriftAction::None);
        CHECK(nearly(d.drift, 0.25));
        // A hair past it nudges, and in the right direction: ahead of the host means slow down.
        d = decideDrift(100.0, true, 100.26, 0.0, false);
        CHECK(d.action == DriftAction::Nudge);
        CHECK(d.speed < 1.0);
        // Behind the host means speed up.
        d = decideDrift(100.0, true, 99.74, 0.0, false);
        CHECK(d.action == DriftAction::Nudge);
        CHECK(d.speed > 1.0);

        // The RELEASE threshold: already nudging, a drift the deadband would have ignored still corrects,
        // right down to exactly 0.10 — where it stops.
        d = decideDrift(100.0, true, 100.20, 0.0, true);
        CHECK(d.action == DriftAction::Nudge);
        d = decideDrift(100.0, true, 100.10, 0.0, true);
        CHECK(d.action == DriftAction::None);
        CHECK(nearly(d.speed, 1.0));
        d = decideDrift(100.0, true, 100.11, 0.0, true);
        CHECK(d.action == DriftAction::Nudge);
        // ...and the same 0.20 drift with nudging FALSE is inside the deadband and left alone. This pair IS
        // the hysteresis: same inputs, different answer, because of where the controller already is.
        d = decideDrift(100.0, true, 100.20, 0.0, false);
        CHECK(d.action == DriftAction::None);

        // The +-3% clamp, at a drift just under the hard-seek line.
        d = decideDrift(100.0, true, 104.99, 0.0, false);
        CHECK(d.action == DriftAction::Nudge);
        CHECK(nearly(d.speed, 0.97));
        d = decideDrift(100.0, true, 95.01, 0.0, false);
        CHECK(d.action == DriftAction::Nudge);
        CHECK(nearly(d.speed, 1.03));

        // EXACTLY at the hard-seek threshold seeks (this boundary belongs to the loud side: five seconds out
        // is not something to walk off at 3%).
        d = decideDrift(100.0, true, 105.0, 0.0, false);
        CHECK(d.action == DriftAction::HardSeek);
        CHECK(nearly(d.seekTo, 100.0));
        CHECK(nearly(d.speed, 1.0));
        d = decideDrift(100.0, true, 104.999, 0.0, false);
        CHECK(d.action == DriftAction::Nudge);

        // JOIN IN PROGRESS is just the far end of the same function: a guest at zero, a host an hour in.
        d = decideDrift(3600.0, false, 0.0, 0.0, false);
        CHECK(d.action == DriftAction::HardSeek);
        CHECK(nearly(d.seekTo, 3600.0));

        // A PAUSED host does not advance, so a stale beacon from one is still exactly right.
        d = decideDrift(100.0, true, 100.0, 10.0, false);
        CHECK(d.action == DriftAction::None);
        // The same beacon from a PLAYING host means the host is ten seconds further on by now.
        d = decideDrift(100.0, false, 100.0, 10.0, false);
        CHECK(d.action == DriftAction::HardSeek);
        CHECK(nearly(d.seekTo, 110.0));

        // A HOSTILE CLOCK. A beacon from the future, an absurd age, or a NaN age is not evidence of anything:
        // extrapolating from one would hard-seek the whole room off a single bad reading.
        d = decideDrift(100.0, false, 100.0, -5.0, false);
        CHECK(d.action == DriftAction::None);
        CHECK(nearly(d.speed, 1.0));
        d = decideDrift(100.0, false, 100.0, 1.0e9, false);
        CHECK(d.action == DriftAction::None);
        d = decideDrift(100.0, false, 100.0, std::numeric_limits<double>::quiet_NaN(), false);
        CHECK(d.action == DriftAction::None);
        d = decideDrift(std::numeric_limits<double>::quiet_NaN(), false, 100.0, 1.0, false);
        CHECK(d.action == DriftAction::None);
        // The age ceiling, exactly: 30 s is still usable, a hair past it is not.
        CHECK(decideDrift(100.0, true, 130.0, 30.0, false).action == DriftAction::HardSeek);
        CHECK(decideDrift(100.0, true, 130.0, 30.001, false).action == DriftAction::None);

        // NON-OSCILLATION, both directions. One beacon a second, the guest running at whatever rate the last
        // decision asked for. The properties: the correction never changes sign, the drift never grows, and
        // once it settles it stays settled.
        for (int dir = 0; dir < 2; ++dir)
        {
            double host = 600.0;
            double local = dir == 0 ? 598.0 : 602.0;    // two seconds behind, then two seconds ahead
            bool nudging = false;
            double prevMag = std::fabs(local - host);
            int settledFor = 0;
            bool sawWrongSign = false, sawGrowth = false, sawSeek = false;
            for (int step = 0; step < 400; ++step)
            {
                const DriftDecision s = decideDrift(host, false, local, 0.0, nudging, cfg);
                if (s.action == DriftAction::HardSeek) sawSeek = true;
                if (s.action == DriftAction::Nudge)
                {
                    // Behind (dir 0) may only ever speed UP; ahead (dir 1) may only ever slow DOWN.
                    if (dir == 0 && s.speed < 1.0) sawWrongSign = true;
                    if (dir == 1 && s.speed > 1.0) sawWrongSign = true;
                }
                nudging = (s.action == DriftAction::Nudge);
                settledFor = (s.action == DriftAction::None) ? settledFor + 1 : 0;
                const double rate = (s.action == DriftAction::Nudge) ? s.speed : 1.0;
                host += 1.0;
                local += rate;
                const double mag = std::fabs(local - host);
                if (mag > prevMag + 1e-9) sawGrowth = true;
                prevMag = mag;
            }
            CHECK(!sawWrongSign);        // never nudges the wrong way: this is the non-oscillation property
            CHECK(!sawGrowth);           // and never makes it worse
            CHECK(!sawSeek);             // two seconds is the nudge band's job, not the seek's
            CHECK(settledFor > 250);     // converged, and stayed converged for the rest of the run
            CHECK(prevMag <= cfg.deadbandSec);
        }
    }

    // =========================================================== 5. the room, over the fake wire ============
    // Section 5 and section 6 share one drive: the transcript section 6 scans is the transcript this section
    // produces, so the scan is over the real traffic of a real session rather than a hand-made sample.
    Wire wire;
    Room host, guest;
    const QString kHostId = QStringLiteral("host-1"), kGuestId = QStringLiteral("guest-2");
    {
        host.open(QStringLiteral("kj72m"), kHostId, QStringLiteral("Living room"), true);
        guest.open(QStringLiteral("KJ-72M"), kGuestId, QStringLiteral("Sam"), false);
        CHECK(host.code() == QStringLiteral("KJ72M"));
        CHECK(guest.code() == host.code());          // the two spellings normalise to one room
        CHECK(host.isHost() && !guest.isHost());
        CHECK(host.participants().size() == 1);
        CHECK(host.hostPaused());                    // a room opens paused: nothing plays until the host says so

        // THE HOST IS HANDED A RESOLVED URL as the thing to share. This is the hostile case the feature is
        // built around, and it is set up here rather than in section 6 so every later message inherits it.
        PlayOn::ItemRef poisoned;
        poisoned.kind = QStringLiteral("catalog");
        poisoned.id = QStringLiteral("tt1375666");
        poisoned.type = QStringLiteral("movie");
        poisoned.title = QStringLiteral("Inception");
        poisoned.source = kStreamUrl;                // a caller putting the resolved link where the addon goes
        host.setItem(poisoned, 0.0, true);
        CHECK(host.item().id == QStringLiteral("tt1375666"));
        CHECK(host.item().source.isEmpty());         // scrubbed on the way IN, so it is never even held

        // The host starts playing, then a guest joins twenty minutes later.
        host.setHostTransport(false, 1200.0);
        CHECK(!host.hostPaused());

        wire.put(false, guest.helloMessage());
        wire.pump(host, guest);

        // JOIN IN PROGRESS: the guest knows what to play and lands at the host's position.
        CHECK(host.participants().size() == 2);
        CHECK(guest.participants().size() == 2);
        CHECK(guest.participants().at(0).host);      // host first, then join order
        CHECK(guest.participants().at(1).id == kGuestId);
        CHECK(guest.item().id == QStringLiteral("tt1375666"));
        CHECK(guest.item().kind == QStringLiteral("catalog"));
        CHECK(nearly(guest.hostPosition(), 1200.0));
        CHECK(!guest.hostPaused());
        // And the guest resolves its OWN stream from that reference: nothing it received is playable as-is.
        CHECK(guest.item().source.isEmpty());

        // WHAT before WHERE. A guest told the position first would seek a player still holding the old item.
        const QList<Message> answer = host.apply(guest.helloMessage());
        int itemAt = -1, xportAt = -1;
        for (int i = 0; i < answer.size(); ++i)
        {
            if (answer.at(i).type == MsgType::Item) itemAt = i;
            if (answer.at(i).type == MsgType::Transport) xportAt = i;
        }
        CHECK(itemAt >= 0 && xportAt >= 0 && itemAt < xportAt);
        // A rejoin is NOT a second row.
        CHECK(host.participants().size() == 2);
    }

    // ---- host arbitration of a guest's requests
    {
        // The guest asks for a pause. Its OWN state does not move until the host says so — that is what
        // "the host arbitrates" means, and it is the difference between a shared transport and two players
        // that happen to agree.
        const Message req = guest.requestPauseMessage(true);
        CHECK(!guest.hostPaused());
        wire.put(false, req);
        CHECK(!guest.hostPaused());                  // still nothing: the request is only on the wire
        wire.pump(host, guest);
        CHECK(host.hostPaused());
        CHECK(guest.hostPaused());                   // now, and only via the host's answer

        // A seek request, likewise, and clamped by the host.
        wire.put(false, guest.requestSeekMessage(1500.0));
        wire.pump(host, guest);
        CHECK(nearly(host.hostPosition(), 1500.0));
        CHECK(nearly(guest.hostPosition(), 1500.0));
        // The SENDER clamps too, so a negative position has to be put on the wire by hand to prove the HOST
        // clamps: an older or hostile peer is exactly the case the host-side clamp exists for.
        CHECK(nearly(guest.requestSeekMessage(-40.0).positionSec, 0.0));
        Message rawSeek = guest.requestSeekMessage(0.0);
        rawSeek.positionSec = -40.0;
        wire.put(false, rawSeek);
        wire.pump(host, guest);
        CHECK(nearly(host.hostPosition(), 0.0));     // clamped by the HOST, not negative
        CHECK(nearly(guest.hostPosition(), 0.0));

        // Back to somewhere sensible, playing.
        host.setHostTransport(false, 1500.0);
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);
        CHECK(!guest.hostPaused());

        // A request from someone who is not in the room at all changes nothing.
        Message stranger = guest.requestSeekMessage(9999.0);
        stranger.participantId = QStringLiteral("nobody");
        wire.put(false, stranger);
        wire.pump(host, guest);
        CHECK(nearly(host.hostPosition(), 1500.0));
    }

    // ---- a guest that cannot resolve stays in the room, and cannot steer it
    {
        // THE REPORT IS ONLY NEWS THE FIRST TIME, and this is the guard that stops a message loop: a guest
        // reports "I resolved it" by saying hello, the host answers a hello with the room's item, and the
        // item is what made the guest report. Unconditional, that is an infinite loop — it ran on live
        // hardware and re-opened the film every few seconds until this returned false.
        CHECK(guest.selfResolved());
        CHECK(!guest.setSelfResolved());                     // already resolved: nothing to say
        wire.put(false, guest.unresolvedMessage(QStringLiteral("no source has this film here")));
        CHECK(guest.setSelfUnresolved(QStringLiteral("no source has this film here")));
        CHECK(!guest.selfResolved());
        CHECK(!guest.setSelfUnresolved(QStringLiteral("no source has this film here")));   // and again is not
        wire.pump(host, guest);
        CHECK(host.participants().size() == 2);              // still here: not dropped
        CHECK(!host.participants().at(1).resolved);
        CHECK(host.participants().at(1).note == QStringLiteral("no source has this film here"));
        CHECK(guest.participants().size() == 2);
        CHECK(!guest.participants().at(1).resolved);         // and the roster says so on both screens

        // ...and the others are not blocked: the host is still playing.
        CHECK(!host.hostPaused());
        // Someone who could not get the film does not get to pause it for the people who did.
        wire.put(false, guest.requestPauseMessage(true));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());
        wire.put(false, guest.requestSeekMessage(10.0));
        wire.pump(host, guest);
        CHECK(nearly(host.hostPosition(), 1500.0));

        // ...and a stale stall report from someone who never got the film cannot hold the room up either.
        // (A guest that gave up may still have a buffering timer in flight; it must count for nothing.)
        CHECK(!host.hostPaused());
        wire.put(false, guest.bufferingMessage(true, 1500.0));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());
        CHECK(!host.heldForBuffering());
        wire.put(false, guest.bufferingMessage(false, 1500.0));
        wire.pump(host, guest);

        // It resolves after all (a second addon answered): it is a full participant again — and THAT is a
        // change, so it is worth a message.
        CHECK(guest.setSelfResolved());
        CHECK(guest.selfResolved());
        CHECK(!guest.setSelfResolved());
        wire.put(false, guest.helloMessage());
        wire.pump(host, guest);
        CHECK(host.participants().at(1).resolved);
        wire.put(false, guest.requestPauseMessage(true));
        wire.pump(host, guest);
        CHECK(host.hostPaused());
        host.setHostTransport(false, 1500.0);
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);
    }

    // ---- both buffering policies
    {
        // The pure decision table first, including the case that makes the feature honest.
        CHECK(decideBuffering(BufferPolicy::WaitForEveryone, true,  true,  false) == BufferAction::HoldForBuffering);
        CHECK(decideBuffering(BufferPolicy::WaitForEveryone, true,  true,  true)  == BufferAction::Nothing);
        CHECK(decideBuffering(BufferPolicy::WaitForEveryone, false, false, true)  == BufferAction::ResumeAfterBuffering);
        CHECK(decideBuffering(BufferPolicy::WaitForEveryone, false, true,  false) == BufferAction::Nothing);
        // A stall while the film is ALREADY paused by the user must not arm a hold — because the hold's
        // release would then resume a film the user paused on purpose.
        CHECK(decideBuffering(BufferPolicy::WaitForEveryone, true,  false, false) == BufferAction::Nothing);
        CHECK(decideBuffering(BufferPolicy::KeepGoing,       true,  true,  false) == BufferAction::Nothing);
        // Switching the policy to "keep going" while the room is held has to release it, or nothing ever will.
        CHECK(decideBuffering(BufferPolicy::KeepGoing,       true,  false, true)  == BufferAction::ResumeAfterBuffering);
        CHECK(policyId(BufferPolicy::WaitForEveryone) == QStringLiteral("wait"));
        CHECK(policyId(BufferPolicy::KeepGoing) == QStringLiteral("keepgoing"));
        CHECK(policyFromId(QStringLiteral("keepgoing")) == BufferPolicy::KeepGoing);
        CHECK(policyFromId(QStringLiteral("wait")) == BufferPolicy::WaitForEveryone);
        CHECK(policyFromId(QStringLiteral("hand-edited nonsense")) == BufferPolicy::WaitForEveryone);

        // WAIT FOR EVERYONE, over the wire.
        host.setBufferPolicy(BufferPolicy::WaitForEveryone);
        CHECK(!host.hostPaused());
        wire.put(false, guest.bufferingMessage(true, 1499.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused());                 // the room waits
        CHECK(host.heldForBuffering());
        CHECK(guest.hostPaused());
        CHECK(guest.participants().at(1).buffering);
        wire.put(false, guest.bufferingMessage(false, 1499.0));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());                // and resumes by itself
        CHECK(!host.heldForBuffering());
        CHECK(!guest.hostPaused());

        // A HOLD IS ARMED AND THEN THE USER PRESSES PAUSE. The hold has to be forgotten at that moment: it is
        // no longer this machine's pause to release, and releasing it would restart a film the user stopped.
        wire.put(false, guest.bufferingMessage(true, 1500.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused() && host.heldForBuffering());
        host.setHostTransport(true, 1500.0);            // the user presses pause, on top of the hold
        CHECK(!host.heldForBuffering());
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);
        wire.put(false, guest.bufferingMessage(false, 1500.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused());                       // the guest recovered; the user's pause stands
        host.setHostTransport(false, 1500.0);
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);
        CHECK(!host.hostPaused());

        // A hold must never resume a film the USER paused. Pause by hand, stall, recover: still paused.
        host.setHostTransport(true, 1500.0);
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);
        wire.put(false, guest.bufferingMessage(true, 1500.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused());
        CHECK(!host.heldForBuffering());          // paused, but not by the buffering machinery
        wire.put(false, guest.bufferingMessage(false, 1500.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused());                 // still paused, as the user left it
        host.setHostTransport(false, 1500.0);
        wire.put(true, host.transportMessage());
        wire.pump(host, guest);

        // KEEP GOING: the stall is reported and shown, and nothing stops.
        host.setBufferPolicy(BufferPolicy::KeepGoing);
        wire.put(false, guest.bufferingMessage(true, 1501.0));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());                // the room does not wait
        CHECK(host.participants().at(1).buffering);
        CHECK(guest.participants().at(1).buffering);   // but everyone can SEE the stall
        wire.put(false, guest.bufferingMessage(false, 1501.0));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());

        // Switching policy back while someone is stalled arms the hold at the next report, not retroactively.
        host.setBufferPolicy(BufferPolicy::WaitForEveryone);
        wire.put(false, guest.bufferingMessage(true, 1502.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused());
        wire.put(false, guest.bufferingMessage(false, 1502.0));
        wire.pump(host, guest);
        CHECK(!host.hostPaused());
    }

    // ---- leaving and rejoining
    {
        wire.put(false, guest.byeMessage());
        wire.pump(host, guest);
        CHECK(host.participants().size() == 1);
        CHECK(host.participants().at(0).id == kHostId);

        // A guest that leaves WHILE the room is held for its own stall must not leave it held for ever.
        wire.put(false, guest.helloMessage());
        wire.pump(host, guest);
        CHECK(host.participants().size() == 2);
        wire.put(false, guest.bufferingMessage(true, 1600.0));
        wire.pump(host, guest);
        CHECK(host.hostPaused() && host.heldForBuffering());
        wire.put(false, guest.byeMessage());
        wire.pump(host, guest);
        CHECK(host.participants().size() == 1);
        CHECK(!host.hostPaused());                // released by the departure
        CHECK(!host.heldForBuffering());

        // Rejoin: one row, not two, and it lands at the host's position again.
        host.setHostTransport(false, 2400.0);
        guest.open(QStringLiteral("KJ72M"), kGuestId, QStringLiteral("Sam"), false);
        wire.put(false, guest.helloMessage());
        wire.pump(host, guest);
        CHECK(host.participants().size() == 2);
        CHECK(nearly(guest.hostPosition(), 2400.0));
        CHECK(guest.item().id == QStringLiteral("tt1375666"));

        // A peer speaking a different protocol version is refused with a reason, not silently half-joined.
        Message oldHello = guest.helloMessage();
        oldHello.participantId = QStringLiteral("old-3");
        oldHello.protocol = kProtocol + 1;
        const QList<Message> refusal = host.apply(oldHello);
        CHECK(refusal.size() == 1);
        CHECK(refusal.at(0).type == MsgType::Bye);
        CHECK(!refusal.at(0).reason.isEmpty());
        CHECK(host.participants().size() == 2);   // not added
    }

    // =========================================================== 6. the credential byte-scan ================
    // The whole transcript of the session above — every byte either side would have written to a socket —
    // after a drive in which the host was deliberately handed a resolved, account-bearing stream url as the
    // thing to share. This is the assertion the feature exists to be able to make.
    {
        CHECK(wire.transcript.size() > 500);                 // there WAS a session; an empty scan proves nothing
        CHECK(!wire.transcript.contains(kSecret.toUtf8()));
        CHECK(!wire.transcript.contains("://"));             // no url of any scheme, anywhere
        CHECK(!wire.transcript.contains("apikey"));
        CHECK(!wire.transcript.contains("example-debrid"));
        CHECK(!wire.transcript.contains("Inception.2010.mkv"));
        // The identity it DID carry, so the scan is not passing because nothing was ever sent.
        CHECK(wire.transcript.contains("tt1375666"));
        CHECK(wire.transcript.contains("\"t\":\"beacon\"") || wire.transcript.contains("\"t\":\"item\""));

        // And the persisted record: the same rule about what is written to disk.
        const QVariantMap rec = roomRecord(host);
        CHECK(recordSafe(rec));
        CHECK(rec.value(QStringLiteral("refId")).toString() == QStringLiteral("tt1375666"));
        CHECK(rec.value(QStringLiteral("refSource")).toString().isEmpty());
        CHECK(rec.value(QStringLiteral("code")).toString() == QStringLiteral("KJ72M"));
        CHECK(rec.value(QStringLiteral("role")).toString() == QStringLiteral("host"));
        QByteArray flat;
        for (auto it = rec.constBegin(); it != rec.constEnd(); ++it)
            flat += it.key().toUtf8() + '=' + it.value().toString().toUtf8() + ';';
        CHECK(!flat.contains(kSecret.toUtf8()));
        CHECK(!flat.contains("://"));
        // recordSafe is not vacuously true: a record that HAS been poisoned is refused.
        QVariantMap poisonedRec = rec;
        poisonedRec.insert(QStringLiteral("refId"), kStreamUrl);
        CHECK(!recordSafe(poisonedRec));
        // The ini group, named here so the writer and this scan cannot drift.
        CHECK(QString::fromLatin1(kSettingsGroup) == QStringLiteral("watchtogether"));
    }

    if (failures == 0) std::printf("WATCHTOGETHER-OK\n");
    return failures == 0 ? 0 : 1;
}
