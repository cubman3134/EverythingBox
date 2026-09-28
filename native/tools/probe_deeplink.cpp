// Headless coverage for everythingbox:// deep links (issue #80, proposal 1).
//
// A deep link is an unauthenticated install vector, so what is pinned here is exactly what such a link can and
// cannot make the app do before a person presses Install:
//   1. the parser's accept / refuse table — https only, no userinfo, /manifest.json only, the 2 KiB cap, the
//      `addon` verb only, and percent-decoding EXACTLY ONCE (a double-encoded scheme or host is refused);
//   2. the handoff message between a second process and the running one, and that the receiver re-validates
//      what it is sent — pure frames first, then a real QLocalServer round trip on a name of its own;
//   3. the per-user, per-session handoff name;
//   4. "a second link replaces the open card" — one card at a time, and an Install press counts only on the
//      card that is open now;
//   5. the Windows registration as a PURE list of (key, value) operations, and the Linux .desktop entry. The
//      probe never touches the registry or ~/.local: it tests the plan, not the machine;
//   6. the confirmation card's words.
//
// Prints DEEPLINK-OK on success; any failure prints DEEPLINK-FAIL and exits non-zero.
#include "DeepLink.h"
#include "DeepLinkChannel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <cstring>
#include <functional>

using DeepLink::Refusal;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const QString& what)
{
    if (ok) { ++g_pass; return; }
    ++g_fail;
    std::printf("  FAILED: %s\n", qPrintable(what));
}

static QString refusalName(Refusal r) { return QString::fromLatin1(DeepLink::refusalCode(r)); }

static void expectAccept(const QString& link, const QString& want, const QString& label)
{
    const DeepLink::Result r = DeepLink::parse(link);
    check(r.ok() && r.manifestUrl == want,
          QStringLiteral("accept %1: got %2 / %3").arg(label, refusalName(r.refusal), r.manifestUrl));
}

static void expectRefuse(const QString& link, Refusal want, const QString& label)
{
    const DeepLink::Result r = DeepLink::parse(link);
    check(!r.ok() && r.refusal == want && r.manifestUrl.isEmpty(),
          QStringLiteral("refuse %1: want %2, got %3 (%4)").arg(label, refusalName(want), refusalName(r.refusal),
                                                               r.manifestUrl));
}

// Percent-encode a whole string the way a web page building the link would (encodeURIComponent).
static QString enc(const QString& s) { return QString::fromLatin1(QUrl::toPercentEncoding(s)); }

static QString link(const QString& manifestUrl) { return QStringLiteral("everythingbox://addon/") + enc(manifestUrl); }

