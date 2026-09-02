// Headless check of "Play on device" (issue #143) — src/core/PlayOnDevice.{h,cpp} plus the #143 additions to
// the pure #76 remote-control core (src/core/RemoteApi.{h,cpp}).
//
// What this pins, without a socket, a window or a second machine:
//
//   1. THE ADVERTISEMENT. txtRecords in its pinned order (name / ver / id); mdnsQuery against a HAND-WRITTEN
//      hex fixture of the exact datagram (not a round-trip through the builder); isServiceQuery telling a
//      question about our service from a question about _googlecast, from a RESPONSE, and from garbage;
//      parseAdvert against a packet this file assembles with its OWN record builder — an independent oracle,
//      so a bug shared between builder and parser cannot hide.
//   2. THE PICKER MERGE. Three target kinds in one list, EverythingBox peers first, "EverythingBox on <name>"
//      as the label, the paired flag, dedup by id, and THE RULE THAT MATTERS: an instance never lists itself.
//   3. THE HAND-OFF. handoffJson/parseHandoff round-trip; the payload is a REFERENCE and a position and
//      carries no URL; decideOpen's whole table including the 409 an unresolvable reference gets, and the
//      ORDER — the profile gate answers 403 before resolvability is even consulted, so a hand-off cannot be
//      used to probe what is behind a passcode wall.
//   4. PAIRING. A right code issues a token and consumes the offer; a wrong code is refused; three wrong
//      answers burn the offer. And the credential rule, asserted rather than assumed: the token never appears
//      in any string this unit produces for display or for the wire error bodies.
//   5. REMOTE MODE AND THE PULL. /state -> RemoteView; /state -> Pull; and their SYMMETRY — a pull re-derives
//      byte-for-byte the hand-off payload the peer would itself have sent.
//   6. THE ROUTES. POST /open and POST /pair through RemoteApi::route, the bearer credential off either
//      header spelling, and the ADDITIVE /state body: every key #76 shipped is still there.
//
// Prints PLAYON-OK on success; any failure prints PLAYON-FAIL <cond> (line) and exits non-zero.
#include "PlayOnDevice.h"
#include "RemoteApi.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PLAYON-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- the probe's OWN mDNS record builder ------------------------------------------------------------------
// Deliberately not PlayOn's. parseAdvert is asserted against packets built here, so a mistake made in
// mdnsResponse and repeated in parseAdvert (the classic round-trip blind spot) still shows up as a failure.
namespace oracle
{
    static void u16(QByteArray& b, int v) { b.append(char((v >> 8) & 0xFF)); b.append(char(v & 0xFF)); }
    static void u32(QByteArray& b, quint32 v)
    {
        b.append(char((v >> 24) & 0xFF)); b.append(char((v >> 16) & 0xFF));
        b.append(char((v >> 8) & 0xFF));  b.append(char(v & 0xFF));
    }
    static QByteArray name(const QByteArray& dotted)
    {
        QByteArray o;
        for (const QByteArray& l : dotted.split('.')) { if (l.isEmpty()) continue; o.append(char(l.size())); o += l; }
        o.append(char(0));
        return o;
    }
    static void rr(QByteArray& b, const QByteArray& owner, int type, int cls, const QByteArray& rdata)
    {
        b += name(owner); u16(b, type); u16(b, cls); u32(b, 120); u16(b, rdata.size()); b += rdata;
    }
    // A full EverythingBox advertisement, assembled here from the record layout the header documents.
    static QByteArray advert(const QByteArray& id, const QByteArray& friendly, const QByteArray& ver,
                             int port, quint32 ipv4)
    {
        const QByteArray svc  = "_everythingbox._tcp.local";
        const QByteArray inst = id + "." + svc;
        const QByteArray host = id + ".local";

        QByteArray txt;
        for (const QByteArray& s : { QByteArray("name=") + friendly, QByteArray("ver=") + ver,
                                     QByteArray("id=") + id })
        { txt.append(char(s.size())); txt += s; }

        QByteArray srv; u16(srv, 0); u16(srv, 0); u16(srv, port); srv += name(host);

        QByteArray p;
        u16(p, 0); u16(p, 0x8400);
        u16(p, 0); u16(p, ipv4 ? 4 : 3); u16(p, 0); u16(p, 0);
        rr(p, svc,  12, 1,      name(inst));
        rr(p, inst, 33, 0x8001, srv);
        rr(p, inst, 16, 0x8001, txt);
        if (ipv4) { QByteArray a; u32(a, ipv4); rr(p, host, 1, 0x8001, a); }
        return p;
    }
}