// ---- 1. the parser ----------------------------------------------------------------------------------------
static void parserTable()
{
    std::printf("-- parser --\n");
    const QString m = QStringLiteral("https://torrentio.strem.fun/manifest.json");
    expectAccept(link(m), m, QStringLiteral("plain encoded https manifest"));
    // A configured add-on's options segment: its own %7C survives the ONE decode untouched.
    const QString cfg = QStringLiteral("https://addon.example.org:8443/providers=yts%7Ceztv%7Csort=qs/manifest.json");
    expectAccept(link(cfg), cfg, QStringLiteral("options segment keeps its own percent-escapes"));
    expectAccept(QStringLiteral("EverythingBox://ADDON/") + enc(m), m, QStringLiteral("scheme and verb case"));
    expectAccept(QStringLiteral("everythingbox://addon/") + m, m, QStringLiteral("unencoded payload"));

    // Schemes inside the payload other than https.
    expectRefuse(link(QStringLiteral("http://torrentio.strem.fun/manifest.json")), Refusal::NotHttps,
                 QStringLiteral("http"));
    expectRefuse(link(QStringLiteral("javascript:alert(1)//manifest.json")), Refusal::NotHttps,
                 QStringLiteral("javascript:"));
    expectRefuse(link(QStringLiteral("file:///C:/Users/x/manifest.json")), Refusal::NotHttps, QStringLiteral("file:"));
    expectRefuse(link(QStringLiteral("data:application/json,{}#/manifest.json")), Refusal::NotHttps,
                 QStringLiteral("data:"));
    expectRefuse(link(QStringLiteral("ftp://host/manifest.json")), Refusal::NotHttps, QStringLiteral("ftp"));
    expectRefuse(link(QStringLiteral("//host/manifest.json")), Refusal::NotHttps, QStringLiteral("scheme-relative"));
    expectRefuse(link(QStringLiteral("stremio://host/manifest.json")), Refusal::NotHttps,
                 QStringLiteral("stremio:// inside the payload"));

    // Userinfo.
    expectRefuse(link(QStringLiteral("https://user:pw@host.example/manifest.json")), Refusal::Userinfo,
                 QStringLiteral("user:pass@"));
    expectRefuse(link(QStringLiteral("https://trusted.example@evil.example/manifest.json")), Refusal::Userinfo,
                 QStringLiteral("host-looking userinfo"));

    // Not a manifest.
    expectRefuse(link(QStringLiteral("https://host/manifest.json?x=1")), Refusal::NotManifest, QStringLiteral("query"));
    expectRefuse(link(QStringLiteral("https://host/manifest.json#x")), Refusal::NotManifest, QStringLiteral("fragment"));
    expectRefuse(link(QStringLiteral("https://host/config.json")), Refusal::NotManifest, QStringLiteral("other file"));
    expectRefuse(link(QStringLiteral("https://host/manifest.json.bak")), Refusal::NotManifest, QStringLiteral(".bak"));
    expectRefuse(link(QStringLiteral("https://host/xmanifest.json")), Refusal::NotManifest,
                 QStringLiteral("segment merely ending in manifest.json"));
    expectRefuse(link(QStringLiteral("https://host/Manifest.JSON")), Refusal::NotManifest, QStringLiteral("case"));
    expectRefuse(link(QStringLiteral("https://host/manifest.json/")), Refusal::NotManifest, QStringLiteral("trailing /"));
    expectRefuse(link(QStringLiteral("https://host")), Refusal::NotManifest, QStringLiteral("no path"));

    // Host.
    expectRefuse(link(QStringLiteral("https:///manifest.json")), Refusal::NoHost, QStringLiteral("empty host"));
    expectRefuse(link(QStringLiteral("https://host:abc/manifest.json")), Refusal::NoHost, QStringLiteral("bad port"));

    // Characters.
    expectRefuse(QStringLiteral("everythingbox://addon/") + enc(m) + QStringLiteral("%0A"), Refusal::BadCharacters,
                 QStringLiteral("encoded newline"));
    expectRefuse(QStringLiteral("everythingbox://addon/https://ho st/manifest.json"), Refusal::BadCharacters,
                 QStringLiteral("raw space"));
    expectRefuse(link(QStringLiteral("https://evil.example\\@good.example/manifest.json")), Refusal::BadCharacters,
                 QStringLiteral("backslash"));
    expectRefuse(link(QString::fromUtf8("https://h\xC3\xA9st.example/manifest.json")), Refusal::BadCharacters,
                 QStringLiteral("non-ASCII"));

    // The verb and the outer scheme.
    expectRefuse(QStringLiteral("everythingbox://install/") + enc(m), Refusal::UnknownVerb, QStringLiteral("verb install"));
    expectRefuse(QStringLiteral("everythingbox://addons/") + enc(m), Refusal::UnknownVerb, QStringLiteral("verb addons"));
    expectRefuse(QStringLiteral("everythingbox://") + enc(m), Refusal::UnknownVerb, QStringLiteral("no verb"));
    expectRefuse(QStringLiteral("everythingbox://addon/"), Refusal::NoPayload, QStringLiteral("empty payload"));
    expectRefuse(QStringLiteral("everythingbox://addon"), Refusal::NoPayload, QStringLiteral("no slash, no payload"));
    expectRefuse(QStringLiteral("everythingbox:/addon/") + enc(m), Refusal::WrongScheme, QStringLiteral("one slash"));
    expectRefuse(QStringLiteral("stremio://addon/") + enc(m), Refusal::WrongScheme, QStringLiteral("stremio scheme"));
    expectRefuse(QStringLiteral("everythingbox-test://addon/") + enc(m), Refusal::WrongScheme,
                 QStringLiteral("the test scheme, to the real parser"));
    expectRefuse(QString(), Refusal::Empty, QStringLiteral("empty"));

    // Double encoding: decode EXACTLY once. Each of these is an https manifest URL after TWO decodes.
    expectRefuse(QStringLiteral("everythingbox://addon/") + enc(enc(m)), Refusal::NotHttps,
                 QStringLiteral("double-encoded whole URL"));
    expectRefuse(QStringLiteral("everythingbox://addon/%2568ttps%3A%2F%2Fhost%2Fmanifest.json"), Refusal::NotHttps,
                 QStringLiteral("double-encoded scheme letter"));
    expectRefuse(QStringLiteral("everythingbox://addon/https%3A%2F%2Fhost%252Fmanifest.json"), Refusal::NoHost,
                 QStringLiteral("double-encoded path slash folds the path into the host"));
    expectRefuse(QStringLiteral("everythingbox://addon/https%3A%2F%2Fuser%2540evil.example%2Fmanifest.json"),
                 Refusal::NoHost, QStringLiteral("double-encoded @"));

    // The 2 KiB cap: the whole link, exactly at the boundary and one over it.
    const QString head = QStringLiteral("everythingbox://addon/") + enc(QStringLiteral("https://h.example/"));
    const QString tail = enc(QStringLiteral("/manifest.json"));
    const int pad = DeepLink::kMaxLinkChars - int(head.size()) - int(tail.size());
    const QString atCap = head + QString(pad, QLatin1Char('a')) + tail;
    check(atCap.size() == DeepLink::kMaxLinkChars, QStringLiteral("cap fixture is exactly the cap"));
    check(DeepLink::parse(atCap).ok(), QStringLiteral("a link of exactly 2048 characters is accepted"));
    const QString overCap = head + QString(pad + 1, QLatin1Char('a')) + tail;
    expectRefuse(overCap, Refusal::TooLong, QStringLiteral("2049 characters"));
    expectRefuse(QString(100000, QLatin1Char('e')), Refusal::TooLong, QStringLiteral("100 000 characters"));

    // The UI-test scheme is answered only by a parser told to answer it.
    check(DeepLink::parse(QStringLiteral("everythingbox-test://addon/") + enc(m),
                          QStringLiteral("everythingbox-test")).manifestUrl == m,
          QStringLiteral("a test-scheme parser accepts its own scheme"));
    check(!DeepLink::parse(link(m), QStringLiteral("everythingbox-test")).ok(),
          QStringLiteral("a test-scheme parser refuses the real scheme"));

    check(DeepLink::isLinkArgument(link(m), QStringLiteral("everythingbox")), QStringLiteral("argv: a link is a link"));
    check(!DeepLink::isLinkArgument(QStringLiteral("--fullscreen"), QStringLiteral("everythingbox")),
          QStringLiteral("argv: a flag is not a link"));
    check(!DeepLink::isLinkArgument(QStringLiteral("C:/x/EverythingBox.exe"), QStringLiteral("everythingbox")),
          QStringLiteral("argv: a path is not a link"));

    // Every refusal has words for the screen, and none of them is empty.
    for (int r = int(Refusal::Empty); r <= int(Refusal::NotManifest); ++r)
        check(!DeepLink::refusalText(Refusal(r)).isEmpty() && std::strlen(DeepLink::refusalCode(Refusal(r))) > 0,
              QStringLiteral("refusal %1 has text and a code").arg(r));
}

// ---- 2. the handoff ---------------------------------------------------------------------------------------
static void handoffFrames()
{
    std::printf("-- handoff frames --\n");
    const QString m = QStringLiteral("https://addon.example.org/opts%7Cx/manifest.json");
    const QByteArray msg = DeepLink::encodeHandoff(m);
    check(msg == QByteArray("EBDL1 ") + m.toLatin1() + '\n', QStringLiteral("frame shape"));
    const DeepLink::Result back = DeepLink::decodeHandoff(msg);
    check(back.ok() && back.manifestUrl == m, QStringLiteral("round trip"));

    check(DeepLink::encodeHandoff(QStringLiteral("http://addon.example.org/manifest.json")).isEmpty(),
          QStringLiteral("nothing unvalidated is ever encoded (http)"));
    check(DeepLink::encodeHandoff(QString()).isEmpty(), QStringLiteral("nothing unvalidated is ever encoded (empty)"));

    // A receiver cannot assume the sender was us: a hand-written frame is re-validated.
    check(DeepLink::decodeHandoff("EBDL1 http://addon.example.org/manifest.json\n").refusal == Refusal::NotHttps,
          QStringLiteral("receiver re-validates: http refused"));
    check(DeepLink::decodeHandoff("EBDL1 https://u:p@addon.example.org/manifest.json\n").refusal == Refusal::Userinfo,
          QStringLiteral("receiver re-validates: userinfo refused"));
    check(!DeepLink::decodeHandoff("EBDL1 https://addon.example.org/manifest.json").ok(),
          QStringLiteral("no newline: refused"));
    check(!DeepLink::decodeHandoff("EBDL2 https://addon.example.org/manifest.json\n").ok(),
          QStringLiteral("unknown version: refused"));
    check(!DeepLink::decodeHandoff("EBDL1 https://a.example/manifest.json\nEBDL1 https://b.example/manifest.json\n").ok(),
          QStringLiteral("two lines: refused"));
    check(!DeepLink::decodeHandoff(QByteArray("EBDL1 ") + QByteArray(DeepLink::kMaxHandoffBytes, 'a') + '\n').ok(),
          QStringLiteral("over-long frame: refused"));
    // A link the SENDER refused crosses as its reason alone.
    check(DeepLink::encodeRefusalHandoff(Refusal::NotHttps) == QByteArray("EBDL1! not-https") + char(10),
          QStringLiteral("refusal frame shape"));
    check(DeepLink::encodeRefusalHandoff(Refusal::None).isEmpty(), QStringLiteral("no refusal frame for an accepted link"));
    {
        const DeepLink::Result rr = DeepLink::decodeHandoff(DeepLink::encodeRefusalHandoff(Refusal::Userinfo));
        check(!rr.ok() && rr.refusal == Refusal::Userinfo && rr.manifestUrl.isEmpty(),
              QStringLiteral("refusal frame decodes to its reason and never to a URL"));
        check(DeepLink::decodeHandoff(QByteArray("EBDL1! https://a.example/manifest.json") + char(10)).refusal
                  == Refusal::Empty,
              QStringLiteral("a URL smuggled into a refusal frame is not accepted"));
    }
    // The link form itself is NOT a handoff: only the parsed manifest URL crosses.
    check(!DeepLink::decodeHandoff(QByteArray("EBDL1 ") + link(m).toLatin1() + '\n').ok(),
          QStringLiteral("a raw everythingbox:// link is not a valid frame"));
}