int main()
{
    using namespace PlayOn;

    // =========================================================== 1. the advertisement =======================
    {
        Advert a;
        a.instanceId = QStringLiteral("dev-abc");
        a.name       = QStringLiteral("Den TV");
        a.version    = QStringLiteral("0.9.1");
        a.port       = 8090;

        CHECK(advertValid(a));
        const QList<QByteArray> txt = txtRecords(a);
        CHECK(txt.size() == 3);
        CHECK(txt.value(0) == QByteArray("name=Den TV"));   // the order is the wire format
        CHECK(txt.value(1) == QByteArray("ver=0.9.1"));
        CHECK(txt.value(2) == QByteArray("id=dev-abc"));

        Advert noPort = a; noPort.port = 0;
        CHECK(!advertValid(noPort));                        // an advert nothing can connect to is not one
        Advert noId = a; noId.instanceId.clear();
        CHECK(!advertValid(noId));
        CHECK(mdnsResponse(noPort, 0).isEmpty());           // and it produces no packet at all
    }

    // The query datagram, byte for byte, against a fixture typed out from the DNS wire format by hand.
    {
        const QByteArray expected = QByteArray::fromHex(
            "000000000001000000000000"                       // id, flags(query), qd=1, an/ns/ar=0
            "0e" "5f65766572797468696e67626f78"              // "_everythingbox"
            "04" "5f746370"                                  // "_tcp"
            "05" "6c6f63616c"                                // "local"
            "00"                                             // end of name
            "000c" "0001");                                  // QTYPE=PTR, QCLASS=IN
        CHECK(mdnsQuery() == expected);
        CHECK(mdnsQuery().size() == 12 + 27 + 4);
    }

    // isServiceQuery: only a QUESTION, and only about our service.
    {
        CHECK(isServiceQuery(mdnsQuery()));

        // Same shape, someone else's service.
        QByteArray other = QByteArray::fromHex(
            "000000000001000000000000"
            "0b" "5f676f6f676c6563617374"                    // "_googlecast"
            "04" "5f746370" "05" "6c6f63616c" "00" "000c" "0001");
        CHECK(!isServiceQuery(other));

        // Our service, but a RESPONSE (QR set) -- an answer must never be answered.
        CHECK(!isServiceQuery(oracle::advert("dev-x", "X", "1", 8090, 0xC0A80132u)));

        // The response that actually catches a missing QR check: many responders ECHO the question back in
        // the question section, so the packet carries qd=1 with OUR name AND the QR bit. Reading only the
        // question section, this looks exactly like something to answer -- and answering it is a multicast
        // storm between two instances, each replying to the other's replies for ever.
        QByteArray echoed = mdnsQuery();
        echoed[2] = char(0x84);                              // QR + AA, question section left in place
        CHECK(!isServiceQuery(echoed));
        echoed[2] = char(0x00);
        CHECK(isServiceQuery(echoed));                        // and it IS a question again with the bit clear

        CHECK(!isServiceQuery(QByteArray()));
        CHECK(!isServiceQuery(QByteArray("garbage")));
    }

    // parseAdvert, against the oracle's packet.
    {
        const QByteArray pkt = oracle::advert("dev-abc", "Den TV", "0.9.1", 8090, 0xC0A80132u); // 192.168.1.50
        Peer p;
        CHECK(parseAdvert(pkt, 0, p));
        CHECK(p.id == QStringLiteral("dev-abc"));
        CHECK(p.name == QStringLiteral("Den TV"));
        CHECK(p.version == QStringLiteral("0.9.1"));
        CHECK(p.port == 8090);
        CHECK(p.host == QStringLiteral("192.168.1.50"));
    }

    // No A record: fall back to the datagram's sender address; with neither, refuse.
    {
        const QByteArray pkt = oracle::advert("dev-abc", "Den TV", "0.9.1", 8090, 0);
        Peer p;
        CHECK(parseAdvert(pkt, 0x0A000005u, p));             // 10.0.0.5
        CHECK(p.host == QStringLiteral("10.0.0.5"));
        Peer q;
        CHECK(!parseAdvert(pkt, 0, q));                      // nothing to connect to -> not a peer
    }

    // A question is not an advertisement, and neither is an empty packet.
    {
        Peer p;
        CHECK(!parseAdvert(mdnsQuery(), 0x0A000005u, p));
        CHECK(!parseAdvert(QByteArray(), 0x0A000005u, p));
    }

    // What PlayOn itself emits is parseable — and carries the same facts.
    {
        Advert a; a.instanceId = QStringLiteral("dev-9"); a.name = QStringLiteral("Kitchen");
        a.version = QStringLiteral("2.0"); a.port = 9001;
        Peer p;
        CHECK(parseAdvert(mdnsResponse(a, 0xC0A80105u), 0, p));
        CHECK(p.id == QStringLiteral("dev-9"));
        CHECK(p.name == QStringLiteral("Kitchen"));
        CHECK(p.version == QStringLiteral("2.0"));
        CHECK(p.port == 9001);
        CHECK(p.host == QStringLiteral("192.168.1.5"));
        // The response is an ANSWER: the responder must not treat its own packet as a question to answer.
        CHECK(!isServiceQuery(mdnsResponse(a, 0xC0A80105u)));
    }

    // =========================================================== 2. the picker merge ========================
    {
        QList<Target> cast;
        Target cc; cc.kind = TargetKind::Chromecast; cc.id = QStringLiteral("cc:192.168.1.9");
        cc.name = QStringLiteral("Living Room TV"); cc.label = cc.name; cast << cc;
        Target dl; dl.kind = TargetKind::Dlna; dl.id = QStringLiteral("dlna:192.168.1.7");
        dl.name = QStringLiteral("Hi-Fi"); dl.label = dl.name; cast << dl;

        QList<Peer> peers;
        Peer me;   me.id = QStringLiteral("self");  me.name = QStringLiteral("This Box"); me.host = QStringLiteral("192.168.1.2"); me.port = 8090;
        Peer them; them.id = QStringLiteral("dev-b"); them.name = QStringLiteral("Bedroom"); them.host = QStringLiteral("192.168.1.3"); them.port = 8090;
        Peer dup;  dup = them;                        // the same peer heard twice (mDNS bursts repeat)
        Peer anon; anon.id.clear(); anon.name = QStringLiteral("?"); anon.host = QStringLiteral("192.168.1.4"); anon.port = 8090;
        peers << me << them << dup << anon;

        QSet<QString> paired; paired.insert(QStringLiteral("dev-b"));
        const QList<Target> merged = mergeTargets(cast, peers, QStringLiteral("self"), paired);

        CHECK(merged.size() == 3);                            // self dropped, the duplicate collapsed, anon dropped
        CHECK(merged.value(0).kind == TargetKind::EverythingBox);   // peers first
        CHECK(merged.value(0).id == QStringLiteral("eb:dev-b"));
        CHECK(merged.value(0).label == QStringLiteral("EverythingBox on Bedroom"));
        CHECK(merged.value(0).paired);
        CHECK(merged.value(0).port == 8090);
        CHECK(merged.value(1).kind == TargetKind::Chromecast);
        CHECK(merged.value(2).kind == TargetKind::Dlna);
        for (const Target& t : merged) CHECK(t.id != QStringLiteral("eb:self"));   // never itself

        // An unpaired peer is still listed — it is how pairing gets started; it is just not marked paired.
        const QList<Target> nonePaired = mergeTargets(cast, peers, QStringLiteral("self"), QSet<QString>());
        CHECK(nonePaired.size() == 3);
        CHECK(!nonePaired.value(0).paired);

        // With no self id known (an install whose device id has not been minted yet) nothing is excluded --
        // but the id namespace still separates a peer from a cast device.
        const QList<Target> noSelf = mergeTargets(cast, peers, QString(), paired);
        CHECK(noSelf.size() == 4);
        CHECK(peerTargetId(QStringLiteral("x")) == QStringLiteral("eb:x"));
        CHECK(peerLabel(QStringLiteral("Loft")) == QStringLiteral("EverythingBox on Loft"));
    }

    // =========================================================== 3. the hand-off ============================
    Handoff sent;
    sent.ref.kind   = QStringLiteral("local");
    sent.ref.id     = QStringLiteral("lib:track:9931");
    sent.ref.type   = QStringLiteral("music");
    sent.ref.title  = QStringLiteral("Take Five");
    sent.ref.source = QString();
    sent.positionSec   = 63.5;
    sent.audioTrack    = QStringLiteral("1");
    sent.subtitleTrack = QString();

    {
        const QByteArray json = handoffJson(sent);
        // The whole point of the contract: a reference and a position, never bytes and never a URL.
        CHECK(!json.contains("http"));
        CHECK(!json.contains("file:"));
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        CHECK(o.value(QStringLiteral("position")).toDouble() == 63.5);
        CHECK(o.value(QStringLiteral("ref")).toObject().value(QStringLiteral("id")).toString()
              == QStringLiteral("lib:track:9931"));
        CHECK(o.value(QStringLiteral("tracks")).toObject().value(QStringLiteral("audio")).toString()
              == QStringLiteral("1"));

        Handoff back; QString err;
        CHECK(parseHandoff(json, back, err));
        CHECK(err.isEmpty());
        CHECK(back.ref.kind == sent.ref.kind);
        CHECK(back.ref.id == sent.ref.id);
        CHECK(back.ref.type == sent.ref.type);
        CHECK(back.ref.title == sent.ref.title);
        CHECK(back.positionSec == sent.positionSec);
        CHECK(back.audioTrack == sent.audioTrack);
        CHECK(handoffJson(back) == json);                    // a re-send is byte-identical
    }

    // Malformed payloads are refused with a reason, never half-accepted.
    {
        Handoff h; QString err;
        CHECK(!parseHandoff(QByteArray("not json"), h, err));
        CHECK(!err.isEmpty());
        err.clear();
        CHECK(!parseHandoff(QByteArray("{\"position\":10}"), h, err));   // no ref at all
        CHECK(!err.isEmpty());
        err.clear();
        CHECK(!parseHandoff(QByteArray("[]"), h, err));
    }

    // A negative resume point is clamped rather than carried into a seek.
    {
        Handoff h; QString err;
        CHECK(parseHandoff(QByteArray("{\"ref\":{\"kind\":\"local\",\"id\":\"x\"},\"position\":-5}"), h, err));
        CHECK(h.positionSec == 0.0);
    }

    // ---- decideOpen: the whole table ----
    {
        OpenEnv have; have.localIdKnown = true;
        const OpenResult ok = decideOpen(sent, have);
        CHECK(ok.outcome == OpenOutcome::Accepted);
        CHECK(ok.httpStatus == 200);
        CHECK(ok.reason.isEmpty());
        CHECK(describeRefusal(ok, QStringLiteral("Bedroom")).isEmpty());
    }
    {
        // The headline 409: a local-only file the target does not have.
        OpenEnv bare;                                        // localIdKnown false
        const OpenResult r = decideOpen(sent, bare);
        CHECK(r.outcome == OpenOutcome::Unresolvable);
        CHECK(r.httpStatus == 409);
        CHECK(!r.reason.isEmpty());
        const QString shown = describeRefusal(r, QStringLiteral("Bedroom"));
        CHECK(shown.startsWith(QStringLiteral("Not available on Bedroom")));
        const QJsonObject body = QJsonDocument::fromJson(openResultJson(r)).object();
        CHECK(body.value(QStringLiteral("ok")).toBool() == false);
        CHECK(body.value(QStringLiteral("status")).toInt() == 409);
        CHECK(body.value(QStringLiteral("reason")).toString() == r.reason);
    }
    {
        Handoff cat = sent; cat.ref.kind = QStringLiteral("catalog"); cat.ref.id = QStringLiteral("tt1375666");
        OpenEnv none;
        CHECK(decideOpen(cat, none).httpStatus == 409);       // no source could resolve it
        OpenEnv withAddons; withAddons.addonsAvailable = true;
        CHECK(decideOpen(cat, withAddons).outcome == OpenOutcome::Accepted);
        // localIdKnown must not rescue a catalog reference, and addons must not rescue a local one.
        OpenEnv onlyLocal; onlyLocal.localIdKnown = true;
        CHECK(decideOpen(cat, onlyLocal).httpStatus == 409);
        Handoff loc = sent;
        CHECK(decideOpen(loc, withAddons).httpStatus == 409);
    }
    {
        Handoff srv = sent; srv.ref.kind = QStringLiteral("server"); srv.ref.source = QStringLiteral("home");
        OpenEnv none;
        CHECK(decideOpen(srv, none).httpStatus == 409);
        OpenEnv signedIn; signedIn.serverConfigured = true;
        CHECK(decideOpen(srv, signedIn).outcome == OpenOutcome::Accepted);
    }
    {
        Handoff bad = sent; bad.ref.id.clear();
        CHECK(decideOpen(bad, OpenEnv{}).httpStatus == 400);
        Handoff weird = sent; weird.ref.kind = QStringLiteral("magnet");
        CHECK(decideOpen(weird, OpenEnv{}).httpStatus == 400);
        CHECK(decideOpen(weird, OpenEnv{}).outcome == OpenOutcome::BadRequest);
    }
    {
        // The restricted-profile target. THE ORDERING ASSERTION: the gate answers 403 whether or not the
        // target could have resolved the item, so the answer leaks nothing about what is behind the wall.
        OpenEnv walled; walled.localIdKnown = true; walled.profileBlocks = true;
        walled.blockReason = QStringLiteral("this profile is limited to PG");
        const OpenResult r = decideOpen(sent, walled);
        CHECK(r.outcome == OpenOutcome::Gated);
        CHECK(r.httpStatus == 403);
        CHECK(r.reason == QStringLiteral("this profile is limited to PG"));
        CHECK(describeRefusal(r, QStringLiteral("Bedroom")).startsWith(QStringLiteral("Blocked on Bedroom")));

        OpenEnv walledAndMissing; walledAndMissing.profileBlocks = true;   // could NOT have resolved it either
        const OpenResult r2 = decideOpen(sent, walledAndMissing);
        CHECK(r2.httpStatus == 403);                          // 403, not 409 -- same answer either way
        OpenEnv walledNoReason; walledNoReason.localIdKnown = true; walledNoReason.profileBlocks = true;
        CHECK(!decideOpen(sent, walledNoReason).reason.isEmpty());   // always says something
    }

    // =========================================================== 4. pairing =================================
    {
        CHECK(pairingCode(0) == QStringLiteral("000000"));    // zero-padded to six
        CHECK(pairingCode(42) == QStringLiteral("000042"));
        CHECK(pairingCode(1234567u) == QStringLiteral("234567"));
        CHECK(pairingCode(4294967295u).size() == 6);
        CHECK(normalizeCode(QStringLiteral(" 123-456 ")) == QStringLiteral("123456"));
        CHECK(codeMatches(QStringLiteral("123456"), QStringLiteral("123 456")));
        CHECK(codeMatches(QStringLiteral("123456"), QStringLiteral("123-456")));
        CHECK(!codeMatches(QStringLiteral("123456"), QStringLiteral("123457")));
        CHECK(!codeMatches(QString(), QString()));            // no offer armed matches nothing
    }

    const QByteArray tokenEntropy = QByteArray("entropy-for-the-token-fixture");
    QString issued;
    {
        Pairing p;
        CHECK(!p.pending());
        CHECK(p.redeem(QStringLiteral("000000"), tokenEntropy).isEmpty());   // nothing to redeem

        const QString code = p.begin(3141592u);
        CHECK(code == pairingCode(3141592u));
        CHECK(p.pending());
        CHECK(p.attemptsLeft() == Pairing::kMaxAttempts);

        CHECK(p.redeem(QStringLiteral("000001"), tokenEntropy).isEmpty());   // wrong
        CHECK(p.attemptsLeft() == Pairing::kMaxAttempts - 1);
        CHECK(p.pending());

        issued = p.redeem(code, tokenEntropy);
        CHECK(!issued.isEmpty());
        CHECK(!p.pending());                                  // one-shot: the offer is consumed
        CHECK(p.redeem(code, tokenEntropy).isEmpty());        // and cannot be replayed
    }
    {
        Pairing p;
        const QString code = p.begin(777777u);
        for (int i = 0; i < Pairing::kMaxAttempts; ++i)
            CHECK(p.redeem(QStringLiteral("999999"), tokenEntropy).isEmpty());
        CHECK(!p.pending());                                  // burned; the code must be re-shown
        CHECK(p.redeem(code, tokenEntropy).isEmpty());        // even the RIGHT code is dead now
    }
    {
        CHECK(issued.size() == 64);                           // sha256 hex
        for (const QChar c : issued) CHECK(c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')));
        CHECK(mintToken(tokenEntropy) == issued);             // deterministic in its entropy
        CHECK(mintToken(QByteArray("something else")) != issued);
        CHECK(issued != QString::fromLatin1(tokenEntropy));   // never the caller's bytes verbatim
        CHECK(!issued.contains(QString::fromLatin1(tokenEntropy)));

        CHECK(tokenKey(QStringLiteral("dev-b")) == QStringLiteral("playon/peers/dev-b/token"));
        CHECK(PlayOn::isDeviceLocalKey(tokenKey(QStringLiteral("dev-b"))));
        CHECK(PlayOn::isDeviceLocalKey(QStringLiteral("playon/enabled")));
        CHECK(!PlayOn::isDeviceLocalKey(QStringLiteral("player/external")));
    }

    // =========================================================== 5. auth ====================================
    {
        CHECK(routeNeedsToken(QStringLiteral("/open")));
        CHECK(!routeNeedsToken(QStringLiteral("/pair")));     // /pair is HOW a token is obtained
        CHECK(!routeNeedsToken(QStringLiteral("/state")));    // #76's surface keeps its posture
        CHECK(!routeNeedsToken(QStringLiteral("/player")));
        CHECK(!routeNeedsToken(QStringLiteral("/input")));

        QSet<QString> live; live.insert(issued);
        CHECK(authorized(issued, live));
        CHECK(!authorized(QString(), live));
        CHECK(!authorized(QStringLiteral("deadbeef"), live));
        CHECK(!authorized(issued, QSet<QString>()));          // a target that has paired with nobody

        const QJsonObject u = QJsonDocument::fromJson(unauthorizedJson()).object();
        CHECK(u.value(QStringLiteral("status")).toInt() == 401);
        CHECK(u.value(QStringLiteral("ok")).toBool() == false);
    }

    // THE CREDENTIAL RULE, asserted rather than assumed: nothing this unit produces for a human or for the
    // wire carries the token.
    {
        OpenResult r; r.outcome = OpenOutcome::Unresolvable; r.httpStatus = 409; r.reason = QStringLiteral("nope");
        CHECK(!QString::fromUtf8(openResultJson(r)).contains(issued));
        CHECK(!QString::fromUtf8(unauthorizedJson()).contains(issued));
        CHECK(!describeRefusal(r, QStringLiteral("Bedroom")).contains(issued));
        CHECK(!QString::fromUtf8(handoffJson(sent)).contains(issued));
    }

    // =========================================================== 6. remote mode =============================
    {
        RemoteApi::PlayerStateView s;
        s.hasMedia = true; s.playing = true; s.title = QStringLiteral("Take Five");
        s.positionSec = 63.5; s.durationSec = 324.0; s.volume = 70; s.screen = QStringLiteral("player");
        s.volumeControllable = true;
        s.refKind = sent.ref.kind; s.refId = sent.ref.id; s.refType = sent.ref.type;
        s.refTitle = sent.ref.title; s.refSource = sent.ref.source;
        s.audioTrack = sent.audioTrack; s.subtitleTrack = sent.subtitleTrack;
        const QByteArray state = RemoteApi::stateJson(s);

        const RemoteView v = remoteView(state, true);
        CHECK(v.reachable);
        CHECK(v.hasMedia);
        CHECK(v.playing);
        CHECK(v.title == QStringLiteral("Take Five"));
        CHECK(v.positionSec == 63.5);
        CHECK(v.durationSec == 324.0);
        CHECK(v.volume == 70);
        CHECK(v.volumeControllable);

        // Unreachable: reachable false and everything else at rest, so the UI cannot render a stale position
        // as if it were live.
        const RemoteView dead = remoteView(state, false);
        CHECK(!dead.reachable);
        CHECK(!dead.hasMedia);
        CHECK(dead.positionSec == 0.0);
        CHECK(dead.title.isEmpty());

        const RemoteView junk = remoteView(QByteArray("<html>"), true);
        CHECK(!junk.reachable);

        // A target that does not report a volume is NOT offered one.
        RemoteApi::PlayerStateView noVol = s; noVol.volumeControllable = false;
        CHECK(!remoteView(RemoteApi::stateJson(noVol), true).volumeControllable);

        // ---- 7. the pull, and its symmetry with the hand-off ----
        const Pull p = continueHere(state, true);
        CHECK(p.valid);
        CHECK(p.reason.isEmpty());
        CHECK(p.ref.id == sent.ref.id);
        CHECK(p.positionSec == sent.positionSec);
        CHECK(p.audioTrack == sent.audioTrack);
        // The assertion the whole "opposite direction" claim rests on: taking over from a peer produces
        // EXACTLY the payload that peer would have handed us.
        CHECK(handoffJson(handoffFromPull(p)) == handoffJson(sent));

        const Pull unreachable = continueHere(state, false);
        CHECK(!unreachable.valid);
        CHECK(!unreachable.reason.isEmpty());

        RemoteApi::PlayerStateView idle = s; idle.hasMedia = false;
        const Pull nothing = continueHere(RemoteApi::stateJson(idle), true);
        CHECK(!nothing.valid);
        CHECK(!nothing.reason.isEmpty());

        // Playing, but unable to name what: refused rather than guessed at from the title.
        RemoteApi::PlayerStateView anon = s; anon.refKind.clear(); anon.refId.clear();
        const Pull blind = continueHere(RemoteApi::stateJson(anon), true);
        CHECK(!blind.valid);
        RemoteApi::PlayerStateView halfNamed = s; halfNamed.refId.clear();
        CHECK(!continueHere(RemoteApi::stateJson(halfNamed), true).valid);

        CHECK(!continueHere(QByteArray("nope"), true).valid);
    }

    // The /player bodies.
    {
        const QJsonObject a = QJsonDocument::fromJson(playerCommandBody(QStringLiteral("playpause"))).object();
        CHECK(a.value(QStringLiteral("action")).toString() == QStringLiteral("playpause"));
        const QJsonObject sk = QJsonDocument::fromJson(seekCommandBody(120.25)).object();
        CHECK(sk.value(QStringLiteral("action")).toString() == QStringLiteral("seek"));
        CHECK(sk.value(QStringLiteral("pos")).toDouble() == 120.25);
        CHECK(QJsonDocument::fromJson(seekCommandBody(-4.0)).object()
                  .value(QStringLiteral("pos")).toDouble() == 0.0);
        const QJsonObject vl = QJsonDocument::fromJson(volumeCommandBody(140)).object();
        CHECK(vl.value(QStringLiteral("level")).toInt() == 100);   // clamped to the UI range
        CHECK(QJsonDocument::fromJson(volumeCommandBody(-9)).object()
                  .value(QStringLiteral("level")).toInt() == 0);

        // Every body this unit builds must route back through #76's own decision table -- the two halves of
        // the remote cannot be allowed to drift into different vocabularies.
        for (const char* verb : { "play", "pause", "playpause", "stop", "next", "prev" })
        {
            const QByteArray body = playerCommandBody(QString::fromLatin1(verb));
            const QByteArray raw = "POST /player HTTP/1.1\r\nContent-Length: "
                                   + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            const RemoteApi::Command c = RemoteApi::route(RemoteApi::parseRequest(raw));
            CHECK(c.kind == RemoteApi::CommandKind::Player);
            CHECK(c.player != RemoteApi::PlayerAction::None);
        }
        {
            const QByteArray body = seekCommandBody(90.0);
            const QByteArray raw = "POST /player HTTP/1.1\r\nContent-Length: "
                                   + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            const RemoteApi::Command c = RemoteApi::route(RemoteApi::parseRequest(raw));
            CHECK(c.player == RemoteApi::PlayerAction::Seek);
            CHECK(!c.seekRelative);
            CHECK(c.seekSeconds == 90.0);
        }
    }

    // =========================================================== 8. the routes ==============================
    {
        const QByteArray body = handoffJson(sent);
        const QByteArray raw = "POST /open HTTP/1.1\r\nContent-Length: " + QByteArray::number(body.size())
                               + "\r\nAuthorization: Bearer " + issued.toLatin1() + "\r\n\r\n" + body;
        const RemoteApi::Request req = RemoteApi::parseRequest(raw);
        CHECK(req.valid);
        CHECK(req.path == QStringLiteral("/open"));
        CHECK(req.token == issued);                            // the credential is picked up...
        CHECK(RemoteApi::route(req).kind == RemoteApi::CommandKind::Open);

        Handoff got; QString err;
        CHECK(parseHandoff(req.body, got, err));               // ...and the body is a hand-off
        CHECK(got.ref.id == sent.ref.id);
    }
    {
        // The other header spelling, and a lowercase scheme.
        const RemoteApi::Request a = RemoteApi::parseRequest(
            "POST /open HTTP/1.1\r\nX-EB-Token: abc123\r\nContent-Length: 2\r\n\r\n{}");
        CHECK(a.token == QStringLiteral("abc123"));
        const RemoteApi::Request b = RemoteApi::parseRequest(
            "POST /open HTTP/1.1\r\nauthorization: bearer abc123\r\nContent-Length: 2\r\n\r\n{}");
        CHECK(b.token == QStringLiteral("abc123"));
        const RemoteApi::Request c = RemoteApi::parseRequest("GET /state HTTP/1.1\r\n\r\n");
        CHECK(c.token.isEmpty());
    }
    {
        CHECK(RemoteApi::route(RemoteApi::parseRequest("GET /open HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::BadRequest);          // POST only
        CHECK(RemoteApi::route(RemoteApi::parseRequest("POST /open HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::BadRequest);          // an /open with no reference
    }
    {
        const RemoteApi::Command begin = RemoteApi::route(RemoteApi::parseRequest("POST /pair HTTP/1.1\r\n\r\n"));
        CHECK(begin.kind == RemoteApi::CommandKind::PairBegin);
        CHECK(begin.pairCode.isEmpty());

        const RemoteApi::Command redeem =
            RemoteApi::route(RemoteApi::parseRequest("POST /pair?code=123456 HTTP/1.1\r\n\r\n"));
        CHECK(redeem.kind == RemoteApi::CommandKind::PairRedeem);
        CHECK(redeem.pairCode == QStringLiteral("123456"));

        const QByteArray jb = "{\"code\":\"654321\"}";
        const RemoteApi::Command viaBody = RemoteApi::route(RemoteApi::parseRequest(
            "POST /pair HTTP/1.1\r\nContent-Length: " + QByteArray::number(jb.size()) + "\r\n\r\n" + jb));
        CHECK(viaBody.kind == RemoteApi::CommandKind::PairRedeem);
        CHECK(viaBody.pairCode == QStringLiteral("654321"));

        CHECK(RemoteApi::route(RemoteApi::parseRequest("POST /pair?code= HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::BadRequest);
        CHECK(RemoteApi::route(RemoteApi::parseRequest("GET /pair HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::BadRequest);
    }
    {
        // #76's three routes are untouched by any of this.
        CHECK(RemoteApi::route(RemoteApi::parseRequest("GET /state HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::State);
        CHECK(RemoteApi::route(RemoteApi::parseRequest("GET /nope HTTP/1.1\r\n\r\n")).kind
              == RemoteApi::CommandKind::NotFound);
        CHECK(QByteArray(RemoteApi::reasonPhrase(401)) == "Unauthorized");
        CHECK(QByteArray(RemoteApi::reasonPhrase(409)) == "Conflict");
    }
    {
        // The /state body stayed ADDITIVE: #76's phone remote reads these seven keys and must keep working.
        RemoteApi::PlayerStateView s;
        s.hasMedia = true; s.playing = false; s.title = QStringLiteral("T");
        s.positionSec = 1.0; s.durationSec = 2.0; s.volume = 5; s.screen = QStringLiteral("player");
        const QJsonObject o = QJsonDocument::fromJson(RemoteApi::stateJson(s)).object();
        for (const char* k : { "hasMedia", "playing", "title", "position", "duration", "volume", "screen" })
            CHECK(o.contains(QString::fromLatin1(k)));
        CHECK(o.contains(QStringLiteral("item")));
        CHECK(o.contains(QStringLiteral("tracks")));
        CHECK(o.contains(QStringLiteral("volumeControllable")));
    }

    if (failures == 0) std::printf("PLAYON-OK\n");
    else               std::fprintf(stderr, "PLAYON had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