static bool waitFor(const std::function<bool()>& cond, int ms)
{
    QElapsedTimer t; t.start();
    while (!cond() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void handoffSocket()
{
    std::printf("-- handoff over a real local socket --\n");
    const QString name = DeepLink::handoffServerName(
        QStringLiteral("probe-home"), QString::number(QCoreApplication::applicationPid()),
        QStringLiteral("probe-") + QString::number(QRandomGenerator::global()->generate(), 16));

    check(DeepLinkChannel::sendToRunning(name, QStringLiteral("https://a.example/manifest.json"), 500)
              == DeepLinkChannel::Handoff::NoInstance,
          QStringLiteral("nobody listening: NoInstance (the new process starts normally)"));

    DeepLinkChannel receiver;
    QStringList arrived; QList<int> refused;
    QObject::connect(&receiver, &DeepLinkChannel::linkArrived, [&](const QString& u) { arrived << u; });
    QObject::connect(&receiver, &DeepLinkChannel::linkRefused, [&](int r) { refused << r; });
    check(receiver.listen(name), QStringLiteral("receiver listens"));

    // Held until the window is ready, then delivered. sendToRunning() blocks, and a receiver in the same thread
    // cannot answer a blocked sender, so the in-process sender here is a raw socket writing the same frame.
    const QString m = QStringLiteral("https://addon.example.org/opts%7Cx/manifest.json");
    auto sendRaw = [&](const QByteArray& frame) -> QByteArray {
        QLocalSocket s;
        s.connectToServer(name);
        if (!waitFor([&] { return s.state() == QLocalSocket::ConnectedState; }, 2000)) return "no-connect";
        s.write(frame);
        s.flush();
        QByteArray ack;
        waitFor([&] { ack += s.readAll(); return ack.contains('\n'); }, 3000);
        return ack;
    };
    const QByteArray ack1 = sendRaw(DeepLink::encodeHandoff(m));
    check(ack1 == "OK\n", QStringLiteral("valid frame acknowledged OK (got %1)").arg(QString::fromLatin1(ack1)));
    waitFor([] { return false; }, 150);
    check(arrived.isEmpty(), QStringLiteral("nothing is delivered before the window is ready"));
    receiver.setReady();
    check(waitFor([&] { return !arrived.isEmpty(); }, 2000) && arrived.value(0) == m,
          QStringLiteral("held link delivered once ready, unchanged"));

    const QByteArray ack2 = sendRaw("EBDL1 http://addon.example.org/manifest.json\n");
    check(ack2 == "NO\n", QStringLiteral("hand-written http frame answered NO (got %1)").arg(QString::fromLatin1(ack2)));
    check(waitFor([&] { return !refused.isEmpty(); }, 2000) && refused.value(0) == int(Refusal::NotHttps),
          QStringLiteral("receiver re-validated and refused it"));
    check(arrived.size() == 1, QStringLiteral("the refused frame delivered nothing"));

    const QByteArray ack3 = sendRaw(QByteArray(DeepLink::kMaxHandoffBytes + 64, 'x'));
    check(ack3 == "NO\n", QStringLiteral("an over-long stream is cut off with NO (got %1 bytes)").arg(ack3.size()));
    check(waitFor([&] { return refused.size() >= 2; }, 2000) && refused.value(1) == int(Refusal::TooLong),
          QStringLiteral("the over-long stream is reported as too long"));
    check(arrived.size() == 1, QStringLiteral("the over-long stream delivered nothing"));

    // A raw argv link reaches the same gate: a bad one is refused, a good one delivered.
    receiver.offer(QStringLiteral("everythingbox://addon/") + enc(QStringLiteral("file:///etc/manifest.json")),
                   QStringLiteral("everythingbox"));
    check(waitFor([&] { return refused.size() >= 3; }, 2000) && refused.value(2) == int(Refusal::NotHttps),
          QStringLiteral("argv link refused through the same parser"));
    receiver.offer(link(QStringLiteral("https://b.example/manifest.json")), QStringLiteral("everythingbox"));
    check(waitFor([&] { return arrived.size() >= 2; }, 2000)
              && arrived.value(1) == QStringLiteral("https://b.example/manifest.json"),
          QStringLiteral("argv link delivered"));

    // A second process that refused a link itself tells the running app why, with no URL in the message.
    const QByteArray ack4 = sendRaw(DeepLink::encodeRefusalHandoff(Refusal::UnknownVerb));
    check(ack4.startsWith("NO"), QStringLiteral("refusal frame answered NO"));
    check(waitFor([&] { return refused.size() >= 4; }, 2000) && refused.value(3) == int(Refusal::UnknownVerb),
          QStringLiteral("refusal frame reported with its reason"));
    check(arrived.size() == 2, QStringLiteral("a refusal frame delivers nothing to install"));
}

// ---- 3. the name ------------------------------------------------------------------------------------------
static void serverNames()
{
    std::printf("-- handoff server name --\n");
    const QString a1 = DeepLink::handoffServerName(QStringLiteral("C:/Users/alice"), QStringLiteral("1"), QString());
    const QString a1again = DeepLink::handoffServerName(QStringLiteral("C:/Users/alice"), QStringLiteral("1"), QString());
    const QString a2 = DeepLink::handoffServerName(QStringLiteral("C:/Users/alice"), QStringLiteral("2"), QString());
    const QString b1 = DeepLink::handoffServerName(QStringLiteral("C:/Users/bob"), QStringLiteral("1"), QString());
    const QString t1 = DeepLink::handoffServerName(QStringLiteral("C:/Users/alice"), QStringLiteral("1"),
                                                   QStringLiteral("eb-80b-rig"));
    check(a1 == a1again, QStringLiteral("stable for one user and session"));
    check(a1 != b1, QStringLiteral("two users never share a name"));
    check(a1 != a2, QStringLiteral("one user's two sessions never share a name"));
    check(t1 != a1 && t1.contains(QStringLiteral("eb-80b-rig")), QStringLiteral("a test rig gets its own name"));
    check(!a1.contains(QStringLiteral("alice"), Qt::CaseInsensitive), QStringLiteral("the name does not spell the profile"));
    bool clean = true;
    for (const QString& n : { a1, a2, b1, t1 })
        for (const QChar c : n)
            if (!(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')) || c.unicode() > 126) clean = false;
    check(clean && a1.size() < 100, QStringLiteral("names are short and [A-Za-z0-9_-] only"));
    check(!DeepLink::handoffServerName(QStringLiteral("x"), QString(), QStringLiteral("../../evil pipe")).contains(QLatin1Char('/')),
          QStringLiteral("a hostile test suffix cannot add a path"));
}

// ---- 4. one card at a time ----------------------------------------------------------------------------------
static void inbox()
{
    std::printf("-- one card at a time --\n");
    DeepLink::Inbox in;
    const auto a = in.arrive();
    check(!a.closeOpenCard, QStringLiteral("first link: nothing to close"));
    check(in.presentCard(a.ticket) && in.openCard() == a.ticket, QStringLiteral("first link's card opens"));

    // A second link while A's card is open: A's card closes, and B's opens when its fetch lands.
    const auto b = in.arrive();
    check(b.closeOpenCard, QStringLiteral("second link closes the open card"));
    check(in.openCard() == 0, QStringLiteral("no card counts as open while B fetches"));
    check(!in.takeInstall(a.ticket), QStringLiteral("an Install press on A's closing card installs nothing"));
    in.cardClosed(a.ticket);
    check(in.presentCard(b.ticket) && in.openCard() == b.ticket, QStringLiteral("B's card opens"));

    // A third and fourth arrive back to back while B's card is up; only the newest fetch may present.
    const auto c = in.arrive();
    const auto d = in.arrive();
    check(c.closeOpenCard && !d.closeOpenCard, QStringLiteral("only the arrival that found a card open closes one"));
    in.cardClosed(b.ticket);
    check(!in.presentCard(c.ticket), QStringLiteral("C's fetch lands late: superseded, no card"));
    check(in.presentCard(d.ticket), QStringLiteral("D's card opens"));
    in.cardClosed(c.ticket);   // a stale close must not close D's card
    check(in.openCard() == d.ticket, QStringLiteral("a superseded card's close leaves the open card alone"));

    check(in.takeInstall(d.ticket), QStringLiteral("Install on the open card counts"));
    check(!in.takeInstall(d.ticket), QStringLiteral("…exactly once"));
    in.cardClosed(d.ticket);
    check(in.openCard() == 0, QStringLiteral("closed"));
    check(!in.takeInstall(d.ticket), QStringLiteral("no Install after the card closed"));

    // Never more than one card, however many links arrive.
    DeepLink::Inbox flood;
    int open = 0, maxOpen = 0;
    QVector<quint64> tickets;
    for (int i = 0; i < 50; ++i)
    {
        const auto t = flood.arrive();
        if (t.closeOpenCard) --open;
        tickets << t.ticket;
        if (i % 3 == 0 && flood.presentCard(t.ticket)) ++open;
        maxOpen = qMax(maxOpen, open);
    }
    for (quint64 t : tickets) if (flood.presentCard(t)) { ++open; maxOpen = qMax(maxOpen, open); }
    check(maxOpen <= 1, QStringLiteral("a flood of 50 links never has two cards open (max %1)").arg(maxOpen));
}

// ---- 5. registration, as data -------------------------------------------------------------------------------
static void registration()
{
    std::printf("-- registration plan --\n");
    using Op = DeepLink::RegOp;
    const QString exe = QStringLiteral("C:\\Program Files\\EverythingBox\\EverythingBox.exe");
    const QVector<Op> plan = DeepLink::windowsRegisterPlan(QStringLiteral("everythingbox"), exe);
    const QString k = QStringLiteral("Software\\Classes\\everythingbox");
    const QVector<Op> want = {
        { Op::SetValue, k, QString(), QStringLiteral("URL:EverythingBox link") },
        { Op::SetValue, k, QStringLiteral("URL Protocol"), QString() },
        { Op::SetValue, k + QStringLiteral("\\DefaultIcon"), QString(), QLatin1Char('"') + exe + QStringLiteral("\",0") },
        { Op::SetValue, k + QStringLiteral("\\shell\\open\\command"), QString(),
          QLatin1Char('"') + exe + QStringLiteral("\" \"%1\"") },
    };
    check(plan == want, QStringLiteral("register plan is exactly the four values (%1 ops)").arg(plan.size()));
    for (const Op& o : plan)
        check(o.key == k || o.key.startsWith(k + QLatin1Char('\\')),
              QStringLiteral("every op stays under Software\\Classes\\<scheme>: %1").arg(o.key));
    check(DeepLink::windowsOpenCommand(QStringLiteral("C:/x/EverythingBox.exe"))
              == QStringLiteral("\"C:\\x\\EverythingBox.exe\" \"%1\""),
          QStringLiteral("command: native separators, both halves quoted"));

    const QVector<Op> off = DeepLink::windowsUnregisterPlan(QStringLiteral("everythingbox"));
    check(off.size() == 1 && off[0].kind == Op::DeleteTree && off[0].key == k,
          QStringLiteral("unregister plan is one DeleteTree of the scheme's own key"));

    check(DeepLink::windowsRegisterPlan(QStringLiteral("everythingbox-test"), exe).value(0).key
              == QStringLiteral("Software\\Classes\\everythingbox-test"),
          QStringLiteral("the test scheme gets its own key"));
    check(DeepLink::windowsRegisterPlan(QString(), exe).isEmpty(), QStringLiteral("no scheme: no plan"));
    check(DeepLink::windowsRegisterPlan(QStringLiteral("evil\\Software"), exe).isEmpty(),
          QStringLiteral("a scheme with a backslash cannot reach another key"));
    check(DeepLink::windowsUnregisterPlan(QStringLiteral("..")).isEmpty(), QStringLiteral("'..' is not a scheme"));
    check(DeepLink::windowsUnregisterPlan(QString()).isEmpty(),
          QStringLiteral("an empty scheme can never delete Software\\Classes itself"));
    check(DeepLink::windowsRegisterPlan(QStringLiteral("everythingbox"), QStringLiteral("C:\\a\"b.exe")).isEmpty(),
          QStringLiteral("an exe path with a quote: no plan"));
    check(DeepLink::windowsRegisterPlan(QStringLiteral("everythingbox"), QString()).isEmpty(),
          QStringLiteral("no exe path: no plan"));

    // Which scheme a process answers to, and which it may register.
    check(DeepLink::schemeFor(false, QStringLiteral("everythingbox-test")) == QStringLiteral("everythingbox"),
          QStringLiteral("the override is ignored without the test channel"));
    check(DeepLink::schemeFor(true, QStringLiteral("everythingbox-test")) == QStringLiteral("everythingbox-test"),
          QStringLiteral("the override applies under the test channel"));
    check(DeepLink::schemeFor(true, QStringLiteral("Bad Scheme!")) == QStringLiteral("everythingbox"),
          QStringLiteral("an invalid override is ignored"));
    check(DeepLink::registrationSchemeFor(false, QString()) == QStringLiteral("everythingbox"),
          QStringLiteral("production registers everythingbox"));
    check(DeepLink::registrationSchemeFor(true, QString()).isEmpty(),
          QStringLiteral("a test rig without an override registers NOTHING"));
    check(DeepLink::registrationSchemeFor(true, QStringLiteral("everythingbox")).isEmpty(),
          QStringLiteral("a test rig may never register the real scheme"));
    check(DeepLink::registrationSchemeFor(true, QStringLiteral("EverythingBox")).isEmpty(),
          QStringLiteral("…in any case"));
    check(DeepLink::registrationSchemeFor(true, QStringLiteral("everythingbox-test")) == QStringLiteral("everythingbox-test"),
          QStringLiteral("a test rig registers its test scheme"));
    check(DeepLink::registrationSchemeFor(false, QStringLiteral("everythingbox-test")) == QStringLiteral("everythingbox"),
          QStringLiteral("production ignores the override"));
    check(DeepLink::isValidScheme(QStringLiteral("everythingbox-test")) && !DeepLink::isValidScheme(QStringLiteral("1abc"))
              && !DeepLink::isValidScheme(QStringLiteral("a b")) && !DeepLink::isValidScheme(QString()),
          QStringLiteral("scheme syntax"));

    std::printf("-- linux .desktop entry --\n");
    const QString de = DeepLink::linuxDesktopEntry(QStringLiteral("everythingbox"),
                                                   QStringLiteral("/home/u/Apps/Everything Box/EverythingBox$1.AppImage"));
    check(de.startsWith(QStringLiteral("[Desktop Entry]\n")), QStringLiteral("desktop: header"));
    check(de.contains(QStringLiteral("\nMimeType=x-scheme-handler/everythingbox;\n")), QStringLiteral("desktop: MimeType"));
    check(de.contains(QStringLiteral("\nExec=\"/home/u/Apps/Everything Box/EverythingBox\\\\$1.AppImage\" %u\n")),
          QStringLiteral("desktop: Exec quoted, $ escaped, %u passes the URL:\n%1").arg(de));
    check(de.contains(QStringLiteral("\nNoDisplay=true\n")), QStringLiteral("desktop: hidden from menus"));
    check(DeepLink::linuxDesktopFileName(QStringLiteral("everythingbox")) == QStringLiteral("everythingbox-url-handler.desktop"),
          QStringLiteral("desktop: file name"));
    check(DeepLink::linuxDesktopEntry(QStringLiteral("everythingbox"), QStringLiteral("/a\nExec=evil")).isEmpty(),
          QStringLiteral("desktop: a newline in the path refuses"));
    check(DeepLink::linuxDesktopEntry(QStringLiteral("a/b"), QStringLiteral("/x")).isEmpty(),
          QStringLiteral("desktop: an invalid scheme refuses"));
}

// ---- 6. the card's words -------------------------------------------------------------------------------------
static void cardWords()
{
    std::printf("-- the card --\n");
    const QString msg = DeepLink::confirmMessage(QStringLiteral("addon.example.org"),
                                                 { QStringLiteral("catalog"), QStringLiteral("stream") },
                                                 { QStringLiteral("movie"), QStringLiteral("series") }, {});
    check(msg.contains(QStringLiteral("addon.example.org")), QStringLiteral("card names the host"));
    check(msg.contains(QStringLiteral("catalog, stream")), QStringLiteral("card lists the resources"));
    check(msg.contains(QStringLiteral("movie, series")), QStringLiteral("card lists the catalog types"));
    check(msg.contains(QStringLiteral("This link came from outside EverythingBox")), QStringLiteral("card says where it came from"));
    const QString none = DeepLink::confirmMessage(QStringLiteral("h.example"), {}, {}, {});
    check(none.contains(QStringLiteral("h.example")), QStringLiteral("a manifest that declares nothing still names its host"));
    const QString perms = DeepLink::confirmMessage(QStringLiteral("h.example"), {}, {}, { QStringLiteral("network") });
    check(perms.contains(QStringLiteral("network")), QStringLiteral("declared permissions are listed"));
    const QString t = DeepLink::confirmTitle(QStringLiteral("Evil\nOfficial Add-on") + QString(300, QLatin1Char('x')));
    check(!t.contains(QLatin1Char('\n')) && t.size() < 140, QStringLiteral("the add-on's own name cannot add lines or run on"));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    parserTable();
    handoffFrames();
    handoffSocket();
    serverNames();
    inbox();
    registration();
    cardWords();
    std::printf("probe_deeplink: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { std::printf("DEEPLINK-FAIL\n"); return 1; }
    std::printf("DEEPLINK-OK\n");
    return 0;
}
