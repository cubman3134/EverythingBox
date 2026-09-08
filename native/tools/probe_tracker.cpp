// Headless check of the tracker seam and ALL THREE provider rules layers (issue #156, increments 1-3).
//
// INCREMENT 3 ADDED KITSU, and sections 20-21 are its half. They are written to answer ONE question:
// did the layer issue #326 hoisted hold for a provider it was not written against? So §20 asserts only
// what Kitsu SUPPLIES (its password grant, its JSON:API wire, its status codes) and then drives
// everything it CONSUMES — tracker::classifySend and TrackerQueue::Sender — with Id::Kitsu, because
// there is no Kitsu copy of either. §21 pins three trackers at once with one and then two of them
// failing, and AniList and MyAnimeList being byte-identical with Kitsu configured beside them.
//
// KITSU'S CREDENTIAL CLAIM IS STRONGER THAN THE OTHER TWO'S. Its sign-in is an OAuth password grant,
// so the credential is the user's own account password and it is NEVER STORED: §20's byte-scan asserts
// it occurs ZERO times in the ini, where §3 and §18 assert exactly one occurrence for a client secret
// that legitimately lives there.
//
// NO KITSU ACCOUNT WAS CREATED and no API client was registered for this work.
//
// INCREMENT 2 ADDED MYANIMELIST behind the same seam, and sections 12-18 are its half: the OAuth+PKCE
// exchange, the REST wire (search, pagination, the entry read, the list write), the rate-limit backoff,
// the "a match we are not sure of is not written" rule, several trackers configured at once with one of
// them failing, the ONE offline queue both of them share, the MyAnimeList credential byte-scan, and
// ANILIST BEING UNCHANGED - the last of those pins increment 1's bytes and state exactly, because a
// second provider can break either without touching a line of the first one's code.
//
// NO MYANIMELIST ACCOUNT WAS CREATED and no API client was registered for this work. Every MAL fixture
// below is written from MyAnimeList's published API v2 reference, and nothing here or in the live drive
// that accompanies it contacted MyAnimeList.
//
// Everything that decides anything in this feature is pure — the OAuth bodies, the three GraphQL documents,
// the debounce, the offline queue, the furthest-wins reconciliation and the per-item link store — so all of
// it is reachable here with no socket, no AniList account and no browser. AniListTracker owns the socket and
// nothing else; if a rule can be got wrong, it can be got wrong in a file this probe links.
//
// The fixtures are written from AniList's published GraphQL schema (anilist.gitbook.io / the public
// GraphiQL): `Page.media` for search, `Media.mediaListEntry` for the account's row, and
// `SaveMediaListEntry(mediaId, progress, status, scoreRaw)` for the push. REAL ANILIST WAS NOT REACHED by
// this probe or by the live drive that accompanies it — a fixture stub answered both.
//
// The three properties pinned hardest, because each one damages somebody's tracker account rather than
// merely failing:
//   * `scoreRaw` is ABSENT from a mutation unless the app really has a rating (§6). AniList reads 0 as
//     "rated zero", so an unconditional score erases a rating the user set by hand.
//   * status COMPLETED needs the app's claim AND the tracker's own unit count to agree (§6). A provider
//     listing missing its final chapters would otherwise mark a running series finished.
//   * a token reply that is not one can never be stored (§2). Writing its empty strings over the live
//     tokens permanently unlinks the account — the failure TraktRead §13 documents for Trakt.
//
// §3 is the credential byte-scan: the fixture client secret must appear in the ini under exactly ONE key,
// and that key must be inside the device-local carve-out. Nothing this probe prints contains it.
//
// Prints TRACKER-OK on success; any failure prints TRACKER-FAIL <cond> and exits non-zero.
#include "Tracker.h"
#include "TrackerFanout.h"
#include "TrackerLinks.h"
#include "TrackerQueue.h"
#include "TrackerRules.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdio>
#include <functional>

using namespace tracker;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "TRACKER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The fixture credentials. Distinctive enough that a byte-scan cannot match them by accident, and NEVER
// printed by this probe — see §3 and the credential rule in CONTRIBUTING.
static const char* kFixtureClientId = "fixture-client-14882";
static const char* kFixtureSecret   = "FIXTURE-ANILIST-SECRET-Z9Q7X";

// ---- fixtures ------------------------------------------------------------------------------------------

// A search reply in AniList's shape: three rows. The first has both titles and an episode count (anime); the
// second has only a romaji title and a chapter count (manga); the third is MALFORMED — no id — and must be
// skipped without costing the other two.
static const char* kSearchReply = R"({
  "data": { "Page": { "media": [
    { "id": 20605, "title": { "romaji": "Boku no Hero", "english": "My Hero Academia" },
      "startDate": { "year": 2016 }, "episodes": 13, "chapters": null,
      "coverImage": { "large": "https://img/1.jpg" } },
    { "id": 30002, "title": { "romaji": "Berserk", "english": null },
      "startDate": { "year": 1989 }, "episodes": null, "chapters": 364,
      "coverImage": { "large": "https://img/2.jpg" } },
    { "title": { "romaji": "No Id At All" }, "startDate": { "year": 2020 } }
  ] } }
})";

// The account HAS a row for this media: 12 chapters in, CURRENT, rated 85.
static const char* kEntryReply = R"({
  "data": { "Media": { "id": 30002, "episodes": null, "chapters": 364,
    "mediaListEntry": { "id": 5551, "progress": 12, "status": "CURRENT", "score": 85 } } }
})";

// The account does NOT have a row: mediaListEntry is an explicit null. Distinct from "progress 0".
static const char* kEntryUnlisted = R"({
  "data": { "Media": { "id": 30002, "episodes": null, "chapters": 364, "mediaListEntry": null } }
})";

// A GraphQL error. HTTP 200, no `data`. Every parser must treat it as "not that payload".
static const char* kGraphQlError = R"({
  "errors": [ { "message": "Invalid token", "status": 401 } ], "data": null
})";

// ---- MyAnimeList fixtures (issue #156, increment 2) -----------------------------------------------------
//
// Written from MyAnimeList's published API v2 reference. NO MYANIMELIST ACCOUNT WAS CREATED, no API client
// was registered, and neither this probe nor the live drive that accompanies it contacted MyAnimeList: a
// local fixture server answered both.
static const char* kMalClientId = "fixture-mal-27310";
static const char* kMalSecret   = "FIXTURE-MAL-SECRET-Q4W8R";

// A search reply in MAL's shape: rows are wrapped in `node`, and the page carries an absolute `paging.next`.
// The first row is anime with both titles and a large cover; the second is manga with one title and only a
// medium cover; the third is MALFORMED (no id) and must be skipped without costing the other two.
static const char* kMalSearchReply = R"({
  "data": [
    { "node": { "id": 31964, "title": "Boku no Hero Academia",
                "main_picture": { "medium": "https://cdn/1m.jpg", "large": "https://cdn/1l.jpg" },
                "alternative_titles": { "en": "My Hero Academia", "ja": "僕のヒーロー" },
                "start_date": "2016-04-03", "num_episodes": 13 } },
    { "node": { "id": 2, "title": "Berserk",
                "main_picture": { "medium": "https://cdn/2m.jpg" },
                "alternative_titles": { "en": "" },
                "start_date": "1989-08-25", "num_chapters": 364 } },
    { "node": { "title": "No Id At All", "start_date": "2020" } }
  ],
  "paging": { "next": "https://api.myanimelist.net/v2/anime?offset=4&q=My%20Hero" }
})";

// A row MAL has no count and no date for — an unannounced series. It is filed under the kind the caller
// ASKED for, because the search endpoint itself is per kind.
static const char* kMalUnairedReply = R"({
  "data": [ { "node": { "id": 99999, "title": "Something Unannounced" } } ]
})";

// The account HAS a row for this manga: 12 chapters in, reading, rated 9 (which is 90 at the seam).
static const char* kMalEntryReply = R"({
  "id": 2, "title": "Berserk", "num_chapters": 364,
  "my_list_status": { "status": "reading", "score": 9, "num_chapters_read": 12,
                      "num_volumes_read": 2, "is_rereading": false, "updated_at": "2024-01-01T00:00:00+00:00" }
})";

// ...and the anime side, which is where MAL's asymmetry lives: it REPORTS `num_episodes_watched` here and
// ACCEPTS `num_watched_episodes` on the write.
static const char* kMalEntryAnimeReply = R"({
  "id": 31964, "title": "Boku no Hero Academia", "num_episodes": 13,
  "my_list_status": { "status": "completed", "score": 0, "num_episodes_watched": 7,
                      "is_rewatching": false }
})";

// The account does NOT have a row: `my_list_status` is absent entirely, which is how MAL says it.
static const char* kMalEntryUnlisted = R"({
  "id": 2, "title": "Berserk", "num_chapters": 364
})";

// MAL's error envelope. It arrives at several statuses and must be "not that payload" to every parser.
static const char* kMalErrorReply = R"({
  "error": "invalid_token", "message": "The access token is invalid"
})";


// ---- Kitsu fixtures (issue #156, increment 3) -----------------------------------------------------------
//
// Written from Kitsu's published JSON:API reference. NO KITSU ACCOUNT WAS CREATED, no API client was
// registered, and neither this probe nor the live drive that accompanies it contacted Kitsu: a local
// fixture server answered both.
//
// THE PASSWORD IS THE ONE CREDENTIAL IN THIS FEATURE THAT IS NEVER STORED. §20's byte-scan asserts it
// occurs ZERO times in the ini, where AniList's and MyAnimeList's secrets are asserted to occur exactly
// once. It is never printed by this probe.
static const char* kKitsuEmail    = "fixture-kitsu@example.invalid";
static const char* kKitsuPassword = "FIXTURE-KITSU-PASSWORD-M3V6B";

// `users?filter[self]=true` — the signed-in account, whose id every library-entry request needs.
static const char* kSelfReply = R"({
  "data": [ { "id": "42", "type": "users", "attributes": { "name": "fixture" } } ]
})";

// A search reply in Kitsu's shape: JSON:API resource objects with the payload under `attributes`, and an
// absolute `links.next`. The first row is anime with both titles and an original poster; the second is
// manga with one title and only a medium poster; the third is MALFORMED (no id) and must be skipped
// without costing the other two.
static const char* kKitsuSearchReply = R"({
  "data": [
    { "id": "7442", "type": "anime", "attributes": {
        "canonicalTitle": "Boku no Hero Academia",
        "titles": { "en": "My Hero Academia", "en_jp": "Boku no Hero Academia", "ja_jp": "僕" },
        "startDate": "2016-04-03", "episodeCount": 13,
        "posterImage": { "medium": "https://media.kitsu.test/1-medium.jpg",
                         "original": "https://media.kitsu.test/1-original.jpg" } } },
    { "id": "1712", "type": "manga", "attributes": {
        "canonicalTitle": "Berserk",
        "titles": { "en": "" },
        "startDate": "1989-08-25", "chapterCount": 364,
        "posterImage": { "medium": "https://media.kitsu.test/2-medium.jpg" } } },
    { "type": "anime", "attributes": { "canonicalTitle": "No Id At All", "startDate": "2020" } }
  ],
  "links": { "next": "https://kitsu.app/api/edge/anime?page%5Boffset%5D=8" }
})";

// A row Kitsu has no count and no date for — an unreleased series. It is filed under the kind the caller
// ASKED for, because the search endpoint itself is per kind.
static const char* kKitsuUnreleasedReply = R"({
  "data": [ { "id": "99999", "type": "manga", "attributes": { "canonicalTitle": "Something Unannounced" } } ]
})";

// The account HAS a row for this manga: 12 chapters in, current, ratingTwenty 17 (which is 85 at the seam).
// The library entry has its OWN id — 551 — which is what a PATCH is addressed to, and the media it belongs
// to comes back under `included`, which is where the unit COUNT is read from.
static const char* kKitsuEntryReply = R"({
  "data": [ { "id": "551", "type": "libraryEntries", "attributes": {
      "status": "current", "progress": 12, "ratingTwenty": 17, "reconsuming": false } } ],
  "included": [ { "id": "1712", "type": "manga",
                  "attributes": { "canonicalTitle": "Berserk", "chapterCount": 364 } } ]
})";

// A JSON:API error document. It arrives at several statuses and must be "not that payload" to every parser.
static const char* kKitsuErrorReply = R"({
  "errors": [ { "title": "Unauthorized", "detail": "invalid token", "status": "401" } ]
})";

// ---- a tracker that is not a tracker --------------------------------------------------------------------
// The several-trackers-at-once rule is about what happens when one of them is off, unlinked or refusing,
// and none of those states is reachable through a real socket in a probe. tracker::Tracker is a pure
// abstract seam with no QObject in it precisely so this is possible.
namespace
{
    struct FakeTracker : public tracker::Tracker
    {
        explicit FakeTracker(tracker::Id i) : id_(i) {}

        tracker::Id id_;
        bool on_ = true;         // configured AND connected
        bool refuse = false;     // "connected, and failing" — the case that must not stop the others
        int  refusals = 0;
        QVector<tracker::Update> got;

        tracker::Id id() const override { return id_; }
        QString displayName() const override { return tracker::idToken(id_); }
        bool configured() const override { return on_; }
        bool connected() const override { return on_; }
        void search(const QString&, int, tracker::Kind,
                    std::function<void(QVector<tracker::Match>)> cb) override { if (cb) cb({}); }
        void fetchEntry(const QString&, tracker::Kind,
                        std::function<void(bool, tracker::Entry)> cb) override
        { if (cb) cb(false, tracker::Entry{}); }
        void pushProgress(const tracker::Update& u) override
        {
            if (refuse) { ++refusals; return; }   // accepted the call, delivered nothing — a live failure
            got.push_back(u);
        }
        void flushQueue() override {}
    };
}

static QJsonObject varsOf(const QByteArray& body)
{
    return QJsonDocument::fromJson(body).object().value(QStringLiteral("variables")).toObject();
}

// ---- §19's apparatus: the drain loop as it was, and the drain loop as it is -----------------------------

// The sentence increment 1's AniList drain wrote into the status line on ANY failure. Spelled out here
// rather than referenced, so §19's identity assertion is against the old text and not against whatever the
// new code happens to say.
static const char* kLegacyRetryLine =
    "AniList did not accept the update; it is queued and will be retried.";

// One scripted answer for the fake transport.
struct ScriptedReply
{
    int    status = 200;
    qint64 retryAfterSec = 0;
    bool   accepted = true;
};

// A LITERAL TRANSCRIPTION of AniListTracker::drain() as issue #156 increment 1 shipped it — the version
// this issue replaced. It is here so the "a run with no permanently-refused response behaves exactly as it
// did" claim is checked against the real old loop rather than against a description of one.
//
// The only edits are the ones a probe forces: the clock is passed in, the transport is the script, and
// `retry_->start(ms)` is a push onto `waits`. Every decision is the old decision, including the one that
// was the bug — EVERY failure lands in the same branch and the row stays queued for ever.
static void legacyDrain(const QVector<ScriptedReply>& script, int& at, QStringList& sent,
                        qint64 nowMs, QVector<qint64>& waits)
{
    QVector<Update> q = TrackerQueue::load(Id::AniList);
    if (q.isEmpty()) { waits.push_back(0); return; }   // retry_->stop()

    qint64 soonest = -1;
    const int idx = TrackerQueue::nextSendable(q, Id::AniList, nowMs, &soonest);
    if (idx < 0) { waits.push_back(qBound<qint64>(1000, soonest, kDebounceMs)); return; }

    const Update u = q[idx];
    sent << (u.itemKey + QStringLiteral("@") + QString::number(u.unit));
    const ScriptedReply r = at < script.size() ? script[at++] : ScriptedReply{ 0, 0, false };
    if (!r.accepted)
    {
        TrackerQueue::setLastError(Id::AniList, QString::fromLatin1(kLegacyRetryLine));
        waits.push_back(60000);   // kRetryMs — flat, for ever
        return;
    }
    TrackerQueue::removeDelivered(Id::AniList, u);
    TrackerQueue::noteSent(Id::AniList, u.itemKey, nowMs);
    TrackerQueue::setLastError(Id::AniList, QString());
    legacyDrain(script, at, sent, nowMs, waits);
}

// The SAME run through TrackerQueue::Sender. Returns nothing: everything it is asked about is either in the
// out-parameters or in the ini the two runs share.
static void senderDrain(Id id, const SendPolicy& policy, const QVector<ScriptedReply>& script, int& at,
                        QStringList& sent, QStringList& pushed, qint64 nowMs, QVector<qint64>& waits,
                        const QString& droppedFmt = QString())
{
    TrackerQueue::SendSpec spec;
    spec.id = id;
    spec.policy = policy;
    spec.ready = [] { return true; };
    spec.now = [nowMs] { return nowMs; };
    spec.send = [&script, &at, &sent](const Update& u, std::function<void(TrackerQueue::Reply)> done) {
        sent << (u.itemKey + QStringLiteral("@") + QString::number(u.unit));
        const ScriptedReply s = at < script.size() ? script[at++] : ScriptedReply{ 0, 0, false };
        TrackerQueue::Reply r;
        r.status = s.status;
        r.retryAfterSec = s.retryAfterSec;
        r.accepted = s.accepted;
        // SYNCHRONOUS, which is the point: the real transport answers off a QNetworkReply, and Sender is
        // written so that either is correct. A synchronous answer is what makes the whole drain — including
        // the continue-after-a-drop — reachable in a probe with no socket and no event loop.
        if (done) done(r);
    };
    spec.droppedMessage = [droppedFmt, id](const Update& u) {
        // THE SPEC'S OWN id. It was Id::AniList when only two trackers existed and only AniList's drop
        // message was driven through here; a third provider made that a lookup on the wrong tracker's link
        // store, which reads back an empty title and quietly turns a "which item" message into a generic
        // one. §19's runs drain Id::AniList and are unaffected.
        const QString title = TrackerLinks::get(id, u.itemKey).title;
        return droppedFmt.isEmpty() ? QStringLiteral("dropped:") + u.itemKey
                                    : droppedFmt.arg(title.isEmpty() ? u.itemKey : title);
    };
    spec.reauthMessage = QStringLiteral("reauth");
    // BYTE-FOR-BYTE the sentence increment 1 wrote, so the identity assertion can compare the status line.
    spec.retryMessage = QString::fromLatin1(kLegacyRetryLine);
    spec.pushed = [&pushed](const QString& itemKey, int unit) {
        pushed << (itemKey + QStringLiteral("@") + QString::number(unit));
    };
    spec.wait = [&waits](int delayMs) { waits.push_back(delayMs); };
    TrackerQueue::Sender sender(std::move(spec));
    sender.drain();
}

// Seed `id`'s queue with one row per (itemKey, unit) pair, in order.
static void seedQueue(Id id, const QVector<QPair<QString, int>>& rows, qint64 atMs)
{
    TrackerQueue::forgetAccount(id);
    QVector<Update> q;
    for (const QPair<QString, int>& r : rows)
    {
        Update u;
        u.itemKey = r.first;
        u.mediaId = QStringLiteral("m") + r.first.right(1);
        u.kind = Kind::Anime;
        u.unit = r.second;
        u.atMs = atMs;
        q.push_back(u);
    }
    TrackerQueue::save(id, q);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ===== §1  the seam: stable tokens and the three key families ======================================
    {
        CHECK(idToken(Id::AniList) == QLatin1String("anilist"));
        // RESERVED, and asserted so a later increment cannot renumber them: a stored link filed under
        // token 1 must still mean MyAnimeList when MyAnimeList arrives.
        CHECK(static_cast<int>(Id::AniList) == 0);
        CHECK(static_cast<int>(Id::MyAnimeList) == 1);
        CHECK(static_cast<int>(Id::Kitsu) == 2);
        CHECK(idToken(Id::MyAnimeList) == QLatin1String("mal"));
        CHECK(idToken(Id::Kitsu) == QLatin1String("kitsu"));

        // THE CARVE-OUT SPLIT, which is the whole security posture of this feature in four assertions.
        // Credentials + tokens + push state: device-local, never synced.
        CHECK(isDeviceLocalKey(clientIdKey(Id::AniList)));
        CHECK(isDeviceLocalKey(clientSecretKey(Id::AniList)));
        CHECK(isDeviceLocalKey(accessKey(Id::AniList)));
        CHECK(isDeviceLocalKey(refreshKey(Id::AniList)));
        CHECK(isDeviceLocalKey(queueKey(QString(), Id::AniList)));
        CHECK(isDeviceLocalKey(lastSentKey(QString(), Id::AniList, QStringLiteral("k"))));
        // Links: the INVERSE. They are per-item-synced, so they must NOT be in the device-local family, or
        // a user who links a library on the TV would have to link it again on the laptop.
        CHECK(!isDeviceLocalKey(TrackerLinks::itemsGroup()));
        CHECK(TrackerLinks::itemsGroup().startsWith(linkKeyPrefix()));

        // The settings-transaction split: a typed secret is discardable, a token obtained mid-visit is not.
        CHECK(!isBackgroundStateKey(clientIdKey(Id::AniList)));
        CHECK(!isBackgroundStateKey(clientSecretKey(Id::AniList)));
        CHECK(isBackgroundStateKey(accessKey(Id::AniList)));
        CHECK(isBackgroundStateKey(refreshKey(Id::AniList)));
        CHECK(isBackgroundStateKey(expiryKey(Id::AniList)));
        CHECK(isBackgroundStateKey(queueKey(QString(), Id::AniList)));
        // isTokenKey covers the reserved trackers too, so adding MyAnimeList does not need this list edited.
        CHECK(isTokenKey(accessKey(Id::MyAnimeList)));
        CHECK(isTokenKey(refreshKey(Id::Kitsu)));
        CHECK(!isTokenKey(clientIdKey(Id::MyAnimeList)));

        // An empty profile id maps to the same slot the default profile reads — the Scrobble/Trakt rule.
        CHECK(profileSlot(QString()) == QLatin1String("default"));
        CHECK(queueKey(QString(), Id::AniList) == queueKey(QStringLiteral("default"), Id::AniList));
        // …and two real profiles do NOT share a queue: one person's pending chapters are not another's.
        CHECK(queueKey(QStringLiteral("a"), Id::AniList) != queueKey(QStringLiteral("b"), Id::AniList));

        // The #81 follow-up's slot names are pinned so the later one-line change has something to match.
        CHECK(builtinSecretIdSlot() == QLatin1String("kAniList_Id"));
        CHECK(builtinSecretSecretSlot() == QLatin1String("kAniList_Secret"));
    }

    // ===== §2  OAuth: the authorize URL, the two grant bodies, and the token reply =====================
    {
        const QString redirect = QStringLiteral("http://127.0.0.1:51423");
        const QString url = anilist::authorizeUrl(anilist::defaultAuthBase(),
                                                  QString::fromLatin1(kFixtureClientId), redirect);
        CHECK(url.startsWith(QLatin1String("https://anilist.co/api/v2/oauth/authorize?")));
        CHECK(url.contains(QLatin1String("response_type=code")));
        CHECK(url.contains(QLatin1String("client_id=fixture-client-14882")));
        // The loopback redirect is percent-encoded into the query, which is what AniList's developer page
        // must be told to register. Asserted on the DECODED form so the check does not depend on QUrl's
        // encoding choices.
        CHECK(QUrl::fromPercentEncoding(url.toUtf8()).contains(redirect));
        // No secret is ever in a browser URL.
        CHECK(!url.contains(QString::fromLatin1(kFixtureSecret)));

        const QByteArray ex = anilist::tokenExchangeBody(QString::fromLatin1(kFixtureClientId),
                                                         QString::fromLatin1(kFixtureSecret),
                                                         redirect, QStringLiteral("THE-CODE"));
        const QJsonObject exo = QJsonDocument::fromJson(ex).object();
        CHECK(exo.value(QStringLiteral("grant_type")).toString() == QLatin1String("authorization_code"));
        CHECK(exo.value(QStringLiteral("code")).toString() == QLatin1String("THE-CODE"));
        CHECK(exo.value(QStringLiteral("redirect_uri")).toString() == redirect);
        // The secret is in the POST BODY (over TLS) and in no other artefact this feature produces.
        CHECK(exo.value(QStringLiteral("client_secret")).toString() == QString::fromLatin1(kFixtureSecret));

        const QByteArray rf = anilist::tokenRefreshBody(QString::fromLatin1(kFixtureClientId),
                                                        QString::fromLatin1(kFixtureSecret),
                                                        QStringLiteral("RTOKEN"));
        const QJsonObject rfo = QJsonDocument::fromJson(rf).object();
        CHECK(rfo.value(QStringLiteral("grant_type")).toString() == QLatin1String("refresh_token"));
        CHECK(rfo.value(QStringLiteral("refresh_token")).toString() == QLatin1String("RTOKEN"));
        CHECK(!rfo.contains(QStringLiteral("code")));   // a refresh carries no authorization code

        // A real reply.
        anilist::TokenReply t = anilist::parseTokenReply(
            R"({"token_type":"Bearer","expires_in":31536000,"access_token":"AAA","refresh_token":"BBB"})");
        CHECK(t.ok);
        CHECK(t.accessToken == QLatin1String("AAA"));
        CHECK(t.refreshToken == QLatin1String("BBB"));
        CHECK(t.expiresInSec == 31536000);

        // THE ONE THAT PERMANENTLY UNLINKS AN ACCOUNT. Each of these is a 200 that is not a token reply,
        // and each must come back ok=false so the caller stores nothing.
        CHECK(!anilist::parseTokenReply(R"({"error":"invalid_grant"})").ok);
        CHECK(!anilist::parseTokenReply(R"({"access_token":""})").ok);
        CHECK(!anilist::parseTokenReply("<html>captive portal</html>").ok);
        CHECK(!anilist::parseTokenReply("[]").ok);
        CHECK(!anilist::parseTokenReply(QByteArray()).ok);
        CHECK(anilist::parseTokenReply(R"({"error":"x"})").accessToken.isEmpty());

        // A refresh that omits the refresh token is legal; the caller keeps the old one. ok stays true.
        t = anilist::parseTokenReply(R"({"access_token":"CCC","expires_in":"3600"})");
        CHECK(t.ok);
        CHECK(t.refreshToken.isEmpty());
        CHECK(t.expiresInSec == 3600);   // a stringified number reads too (proxies do this)
    }

    // ===== §3  credential hygiene: a byte-scan of everything this feature writes =======================
    // The rule is not "the secret is stored safely", it is "the secret is stored in exactly one place, and
    // that place is carved out of sync". So: write the whole feature's state — credentials, tokens, a
    // queue, a last-error line, a link — and then read the INI FILE BACK AS BYTES.
    {
        QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                    QSettings::IniFormat);
        s.setValue(clientIdKey(Id::AniList), QString::fromLatin1(kFixtureClientId));
        s.setValue(clientSecretKey(Id::AniList), QString::fromLatin1(kFixtureSecret));
        s.setValue(accessKey(Id::AniList), QStringLiteral("ACCESS-TOKEN-FIXTURE"));

        Update u;
        u.itemKey = QStringLiteral("marks:series:berserk");
        u.mediaId = QStringLiteral("30002");
        u.kind = Kind::Manga;
        u.unit = 12;
        u.atMs = 1700000000000LL;
        s.setValue(queueKey(QString(), Id::AniList), QString::fromUtf8(encodeQueue({ u })));
        s.setValue(lastErrorKey(QString(), Id::AniList),
                   QStringLiteral("AniList did not accept the update; it is queued and will be retried."));
        TrackerLinks::set(Id::AniList, u.itemKey, u.mediaId, Kind::Manga, QStringLiteral("Berserk"), 364);
        s.sync();

        QFile f(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile));
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray ini = f.readAll();
        f.close();
        CHECK(!ini.isEmpty());   // a scan of nothing passes trivially; assert the corpus first

        // The secret occurs EXACTLY ONCE in the whole file…
        int occurrences = 0;
        for (int at = 0; (at = ini.indexOf(kFixtureSecret, at)) >= 0; ++at) ++occurrences;
        CHECK(occurrences == 1);
        // …and the line it is on is the device-local client-secret key. Not "some tracker key" — the exact
        // one, because trackerstate/ and trackerlink/ are also under a "tracker"-ish name and only one of
        // them is excluded from sync.
        const int at = ini.indexOf(kFixtureSecret);
        const int lineStart = ini.lastIndexOf('\n', at) + 1;
        const QByteArray line = ini.mid(lineStart, at - lineStart);
        // QSettings splits "tracker/anilist/clientSecret" into a [tracker] section with the rest as the key,
        // so match on the leaf rather than the full path.
        CHECK(line.contains("clientSecret"));
        // And the classifier agrees about the key that line spells.
        CHECK(isDeviceLocalKey(clientSecretKey(Id::AniList)));

        // The three artefacts that DO travel — the queue row, the link blob, the user-facing error line —
        // carry neither the secret nor the token.
        const QByteArray q = encodeQueue({ u });
        CHECK(!q.contains(kFixtureSecret));
        CHECK(!q.contains("ACCESS-TOKEN-FIXTURE"));
        const QByteArray blob = TrackerLinks::encode(TrackerLinks::get(Id::AniList, u.itemKey)).toUtf8();
        CHECK(!blob.contains(kFixtureSecret));
        CHECK(!blob.contains("ACCESS-TOKEN-FIXTURE"));
        CHECK(!s.value(lastErrorKey(QString(), Id::AniList)).toString()
                 .contains(QString::fromLatin1(kFixtureSecret)));
        // The GraphQL bodies carry no credential at all: authentication is a header, by construction.
        CHECK(!anilist::searchBody(QStringLiteral("Berserk"), 0, Kind::Manga).contains(kFixtureSecret));
        CHECK(!anilist::saveBody(u, 364).contains(kFixtureSecret));
        CHECK(!anilist::entryBody(QStringLiteral("30002")).contains(kFixtureSecret));
    }

    // ===== §4  search: the body's variables and the reply mapping ======================================
    {
        QJsonObject v = varsOf(anilist::searchBody(QStringLiteral("  Berserk  "), 1989, Kind::Manga));
        CHECK(v.value(QStringLiteral("search")).toString() == QLatin1String("Berserk"));  // trimmed
        CHECK(v.value(QStringLiteral("type")).toString() == QLatin1String("MANGA"));
        CHECK(v.value(QStringLiteral("year")).toInt() == 19890000);   // FuzzyDateInt, yyyymmdd

        v = varsOf(anilist::searchBody(QStringLiteral("Berserk"), 0, Kind::Anime));
        CHECK(v.value(QStringLiteral("type")).toString() == QLatin1String("ANIME"));
        // OMITTED, not zero. A year of 0 as a FuzzyDateInt filters to nothing, so an unknown year would
        // turn every search into "no matches" — the failure that looks exactly like a bad title.
        CHECK(!v.contains(QStringLiteral("year")));

        const QVector<Match> ms = anilist::parseSearch(kSearchReply);
        CHECK(ms.size() == 2);   // the id-less third row is skipped, and does not cost the other two
        if (ms.size() == 2)
        {
            CHECK(ms[0].mediaId == QLatin1String("20605"));
            CHECK(ms[0].title == QLatin1String("My Hero Academia"));   // English preferred
            CHECK(ms[0].altTitle == QLatin1String("Boku no Hero"));     // romaji as the second line
            CHECK(ms[0].year == 2016);
            CHECK(ms[0].kind == Kind::Anime);
            CHECK(ms[0].totalUnits == 13);
            CHECK(ms[0].coverUrl == QLatin1String("https://img/1.jpg"));
            // Only a romaji title: it becomes THE title, and there is no second line to show.
            CHECK(ms[1].title == QLatin1String("Berserk"));
            CHECK(ms[1].altTitle.isEmpty());
            // The kind comes from WHICH COUNT the row carries, not from what we asked for — a search that
            // guessed wrong still files the match under what AniList says it is.
            CHECK(ms[1].kind == Kind::Manga);
            CHECK(ms[1].totalUnits == 364);
        }
        // Totality.
        CHECK(anilist::parseSearch(kGraphQlError).isEmpty());
        CHECK(anilist::parseSearch("<html>nope</html>").isEmpty());
        CHECK(anilist::parseSearch(QByteArray()).isEmpty());
        CHECK(anilist::parseSearch(R"({"data":{"Page":{"media":[]}}})").isEmpty());
    }

    // ===== §5  the account's entry ====================================================================
    {
        CHECK(varsOf(anilist::entryBody(QStringLiteral("30002")))
                  .value(QStringLiteral("mediaId")).toInt() == 30002);

        Entry e;
        CHECK(anilist::parseEntry(kEntryReply, QStringLiteral("30002"), e));
        CHECK(e.exists);
        CHECK(e.progress == 12);
        CHECK(e.status == Status::Current);
        CHECK(e.score == 85);
        CHECK(e.totalUnits == 364);
        CHECK(e.mediaId == QLatin1String("30002"));

        // "Asked, and the account has no row" — TRUE (the ask worked), exists FALSE. A caller that
        // collapsed these two would treat a failed request as "you have read nothing" and push over it.
        Entry e2;
        CHECK(anilist::parseEntry(kEntryUnlisted, QStringLiteral("30002"), e2));
        CHECK(!e2.exists);
        CHECK(e2.progress == 0);
        CHECK(e2.totalUnits == 364);

        Entry e3;
        CHECK(!anilist::parseEntry(kGraphQlError, QStringLiteral("30002"), e3));
        CHECK(!anilist::parseEntry("<html>", QStringLiteral("30002"), e3));
        CHECK(!anilist::parseEntry(R"({"data":{}})", QStringLiteral("30002"), e3));

        CHECK(anilist::statusFromToken(QStringLiteral("COMPLETED")) == Status::Completed);
        CHECK(anilist::statusFromToken(QStringLiteral("REPEATING")) == Status::Repeating);
        // An unknown status reads as Current — the safest wrong answer, being the one a push overwrites
        // with the same value.
        CHECK(anilist::statusFromToken(QStringLiteral("SOMETHING_NEW")) == Status::Current);
        CHECK(anilist::statusToken(Status::Completed) == QLatin1String("COMPLETED"));
    }

    // ===== §6  the push mutation — the three account-damaging rules ====================================
    {
        Update u;
        u.itemKey = QStringLiteral("marks:series:berserk");
        u.mediaId = QStringLiteral("30002");
        u.kind = Kind::Manga;
        u.unit = 12;

        QJsonObject v = varsOf(anilist::saveBody(u, 364));
        CHECK(v.value(QStringLiteral("mediaId")).toInt() == 30002);
        CHECK(v.value(QStringLiteral("progress")).toInt() == 12);
        CHECK(v.value(QStringLiteral("status")).toString() == QLatin1String("CURRENT"));
        // RULE 1: no rating, no scoreRaw. Present-with-0 would erase a score the user set on AniList.
        CHECK(!v.contains(QStringLiteral("scoreRaw")));

        u.hasScore = true;
        u.score = 85;
        v = varsOf(anilist::saveBody(u, 364));
        CHECK(v.contains(QStringLiteral("scoreRaw")));
        CHECK(v.value(QStringLiteral("scoreRaw")).toInt() == 85);
        // Clamped into AniList's POINT_100 range rather than sent raw.
        u.score = 500;
        CHECK(varsOf(anilist::saveBody(u, 364)).value(QStringLiteral("scoreRaw")).toInt() == 100);
        u.score = -3;
        CHECK(varsOf(anilist::saveBody(u, 364)).value(QStringLiteral("scoreRaw")).toInt() == 0);
        u.hasScore = false;

        // RULE 2: COMPLETED needs BOTH the caller's claim and the tracker's own count.
        u.completes = true;
        u.unit = 12;
        v = varsOf(anilist::saveBody(u, 364));
        CHECK(v.value(QStringLiteral("status")).toString() == QLatin1String("CURRENT"));  // 12 of 364
        u.unit = 364;
        v = varsOf(anilist::saveBody(u, 364));
        CHECK(v.value(QStringLiteral("status")).toString() == QLatin1String("COMPLETED"));
        // A tracker with no count (an ongoing series) defers to the caller — otherwise nothing ongoing
        // could ever be completed.
        u.unit = 40;
        CHECK(varsOf(anilist::saveBody(u, 0)).value(QStringLiteral("status")).toString()
              == QLatin1String("COMPLETED"));
        // …and a caller that does NOT claim completion never gets COMPLETED, however the counts line up.
        u.completes = false;
        u.unit = 364;
        CHECK(varsOf(anilist::saveBody(u, 364)).value(QStringLiteral("status")).toString()
              == QLatin1String("CURRENT"));

        // RULE 3: progress is never 0 or negative. A 0 tells the account you have read nothing.
        u.unit = 0;
        CHECK(varsOf(anilist::saveBody(u, 364)).value(QStringLiteral("progress")).toInt() == 1);
        u.unit = -5;
        CHECK(varsOf(anilist::saveBody(u, 364)).value(QStringLiteral("progress")).toInt() == 1);

        // The document really is the SaveMediaListEntry mutation, and it really does name scoreRaw as a
        // variable — a mutation missing the declaration would 400 for every rated push only.
        const QByteArray body = anilist::saveBody(u, 364);
        CHECK(body.contains("SaveMediaListEntry"));
        CHECK(body.contains("$scoreRaw: Int"));
        CHECK(body.contains("MediaListStatus"));
    }

    // ===== §7  the debounce, on a FAKE CLOCK ===========================================================
    // No wall clock anywhere: `now` is an argument, so the 30-second window is asserted at its edges rather
    // than by sleeping through it.
    {
        const qint64 t0 = 1'700'000'000'000LL;
        CHECK(kDebounceMs == 30000);
        // A first push is never delayed.
        CHECK(debounceAllows(0, t0));
        CHECK(debounceAllows(-1, t0));
        // Inside the window: refused. At the boundary: allowed (the test is >=, so nothing is held one tick
        // past the moment it becomes eligible).
        CHECK(!debounceAllows(t0, t0));
        CHECK(!debounceAllows(t0, t0 + 1));
        CHECK(!debounceAllows(t0, t0 + kDebounceMs - 1));
        CHECK(debounceAllows(t0, t0 + kDebounceMs));
        CHECK(debounceAllows(t0, t0 + kDebounceMs + 1));
        // A clock that went BACKWARDS must not suspend pushing. The stamp is wall clock (it has to survive
        // a restart), so an NTP correction or an ini written by a fast-clocked peer really does produce
        // this — and the naive subtraction would sit on the queue for hours.
        CHECK(debounceAllows(t0 + 3600'000, t0));
    }

    // ===== §8  the offline queue: coalescing, the cap, and the round trip ==============================
    {
        auto mk = [](const char* key, int unit, bool done = false) {
            Update u;
            u.itemKey = QString::fromLatin1(key);
            u.mediaId = QStringLiteral("30002");
            u.kind = Kind::Manga;
            u.unit = unit;
            u.completes = done;
            u.atMs = 1'700'000'000'000LL + unit;
            return u;
        };

        QVector<Update> q;
        CHECK(coalesce(q, mk("a", 3)));
        CHECK(q.size() == 1);
        // A FURTHER update for the same item REPLACES it — a binge-read does not grow a row per page turn.
        CHECK(coalesce(q, mk("a", 7)));
        CHECK(q.size() == 1);
        CHECK(q[0].unit == 7);
        // An EARLIER one arriving late is dropped, and reports that nothing changed so the caller does not
        // rewrite the ini for it. This is the clause that stops the queue delivering a regression.
        CHECK(!coalesce(q, mk("a", 5)));
        CHECK(q[0].unit == 7);
        // At the SAME unit, the one that completes the series wins: it carries a status transition.
        CHECK(coalesce(q, mk("a", 7, true)));
        CHECK(q[0].completes);
        CHECK(!coalesce(q, mk("a", 7, false)));   // …and does not lose it again
        CHECK(q[0].completes);
        // A different item is a different row.
        CHECK(coalesce(q, mk("b", 1)));
        CHECK(q.size() == 2);
        // An update with no link is not queueable at all.
        Update noLink = mk("c", 4);
        noLink.mediaId.clear();
        CHECK(!coalesce(q, noLink));
        CHECK(q.size() == 2);

        // The cap drops from the FRONT: the newest progress is the progress still worth delivering.
        QVector<Update> big;
        for (int i = 0; i < kMaxQueued + 7; ++i) big.push_back(mk("x", i));
        CHECK(big.size() == kMaxQueued + 7);
        CHECK(applyQueueCap(big) == 7);
        CHECK(big.size() == kMaxQueued);
        CHECK(big.first().unit == 7);          // the seven oldest went
        CHECK(big.last().unit == kMaxQueued + 6);
        CHECK(applyQueueCap(big) == 0);        // idempotent at the cap

        // The round trip is what makes an offline session replayable after a restart.
        QVector<Update> rated = q;
        rated[0].hasScore = true;
        rated[0].score = 90;
        const QVector<Update> back = decodeQueue(encodeQueue(rated));
        CHECK(back.size() == rated.size());
        if (back.size() == rated.size())
        {
            CHECK(back[0].itemKey == rated[0].itemKey);
            CHECK(back[0].mediaId == rated[0].mediaId);
            CHECK(back[0].unit == rated[0].unit);
            CHECK(back[0].completes == rated[0].completes);
            CHECK(back[0].kind == Kind::Manga);
            CHECK(back[0].atMs == rated[0].atMs);
            CHECK(back[0].hasScore);
            CHECK(back[0].score == 90);
            // "No score" round-trips as ABSENT, not as 0 — the same distinction §6 rule 1 turns on, held
            // across a restart. A sentinel on disk would be one edit away from being pushed as a rating.
            CHECK(!back[1].hasScore);
        }
        CHECK(!encodeQueue({ q[1] }).contains("\"score\""));

        // decode is TOTAL: one unusable row is dropped, the rest survive, and rubbish yields nothing.
        const QVector<Update> mixed = decodeQueue(
            R"([{"key":"a","media":"1","unit":2},{"key":"","media":"1","unit":3},)"
            R"({"media":"2","unit":4},{"key":"b","media":"","unit":5},{"key":"c","media":"3","unit":6}])");
        CHECK(mixed.size() == 2);
        CHECK(decodeQueue("<html>").isEmpty());
        CHECK(decodeQueue("{}").isEmpty());
        CHECK(decodeQueue(QByteArray()).isEmpty());
    }

    // ===== §9  the pull: furthest wins, both directions =================================================
    {
        CHECK(reconcile(5, 5) == Reconcile::Nothing);
        CHECK(reconcile(3, 9) == Reconcile::AdvanceLocal);   // read on a phone; we catch up
        CHECK(reconcile(9, 3) == Reconcile::PushRemote);     // read here; the tracker catches up
        CHECK(reconcile(0, 0) == Reconcile::Nothing);
        CHECK(reconcile(0, 1) == Reconcile::AdvanceLocal);
        CHECK(reconcile(1, 0) == Reconcile::PushRemote);
        // NEITHER side is ever regressed: there is no verdict that lowers anything, which is the whole of
        // #136's rule and the reason this returns three values rather than a number.
        // Corrupt state is clamped rather than trusted — a negative "how far through" is not a direction,
        // and treating it as behind would push a wrong number into somebody's account.
        CHECK(reconcile(-4, 0) == Reconcile::Nothing);
        CHECK(reconcile(0, -4) == Reconcile::Nothing);
        CHECK(reconcile(-4, 6) == Reconcile::AdvanceLocal);
        CHECK(reconcile(6, -4) == Reconcile::PushRemote);
    }

    // ===== §10  identity: which chapter, which episode ==================================================
    {
        // A "ch"-marked number beats a bare one, so a volume number never masquerades as a chapter.
        CHECK(chapterNumberFromTitle(QStringLiteral("Vol. 2 · Ch. 14"), -1) == 14);
        CHECK(chapterNumberFromTitle(QStringLiteral("Chapter 7"), -1) == 7);
        CHECK(chapterNumberFromTitle(QStringLiteral("Ch.140 - The Fall"), -1) == 140);
        CHECK(chapterNumberFromTitle(QStringLiteral("c12"), -1) == 12);
        CHECK(chapterNumberFromTitle(QStringLiteral("Ch 8.5"), -1) == 8);   // a half-chapter is chapter 8
        // A bare number is the FIRST one, not the last: "Chapter 5 of 200" is 5.
        CHECK(chapterNumberFromTitle(QStringLiteral("007"), -1) == 7);
        CHECK(chapterNumberFromTitle(QStringLiteral("Chapter 5 of 200"), -1) == 5);
        // No number at all falls back to what the caller knows, rather than guessing 0 and regressing.
        CHECK(chapterNumberFromTitle(QStringLiteral("Epilogue"), 42) == 42);
        CHECK(chapterNumberFromTitle(QString(), 42) == 42);

        // The episode stream id the video completion path already holds.
        CHECK(episodeFromStreamId(QStringLiteral("tt1234567:2:7")) == 7);
        CHECK(seriesFromStreamId(QStringLiteral("tt1234567:2:7")) == QLatin1String("tt1234567"));
        // A MOVIE id names no episode, and must not be mistaken for episode 0 of something.
        CHECK(episodeFromStreamId(QStringLiteral("tt1234567")) == 0);
        CHECK(seriesFromStreamId(QStringLiteral("tt1234567")).isEmpty());
        CHECK(episodeFromStreamId(QStringLiteral("tt1:2:notanumber")) == 0);
        CHECK(episodeFromStreamId(QString()) == 0);

        // THE KEY EVERY ENTRY POINT AGREES ON. Three surfaces reach this feature holding three different
        // handles on "this series", and if they disagreed the reader would prompt for a manga the detail
        // view had already linked. Every episode of one show collapses to the SAME key…
        CHECK(itemKeyFor(QStringLiteral("tt1234567:1:1"), QStringLiteral("Anything"))
              == itemKeyFor(QStringLiteral("tt1234567:4:9"), QStringLiteral("Anything Else")));
        // …and that key is the show id itself, which is what the Trakt watched-history import writes its
        // marks under — so the two integrations key a show the same way rather than nearly the same way.
        CHECK(itemKeyFor(QStringLiteral("tt1234567:1:1"), QString()) == QLatin1String("tt1234567"));
        // A bare id with no episode part is its own key.
        CHECK(itemKeyFor(QStringLiteral("tt1234567"), QStringLiteral("A Film")) == QLatin1String("tt1234567"));
        // A manga has no id, so the title is the key — normalised, so the reader's "Berserk" and the detail
        // page's " berserk " are one series and not two.
        CHECK(itemKeyFor(QString(), QStringLiteral("Berserk")) == QLatin1String("title:berserk"));
        CHECK(itemKeyFor(QString(), QStringLiteral("  BERSERK  "))
              == itemKeyFor(QString(), QStringLiteral("berserk")));
        // The prefix is what stops a title ever being mistaken for an id.
        CHECK(itemKeyFor(QString(), QStringLiteral("tt1234567")) != QLatin1String("tt1234567"));
        // No identity at all -> no key, and the callers all treat an empty key as "do nothing".
        CHECK(itemKeyFor(QString(), QString()).isEmpty());
        CHECK(itemKeyFor(QString(), QStringLiteral("   ")).isEmpty());
    }

    // ===== §11  the link store: round trip, the husk, and the "don't ask" memory =======================
    {
        int hookFired = 0;
        TrackerLinks::setChangeHook([&hookFired] { ++hookFired; });

        const QString key = QStringLiteral("marks:series:tt1234567");
        // Nothing stored: not linked, and the prompt IS offered.
        CHECK(!TrackerLinks::get(Id::AniList, key).linked());
        CHECK(TrackerLinks::shouldPrompt(Id::AniList, key));

        TrackerLinks::set(Id::AniList, key, QStringLiteral("20605"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);
        CHECK(hookFired == 1);   // the sync push is armed exactly once per real change
        TrackerLinks::Link l = TrackerLinks::get(Id::AniList, key);
        CHECK(l.linked());
        CHECK(l.mediaId == QLatin1String("20605"));
        CHECK(l.kind == Kind::Anime);
        CHECK(l.title == QLatin1String("My Hero Academia"));
        CHECK(l.totalUnits == 13);
        CHECK(l.updatedAt > 0);
        // Linked: the prompt is not offered again.
        CHECK(!TrackerLinks::shouldPrompt(Id::AniList, key));
        // The blob round-trips through its own codec unchanged.
        const TrackerLinks::Link rt = TrackerLinks::decode(TrackerLinks::encode(l));
        CHECK(rt.mediaId == l.mediaId);
        CHECK(rt.kind == l.kind);
        CHECK(rt.title == l.title);
        CHECK(rt.totalUnits == l.totalUnits);
        CHECK(rt.updatedAt == l.updatedAt);

        // The app's own side of the reconciliation. MONOTONIC: it only ever rises, so re-reading chapter 3
        // of a series you have finished cannot make the next pull push a lower number at the tracker.
        CHECK(TrackerLinks::get(Id::AniList, key).localUnits == 0);
        CHECK(TrackerLinks::noteLocalProgress(Id::AniList, key, 4));
        CHECK(TrackerLinks::get(Id::AniList, key).localUnits == 4);
        CHECK(hookFired == 2);
        CHECK(!TrackerLinks::noteLocalProgress(Id::AniList, key, 3));   // a lower unit changes nothing…
        CHECK(!TrackerLinks::noteLocalProgress(Id::AniList, key, 4));   // …and neither does the same one
        CHECK(TrackerLinks::get(Id::AniList, key).localUnits == 4);
        CHECK(hookFired == 2);                                          // …so no sync push is armed for it
        CHECK(TrackerLinks::noteLocalProgress(Id::AniList, key, 9));
        CHECK(TrackerLinks::get(Id::AniList, key).localUnits == 9);
        CHECK(hookFired == 3);
        // The reconciliation, driven off the two numbers the store and the tracker really hold.
        CHECK(reconcile(TrackerLinks::get(Id::AniList, key).localUnits, 12) == Reconcile::AdvanceLocal);
        CHECK(reconcile(TrackerLinks::get(Id::AniList, key).localUnits, 2) == Reconcile::PushRemote);
        CHECK(reconcile(TrackerLinks::get(Id::AniList, key).localUnits, 9) == Reconcile::Nothing);
        // An UNLINKED item has nothing for a progress number to be the progress of.
        CHECK(!TrackerLinks::noteLocalProgress(Id::AniList, QStringLiteral("never-linked"), 3));
        CHECK(!TrackerLinks::noteLocalProgress(Id::AniList, key, 0));
        CHECK(!TrackerLinks::noteLocalProgress(Id::AniList, key, -2));

        // The SAME item on a DIFFERENT tracker is a different row. The tracker token is hashed into the
        // leaf, so nothing MyAnimeList stores can be read back as an AniList link.
        CHECK(TrackerLinks::hashFor(Id::AniList, key) != TrackerLinks::hashFor(Id::MyAnimeList, key));
        CHECK(!TrackerLinks::get(Id::MyAnimeList, key).linked());

        // UNLINK writes a husk, not a deletion — a peer holding the old link must lose to a newer record
        // rather than re-merge its copy back in. And unlinking is NOT refusing: the prompt returns.
        TrackerLinks::clear(Id::AniList, key);
        CHECK(hookFired == 4);
        l = TrackerLinks::get(Id::AniList, key);
        CHECK(!l.linked());
        CHECK(!l.declined);
        CHECK(l.updatedAt > 0);   // the husk carries a timestamp, which is what makes it beat the old row
        CHECK(TrackerLinks::shouldPrompt(Id::AniList, key));
        // Unlinking twice writes nothing and arms no sync push.
        TrackerLinks::clear(Id::AniList, key);
        CHECK(hookFired == 4);

        // "Don't ask about this one" survives, and is not re-offered.
        TrackerLinks::decline(Id::AniList, key);
        CHECK(hookFired == 5);
        CHECK(TrackerLinks::get(Id::AniList, key).declined);
        CHECK(!TrackerLinks::shouldPrompt(Id::AniList, key));
        TrackerLinks::decline(Id::AniList, key);
        CHECK(hookFired == 5);   // idempotent
        // …until the user links it by hand, which is them answering the question they declined.
        TrackerLinks::set(Id::AniList, key, QStringLiteral("30002"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        CHECK(!TrackerLinks::get(Id::AniList, key).declined);

        // An item with no identity has nowhere to remember anything, and is never prompted about.
        TrackerLinks::set(Id::AniList, QString(), QStringLiteral("1"), Kind::Anime, QStringLiteral("x"), 1);
        CHECK(!TrackerLinks::shouldPrompt(Id::AniList, QString()));
        CHECK(!TrackerLinks::get(Id::AniList, QString()).linked());
        // A malformed blob reads back as "no link", never as a wild media id.
        CHECK(!TrackerLinks::decode(QStringLiteral("<not json>")).linked());
        CHECK(!TrackerLinks::decode(QString()).linked());

        TrackerLinks::setChangeHook(nullptr);
    }

    // ===== §12  MyAnimeList: OAuth, PKCE and the two grant bodies ======================================
    // MAL's flow is NOT AniList's: PKCE is required, the only challenge method it accepts is `plain`, and
    // both grants are FORM-encoded rather than JSON. Everything below is fixture-driven — no MyAnimeList
    // account was created, no API client was registered, and nothing here or in the live drive contacted
    // MyAnimeList.
    {
        CHECK(mal::defaultApiUrl() == QLatin1String("https://api.myanimelist.net/v2"));
        CHECK(mal::defaultAuthBase() == QLatin1String("https://myanimelist.net/v1/oauth2"));

        // ---- the verifier. Length AND alphabet; MAL refuses the whole request for either.
        CHECK(mal::kVerifierMinChars == 43);
        CHECK(mal::kVerifierMaxChars == 128);
        CHECK(!mal::isValidCodeVerifier(QString()));
        CHECK(!mal::isValidCodeVerifier(QString(42, QLatin1Char('a'))));   // one short of the floor
        CHECK(mal::isValidCodeVerifier(QString(43, QLatin1Char('a'))));
        CHECK(mal::isValidCodeVerifier(QString(128, QLatin1Char('a'))));
        CHECK(!mal::isValidCodeVerifier(QString(129, QLatin1Char('a'))));  // one past the ceiling
        // Outside RFC 7636's unreserved set. A '+' or a '/' here is what a naive base64 verifier produces,
        // and it is refused by MAL as a malformed request rather than as a bad verifier — which presents to
        // the user as "sign-in failed" with nothing to go on.
        CHECK(!mal::isValidCodeVerifier(QString(43, QLatin1Char('a')) + QLatin1Char('+')));
        CHECK(!mal::isValidCodeVerifier(QString(43, QLatin1Char('a')) + QLatin1Char('/')));
        CHECK(!mal::isValidCodeVerifier(QString(43, QLatin1Char('a')) + QLatin1Char('=')));
        CHECK(!mal::isValidCodeVerifier(QString(43, QLatin1Char('a')) + QLatin1Char(' ')));
        // ...and every verifier we GENERATE is one we would accept, and no two are the same. 200 draws is
        // enough to catch a generator that had been reduced to a constant or to a short alphabet.
        QSet<QString> seen;
        for (int i = 0; i < 200; ++i)
        {
            const QString v = mal::makeCodeVerifier();
            CHECK(mal::isValidCodeVerifier(v));
            seen.insert(v);
        }
        CHECK(seen.size() == 200);

        // ---- the browser URL.
        const QString redirect = QStringLiteral("http://127.0.0.1:51423");
        const QString verifier = QStringLiteral("VERIFIER-0123456789012345678901234567890123456789");
        const QString url = mal::authorizeUrl(mal::defaultAuthBase(),
                                              QString::fromLatin1(kMalClientId), redirect,
                                              verifier, QStringLiteral("STATE42"));
        CHECK(url.startsWith(QLatin1String("https://myanimelist.net/v1/oauth2/authorize?")));
        CHECK(url.contains(QLatin1String("response_type=code")));
        CHECK(url.contains(QLatin1String("client_id=fixture-mal-27310")));
        // PLAIN is the only method MAL accepts, and in plain the challenge IS the verifier.
        CHECK(url.contains(QLatin1String("code_challenge_method=plain")));
        CHECK(url.contains(QLatin1String("code_challenge=") + verifier));
        // The CSRF value has to be IN the URL, or there is nothing to compare on the way back.
        CHECK(url.contains(QLatin1String("state=STATE42")));
        CHECK(QUrl::fromPercentEncoding(url.toUtf8()).contains(redirect));
        // No secret is ever in a browser URL.
        CHECK(!url.contains(QString::fromLatin1(kMalSecret)));

        // ---- the two grants. FORM, not JSON: a JSON body here is refused outright by MAL.
        const QByteArray ex = mal::tokenExchangeBody(QString::fromLatin1(kMalClientId),
                                                     QString::fromLatin1(kMalSecret),
                                                     redirect, QStringLiteral("THE-CODE"), verifier);
        CHECK(!ex.startsWith('{'));                       // NOT JSON
        CHECK(ex.contains("grant_type=authorization_code"));
        CHECK(ex.contains("code=THE-CODE"));
        CHECK(ex.contains("code_verifier=" + verifier.toUtf8()));   // PKCE, or MAL refuses the exchange
        CHECK(ex.contains("client_id=fixture-mal-27310"));
        // The redirect is percent-encoded into the body rather than sent raw — a bare "://" in a form field
        // is what makes a server read the value as truncated.
        CHECK(ex.contains("redirect_uri=http%3A%2F%2F127.0.0.1%3A51423"));
        // The secret is in the POST BODY (over TLS) and in no other artefact this feature produces.
        CHECK(ex.contains(QByteArray("client_secret=") + kMalSecret));

        // A PUBLIC client has no secret at all, and MAL refuses a present-but-blank client_secret. Absent,
        // not empty.
        const QByteArray pub = mal::tokenExchangeBody(QString::fromLatin1(kMalClientId), QString(),
                                                      redirect, QStringLiteral("THE-CODE"), verifier);
        CHECK(!pub.contains("client_secret"));
        CHECK(pub.contains("client_id=fixture-mal-27310"));

        const QByteArray rf = mal::tokenRefreshBody(QString::fromLatin1(kMalClientId),
                                                    QString::fromLatin1(kMalSecret),
                                                    QStringLiteral("RTOKEN"));
        CHECK(rf.contains("grant_type=refresh_token"));
        CHECK(rf.contains("refresh_token=RTOKEN"));
        CHECK(!rf.contains("code="));            // a refresh carries no authorization code...
        CHECK(!rf.contains("code_verifier"));    // ...and no verifier

        // ---- the token reply.
        mal::TokenReply t = mal::parseTokenReply(
            R"({"token_type":"Bearer","expires_in":2678400,"access_token":"MAL-AAA","refresh_token":"MAL-BBB"})");
        CHECK(t.ok);
        CHECK(t.accessToken == QLatin1String("MAL-AAA"));
        CHECK(t.refreshToken == QLatin1String("MAL-BBB"));
        CHECK(t.expiresInSec == 2678400);

        // THE ONE THAT PERMANENTLY UNLINKS AN ACCOUNT, in MAL's spelling. Each of these is a body that is
        // not a token reply, and each must come back ok=false so the caller stores nothing over the live
        // tokens.
        CHECK(!mal::parseTokenReply(R"({"error":"invalid_request","message":"code is invalid"})").ok);
        CHECK(!mal::parseTokenReply(R"({"access_token":""})").ok);
        CHECK(!mal::parseTokenReply("<html>captive portal</html>").ok);
        CHECK(!mal::parseTokenReply("[]").ok);
        CHECK(!mal::parseTokenReply(QByteArray()).ok);
        CHECK(mal::parseTokenReply(R"({"error":"x"})").accessToken.isEmpty());
        // MAL rotates the refresh token, but a reply that omits it is still a good reply — the caller keeps
        // the old one rather than blanking it.
        t = mal::parseTokenReply(R"({"access_token":"MAL-CCC","expires_in":"3600"})");
        CHECK(t.ok);
        CHECK(t.refreshToken.isEmpty());
        CHECK(t.expiresInSec == 3600);
    }

    // ===== §13  MyAnimeList: search, pagination, the entry, and the push ===============================
    {
        const QString api = QStringLiteral("https://api.myanimelist.net/v2");

        // ---- search. The URL IS a wire format on a REST API: it is built where a probe can read it.
        CHECK(mal::kMinQueryChars == 3);
        CHECK(!mal::searchable(QStringLiteral("ab")));
        CHECK(!mal::searchable(QStringLiteral("  a  ")));
        CHECK(mal::searchable(QStringLiteral("abc")));
        // Too short to ask: an EMPTY url, so the caller answers "no matches" rather than sending MAL a
        // request it answers 400. A two-character query is also not one anybody could be confident about.
        CHECK(mal::searchUrl(api, QStringLiteral("ab"), Kind::Anime, 8).isEmpty());

        const QString su = mal::searchUrl(api, QStringLiteral("  My Hero  "), Kind::Anime, 8);
        CHECK(su.startsWith(QLatin1String("https://api.myanimelist.net/v2/anime?")));
        CHECK(su.contains(QLatin1String("q=My%20Hero")));   // trimmed AND encoded; a raw space truncates it
        CHECK(su.contains(QLatin1String("limit=8")));
        // MAL returns ONLY the fields asked for, so a missing count here is a missing COMPLETED rule later.
        CHECK(su.contains(QLatin1String("num_episodes")));
        CHECK(!su.contains(QLatin1Char(' ')));
        const QString sm = mal::searchUrl(api, QStringLiteral("Berserk"), Kind::Manga, 8);
        CHECK(sm.startsWith(QLatin1String("https://api.myanimelist.net/v2/manga?")));
        CHECK(sm.contains(QLatin1String("num_chapters")));

        const QVector<Match> ms = mal::parseSearch(kMalSearchReply, Kind::Anime);
        CHECK(ms.size() == 2);   // the id-less third row is skipped, and does not cost the other two
        if (ms.size() == 2)
        {
            CHECK(ms[0].mediaId == QLatin1String("31964"));
            CHECK(ms[0].title == QLatin1String("My Hero Academia"));      // alternative_titles.en preferred
            CHECK(ms[0].altTitle == QLatin1String("Boku no Hero Academia"));
            CHECK(ms[0].year == 2016);                                    // "2016-04-03" -> 2016
            CHECK(ms[0].kind == Kind::Anime);
            CHECK(ms[0].totalUnits == 13);
            CHECK(ms[0].coverUrl == QLatin1String("https://cdn/1l.jpg")); // large preferred over medium
            // Only one title: it becomes THE title and there is no second line to show.
            CHECK(ms[1].mediaId == QLatin1String("2"));
            CHECK(ms[1].title == QLatin1String("Berserk"));
            CHECK(ms[1].altTitle.isEmpty());
            // WHICH COUNT the row carries is the media type, not what we asked for.
            CHECK(ms[1].kind == Kind::Manga);
            CHECK(ms[1].totalUnits == 364);
            CHECK(ms[1].coverUrl == QLatin1String("https://cdn/2m.jpg")); // medium, when there is no large
        }
        // A row carrying NO count at all (an unaired series) is filed under what the caller asked for
        // rather than guessed at — the endpoint itself is per-kind, so that is the better answer.
        const QVector<Match> unaired = mal::parseSearch(kMalUnairedReply, Kind::Manga);
        CHECK(unaired.size() == 1);
        if (unaired.size() == 1)
        {
            CHECK(unaired[0].kind == Kind::Manga);
            CHECK(unaired[0].totalUnits == 0);
            CHECK(unaired[0].year == 0);      // no start_date at all
        }
        // Totality, and the EMPTY LIST — which is a legitimate answer and not an error.
        CHECK(mal::parseSearch(R"({"data":[],"paging":{}})", Kind::Anime).isEmpty());
        CHECK(mal::parseSearch(kMalErrorReply, Kind::Anime).isEmpty());
        CHECK(mal::parseSearch("<html>nope</html>", Kind::Anime).isEmpty());
        CHECK(mal::parseSearch(QByteArray(), Kind::Anime).isEmpty());

        // ---- pagination, and the reason it is not just "follow the URL".
        CHECK(mal::nextPageUrl(kMalSearchReply, api)
              == QLatin1String("https://api.myanimelist.net/v2/anime?offset=4&q=My%20Hero"));
        CHECK(mal::nextPageUrl(R"({"data":[],"paging":{}})", api).isEmpty());
        CHECK(mal::nextPageUrl(R"({"data":[]})", api).isEmpty());
        CHECK(mal::nextPageUrl("<html>", api).isEmpty());
        // OFF-ORIGIN IS REFUSED. `paging.next` is an absolute URL out of a response body, and the request
        // that follows it carries the account's bearer token. Each of these is a different origin, and the
        // first two are the ones that look right at a glance.
        CHECK(mal::nextPageUrl(R"({"paging":{"next":"https://api.myanimelist.net.evil.test/v2/anime"}})",
                               api).isEmpty());
        CHECK(mal::nextPageUrl(R"({"paging":{"next":"http://api.myanimelist.net/v2/anime"}})",
                               api).isEmpty());
        CHECK(mal::nextPageUrl(R"({"paging":{"next":"https://api.myanimelist.net:8443/v2/anime"}})",
                               api).isEmpty());
        CHECK(mal::nextPageUrl(R"({"paging":{"next":"/v2/anime?offset=4"}})", api).isEmpty());

        // ---- the account's entry. KIND-DEPENDENT in the path AND the fields, which is why the seam
        // carries a Kind here at all: AniList ignores it, MAL answers 404 without it.
        const QString ea = mal::entryUrl(api, QStringLiteral("31964"), Kind::Anime);
        CHECK(ea.startsWith(QLatin1String("https://api.myanimelist.net/v2/anime/31964?")));
        CHECK(ea.contains(QLatin1String("num_episodes")));
        CHECK(ea.contains(QLatin1String("my_list_status")));
        const QString em = mal::entryUrl(api, QStringLiteral("2"), Kind::Manga);
        CHECK(em.startsWith(QLatin1String("https://api.myanimelist.net/v2/manga/2?")));
        CHECK(em.contains(QLatin1String("num_chapters")));
        CHECK(mal::entryUrl(api, QString(), Kind::Anime).isEmpty());

        Entry e;
        CHECK(mal::parseEntry(kMalEntryReply, QStringLiteral("2"), Kind::Manga, e));
        CHECK(e.exists);
        CHECK(e.mediaId == QLatin1String("2"));
        CHECK(e.progress == 12);         // num_chapters_read
        CHECK(e.status == Status::Current);
        CHECK(e.totalUnits == 364);
        // THE SCORE CONVERSION, read side: MAL's 9 is the seam's 90.
        CHECK(e.score == 90);

        // THE ASYMMETRY. MAL REPORTS num_episodes_watched and ACCEPTS num_watched_episodes. Reading the
        // WRITE spelling would report every account as being on episode 0, and reconcile() would answer
        // that by pushing our progress over a list that was already ahead.
        Entry ea2;
        CHECK(mal::parseEntry(kMalEntryAnimeReply, QStringLiteral("31964"), Kind::Anime, ea2));
        CHECK(ea2.exists);
        CHECK(ea2.progress == 7);
        CHECK(ea2.totalUnits == 13);
        CHECK(ea2.status == Status::Completed);

        // "Asked, and the account has no row" — TRUE (the ask worked), exists FALSE. A caller that
        // collapsed these two would treat a failed request as "you have watched nothing" and push over it.
        Entry e2;
        CHECK(mal::parseEntry(kMalEntryUnlisted, QStringLiteral("2"), Kind::Manga, e2));
        CHECK(!e2.exists);
        CHECK(e2.progress == 0);
        CHECK(e2.totalUnits == 364);

        Entry e3;
        CHECK(!mal::parseEntry(kMalErrorReply, QStringLiteral("2"), Kind::Manga, e3));
        CHECK(!mal::parseEntry("<html>", QStringLiteral("2"), Kind::Manga, e3));
        CHECK(!mal::parseEntry(R"({})", QStringLiteral("2"), Kind::Manga, e3));
        CHECK(!mal::parseEntry(R"([])", QStringLiteral("2"), Kind::Manga, e3));

        // ---- the statuses. KIND-DEPENDENT, and a token MAL adds later reads back as Current.
        CHECK(mal::statusToken(Status::Current, Kind::Anime) == QLatin1String("watching"));
        CHECK(mal::statusToken(Status::Current, Kind::Manga) == QLatin1String("reading"));
        CHECK(mal::statusToken(Status::Planning, Kind::Anime) == QLatin1String("plan_to_watch"));
        CHECK(mal::statusToken(Status::Planning, Kind::Manga) == QLatin1String("plan_to_read"));
        CHECK(mal::statusToken(Status::Paused, Kind::Anime) == QLatin1String("on_hold"));
        CHECK(mal::statusToken(Status::Dropped, Kind::Manga) == QLatin1String("dropped"));
        CHECK(mal::statusToken(Status::Completed, Kind::Anime) == QLatin1String("completed"));
        // MAL has no "repeating" status at all — it is a boolean beside an otherwise ordinary `watching`.
        // Mapping it onto `completed` would be a status change nobody asked for.
        CHECK(mal::statusToken(Status::Repeating, Kind::Anime) == QLatin1String("watching"));
        CHECK(mal::statusFromToken(QStringLiteral("on_hold")) == Status::Paused);
        CHECK(mal::statusFromToken(QStringLiteral("plan_to_read")) == Status::Planning);
        CHECK(mal::statusFromToken(QStringLiteral("plan_to_watch")) == Status::Planning);
        CHECK(mal::statusFromToken(QStringLiteral("reading")) == Status::Current);
        CHECK(mal::statusFromToken(QStringLiteral("WATCHING")) == Status::Current);   // case-insensitive
        CHECK(mal::statusFromToken(QStringLiteral("something_new")) == Status::Current);
        CHECK(mal::statusFromToken(QString()) == Status::Current);

        // ---- the score conversion, both ways. ROUNDED, not truncated.
        CHECK(mal::scoreToMal(0) == 0);
        CHECK(mal::scoreToMal(85) == 9);     // 8.5 rounds UP; truncating would silently demote it
        CHECK(mal::scoreToMal(84) == 8);
        CHECK(mal::scoreToMal(100) == 10);
        CHECK(mal::scoreToMal(500) == 10);   // clamped rather than sent out of range
        CHECK(mal::scoreToMal(-3) == 0);
        CHECK(mal::scoreFromMal(0) == 0);
        CHECK(mal::scoreFromMal(9) == 90);
        CHECK(mal::scoreFromMal(10) == 100);
        CHECK(mal::scoreFromMal(37) == 100); // clamped: a value MAL cannot mean is not carried into the app

        // ---- the push. The three account-damaging rules, in MAL's spellings.
        CHECK(mal::saveUrl(api, QStringLiteral("2"), Kind::Manga)
              == QLatin1String("https://api.myanimelist.net/v2/manga/2/my_list_status"));
        CHECK(mal::saveUrl(api, QStringLiteral("31964"), Kind::Anime)
              == QLatin1String("https://api.myanimelist.net/v2/anime/31964/my_list_status"));
        CHECK(mal::saveUrl(api, QString(), Kind::Anime).isEmpty());

        Update u;
        u.itemKey = QStringLiteral("marks:series:berserk");
        u.mediaId = QStringLiteral("2");
        u.kind = Kind::Manga;
        u.unit = 12;

        QByteArray body = mal::saveBody(u, 364);
        CHECK(!body.startsWith('{'));                       // FORM, not JSON
        CHECK(body.contains("status=reading"));             // manga, in progress
        CHECK(body.contains("num_chapters_read=12"));
        // RULE 1: no rating, no score. MAL reads 0 as "no score" and WOULD clear one the user set by hand.
        CHECK(!body.contains("score="));

        u.hasScore = true;
        u.score = 85;
        body = mal::saveBody(u, 364);
        CHECK(body.contains("score=9"));                    // 85/100 -> 9/10
        u.score = 0;
        CHECK(mal::saveBody(u, 364).contains("score=0"));   // an explicit "rated zero" IS sent
        u.hasScore = false;

        // RULE 2: COMPLETED needs BOTH the caller's claim and MAL's own count.
        u.completes = true;
        u.unit = 12;
        CHECK(mal::saveBody(u, 364).contains("status=reading"));      // 12 of 364 is not finished
        u.unit = 364;
        CHECK(mal::saveBody(u, 364).contains("status=completed"));
        // A series MAL has no count for (an ongoing one) defers to the caller, or nothing ongoing could
        // ever be completed.
        u.unit = 40;
        CHECK(mal::saveBody(u, 0).contains("status=completed"));
        // ...and a caller that does NOT claim completion never gets `completed`, however the counts line up.
        u.completes = false;
        u.unit = 364;
        CHECK(mal::saveBody(u, 364).contains("status=reading"));

        // RULE 3: progress is never 0 or negative. A 0 tells the account you have read nothing.
        u.unit = 0;
        CHECK(mal::saveBody(u, 364).contains("num_chapters_read=1"));
        u.unit = -5;
        CHECK(mal::saveBody(u, 364).contains("num_chapters_read=1"));

        // THE ASYMMETRY, write side. `num_watched_episodes` is what MAL ACCEPTS; sending the READ spelling
        // is a 200 that stores nothing, which looks from here like a perfect sync that never happened.
        Update a;
        a.itemKey = QStringLiteral("tt1234567");
        a.mediaId = QStringLiteral("31964");
        a.kind = Kind::Anime;
        a.unit = 7;
        const QByteArray abody = mal::saveBody(a, 13);
        CHECK(abody.contains("num_watched_episodes=7"));
        CHECK(!abody.contains("num_episodes_watched"));     // the READ spelling must NOT be the write one
        CHECK(abody.contains("status=watching"));
        CHECK(!abody.contains("num_chapters_read"));
    }

    // ===== §14  the rate limit: back off, do not race =================================================
    // MAL publishes a limit and answers a breach with 429. Four families of answer need four different
    // responses, and a tight retry against a throttle is how an integration gets its client banned.
    {
        CHECK(mal::kBackoffBaseMs == 60000);
        CHECK(mal::kBackoffMaxMs == 1800000);

        // A success is not a failure and decides nothing.
        CHECK(!mal::backoffFor(200, 0, 1).retry);
        CHECK(!mal::backoffFor(200, 0, 1).permanent);
        CHECK(!mal::backoffFor(204, 0, 1).retry);

        // 429: wait, and wait a MINUTE at least — not a second.
        mal::Backoff b = mal::backoffFor(429, 0, 1);
        CHECK(b.retry);
        CHECK(!b.permanent);
        CHECK(!b.reauth);
        CHECK(b.delayMs == mal::kBackoffBaseMs);
        // Retry-After wins when it asks for LONGER than we would wait...
        CHECK(mal::backoffFor(429, 300, 1).delayMs == 300000);
        // ...and is NOT honoured downward: the base is there to protect the account, not to be the smallest
        // legal wait. A server asking us to come back in one second while rate-limiting us is exactly the
        // case where obeying it is the wrong move.
        CHECK(mal::backoffFor(429, 1, 1).delayMs == mal::kBackoffBaseMs);
        // Doubling per consecutive failure, capped. Nothing here can produce a delay below the base.
        CHECK(mal::backoffFor(429, 0, 2).delayMs == 120000);
        CHECK(mal::backoffFor(429, 0, 3).delayMs == 240000);
        CHECK(mal::backoffFor(429, 0, 30).delayMs == mal::kBackoffMaxMs);
        // A long outage must not overflow the exponent into a negative delay — which would read back as
        // "wait the minimum" and be a tight retry loop arrived at by arithmetic.
        CHECK(mal::backoffFor(429, 0, 1000).delayMs == mal::kBackoffMaxMs);
        CHECK(mal::backoffFor(429, 0, 0).delayMs >= mal::kBackoffBaseMs);
        CHECK(mal::backoffFor(429, -5, 1).delayMs == mal::kBackoffBaseMs);

        // 401 is the TOKEN, not the request: refresh and keep the row.
        b = mal::backoffFor(401, 0, 1);
        CHECK(b.retry);
        CHECK(b.reauth);
        CHECK(!b.permanent);

        // 5xx and "no reply at all" are the same waiting problem.
        CHECK(mal::backoffFor(500, 0, 1).retry);
        CHECK(mal::backoffFor(503, 0, 1).retry);
        CHECK(!mal::backoffFor(503, 0, 1).permanent);
        CHECK(mal::backoffFor(0, 0, 1).retry);
        CHECK(mal::backoffFor(0, 0, 1).delayMs == mal::kBackoffBaseMs);

        // PERMANENT: a media id the account cannot write, or an entry that no longer exists. Never
        // acceptable by waiting, so the row is dropped rather than left to wedge the head of the queue for
        // ever — every later chapter behind it would be lost too.
        for (int code : { 400, 404, 422 })
        {
            const mal::Backoff p = mal::backoffFor(code, 0, 1);
            CHECK(p.permanent);
            CHECK(!p.retry);
        }
        // 403 is deliberately NOT permanent: MAL answers a suspended account and a temporarily-refused
        // client with the same status, and dropping every queued chapter for the second is the worse
        // mistake.
        CHECK(!mal::backoffFor(403, 0, 1).permanent);
        CHECK(mal::backoffFor(403, 0, 1).retry);
        // A 4xx we have no rule for is retried rather than dropped: nobody's progress is thrown away on a
        // status nobody has thought about.
        CHECK(!mal::backoffFor(418, 0, 1).permanent);
        CHECK(mal::backoffFor(418, 0, 1).retry);
    }

    // ===== §15  a match we are not sure of is not written ==============================================
    // The conservatism rule, made into a function. Nothing here links anything; it decides what is worth
    // OFFERING and whether an offer is unambiguous.
    {
        auto mk = [](const char* title, const char* alt = "") {
            Match m;
            m.mediaId = QStringLiteral("1");
            m.title = QString::fromUtf8(title);
            m.altTitle = QString::fromUtf8(alt);
            return m;
        };

        // Exact, once normalised for case and punctuation.
        CHECK(titleConfidence(QStringLiteral("My Hero Academia"), mk("my hero academia")) == 100);
        CHECK(titleConfidence(QStringLiteral("My Hero Academia!"), mk("My  Hero-Academia")) == 100);
        // ...or exact on the SECOND title, which is the whole reason a match carries two.
        CHECK(titleConfidence(QStringLiteral("Boku no Hero Academia"),
                              mk("My Hero Academia", "Boku no Hero Academia")) == 100);
        // One wholly inside the other: strong, but not certain — a season or a year is exactly the
        // difference that makes two list entries two list entries.
        CHECK(titleConfidence(QStringLiteral("Berserk"), mk("Berserk 1997")) == 70);
        // Some words in common but neither inside the other: a partial answer, scaled by the LONGER side so
        // two words of three do not score as though they had answered the whole question.
        const int part = titleConfidence(QStringLiteral("Hero Academia Vigilantes"), mk("My Hero Academia"));
        CHECK(part > 0);
        CHECK(part < 70);
        // ...while a title that merely CONTAINS the query is the stronger "close, but a season or a year
        // apart" case, and scores as that rather than as a word overlap.
        CHECK(titleConfidence(QStringLiteral("Hero Academia"), mk("My Hero Academia Season 4")) == 70);
        // NOT ONE WORD IN COMMON is noise, and noise is what the picker must not be full of.
        CHECK(titleConfidence(QStringLiteral("Berserk"), mk("Sailor Moon")) == 0);
        CHECK(titleConfidence(QString(), mk("Berserk")) == 0);
        CHECK(titleConfidence(QStringLiteral("Berserk"), mk("")) == 0);

        // Ranking: best first, noise dropped, and STABLE among equals.
        QVector<Match> ms;
        ms << mk("Sailor Moon") << mk("Berserk 1997") << mk("Berserk") << mk("Cowboy Bebop");
        const QVector<Match> ranked = rankMatches(QStringLiteral("Berserk"), ms);
        CHECK(ranked.size() == 2);
        if (ranked.size() == 2)
        {
            CHECK(ranked[0].title == QLatin1String("Berserk"));        // exact beats contains
            CHECK(ranked[1].title == QLatin1String("Berserk 1997"));
        }
        // EVERY row noise: handed back UNCHANGED rather than emptied. A title in a script the query is not
        // written in shares no word with it, and answering "nothing found" there would make exactly those
        // series permanently unlinkable — the opposite of what this rule is for.
        QVector<Match> foreign;
        foreign << mk("Kimetsu no Yaiba") << mk("Shingeki no Kyojin");
        const QVector<Match> keptAll = rankMatches(QStringLiteral("Demon Slayer"), foreign);
        CHECK(keptAll.size() == 2);
        CHECK(keptAll[0].title == QLatin1String("Kimetsu no Yaiba"));   // and in the provider's own order
        CHECK(rankMatches(QStringLiteral("Berserk"), QVector<Match>{}).isEmpty());

        // "Sure" means EXACT AND ALONE. Everything else falls through to the user, because a wrong link
        // writes somebody's progress onto the wrong series in a list they curate by hand.
        QVector<Match> one;
        one << mk("Berserk") << mk("Sailor Moon");
        CHECK(confidentMatchIndex(QStringLiteral("Berserk"), one) == 0);
        // An exact hit with a near neighbour is AMBIGUOUS, not certain.
        QVector<Match> ambiguous;
        ambiguous << mk("Berserk") << mk("Berserk 1997");
        CHECK(confidentMatchIndex(QStringLiteral("Berserk"), ambiguous) == -1);
        // Two exact hits are an ambiguous field, not two certainties.
        QVector<Match> twins;
        twins << mk("Berserk") << mk("berserk");
        CHECK(confidentMatchIndex(QStringLiteral("Berserk"), twins) == -1);
        // Nothing exact at all: never sure.
        QVector<Match> loose;
        loose << mk("Berserk 1997") << mk("Berserk Golden Age");
        CHECK(confidentMatchIndex(QStringLiteral("Berserk"), loose) == -1);
        CHECK(confidentMatchIndex(QStringLiteral("Berserk"), QVector<Match>{}) == -1);
    }

    // ===== §16  several trackers at once ================================================================
    // The issue's rule: push to both and let each own its own state. Driven with fake tracker::Tracker
    // implementations — no socket, no account, no network — which is the only way "one failing and the
    // other still landing" is reachable at all.
    {
        const QString key = QStringLiteral("tt77777");
        // Two links for the SAME item, with DIFFERENT media ids: the same series under two ids on two
        // accounts, which is exactly what the link store's (Id, itemKey) keying is for.
        TrackerLinks::clear(Id::AniList, key);
        TrackerLinks::clear(Id::MyAnimeList, key);
        TrackerLinks::set(Id::AniList, key, QStringLiteral("20605"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);
        TrackerLinks::set(Id::MyAnimeList, key, QStringLiteral("31964"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);

        FakeTracker a(Id::AniList), m(Id::MyAnimeList);
        const QVector<Tracker*> both{ &a, &m };

        // BOTH get it, each with ITS OWN media id. Nothing here ever hands one tracker another's.
        TrackerFanout::Result r = TrackerFanout::push(both, key, Kind::Anime, 5, false);
        CHECK(r.pushed == 2);
        CHECK(r.unlinked == 0);
        CHECK(r.needLink.isEmpty());
        CHECK(a.got.size() == 1);
        CHECK(m.got.size() == 1);
        if (a.got.size() == 1 && m.got.size() == 1)
        {
            CHECK(a.got[0].mediaId == QLatin1String("20605"));
            CHECK(m.got[0].mediaId == QLatin1String("31964"));
            CHECK(a.got[0].unit == 5);
            CHECK(m.got[0].unit == 5);
        }
        // Each tracker's own side of the reconciliation moved, independently.
        CHECK(TrackerLinks::get(Id::AniList, key).localUnits == 5);
        CHECK(TrackerLinks::get(Id::MyAnimeList, key).localUnits == 5);

        // ONE FAILING MUST NOT BLOCK THE OTHER — with the failing one FIRST...
        a.refuse = true;
        r = TrackerFanout::push(both, key, Kind::Anime, 6, false);
        CHECK(r.pushed == 2);          // both were VISITED; what one did with it is its own business
        CHECK(a.refusals == 1);
        CHECK(m.got.size() == 2);      // ...and the healthy one still landed
        // ...and with the failing one LAST, because "it worked" can be an artefact of the order.
        const QVector<Tracker*> reversed{ &m, &a };
        r = TrackerFanout::push(reversed, key, Kind::Anime, 7, false);
        CHECK(r.pushed == 2);
        CHECK(a.refusals == 2);
        CHECK(m.got.size() == 3);
        a.refuse = false;

        // A tracker that is OFF is skipped, and says nothing about the other. "Off" is not a failure.
        a.on_ = false;
        r = TrackerFanout::push(both, key, Kind::Anime, 8, false);
        CHECK(r.pushed == 1);
        CHECK(m.got.size() == 4);
        CHECK(TrackerFanout::active(both).size() == 1);
        a.on_ = true;

        // A null in the list is skipped rather than dereferenced.
        const QVector<Tracker*> withNull{ nullptr, &a, nullptr, &m };
        CHECK(TrackerFanout::active(withNull).size() == 2);
        CHECK(TrackerFanout::active(QVector<Tracker*>{ nullptr }).isEmpty());

        // UNLINKED on one tracker only: that one is not pushed to, it is offered a prompt, and the other is
        // pushed to as normal.
        TrackerLinks::clear(Id::MyAnimeList, key);
        const int mBefore = m.got.size();
        r = TrackerFanout::push(both, key, Kind::Anime, 9, false);
        CHECK(r.pushed == 1);
        CHECK(r.unlinked == 1);
        CHECK(r.declined == 0);
        CHECK(r.needLink.size() == 1);
        if (r.needLink.size() == 1) CHECK(r.needLink[0]->id() == Id::MyAnimeList);
        CHECK(m.got.size() == mBefore);   // not pushed to: there is no link for a media id to come from
        // ...AND THE UNLINKED ONE FIRST, which is the ordering that catches a loop that RETURNS on the
        // first tracker it can do nothing for instead of moving on to the next. With the list the other way
        // round the linked tracker is visited first and the bug is invisible.
        const int aBeforeUnlinkedFirst = a.got.size();
        r = TrackerFanout::push(reversed, key, Kind::Anime, 12, false);   // { MyAnimeList (unlinked), AniList }
        CHECK(r.pushed == 1);
        CHECK(r.unlinked == 1);
        CHECK(a.got.size() == aBeforeUnlinkedFirst + 1);
        if (!a.got.isEmpty()) CHECK(a.got.last().unit == 12);
        // ...and DECLINED means not even offered, for ever, until the user asks.
        TrackerLinks::decline(Id::MyAnimeList, key);
        r = TrackerFanout::push(both, key, Kind::Anime, 10, false);
        CHECK(r.unlinked == 1);
        CHECK(r.declined == 1);
        CHECK(r.needLink.isEmpty());

        // Nothing at all happens for an item with no identity, or for a unit that is not progress.
        const int aBefore = a.got.size();
        CHECK(TrackerFanout::push(both, QString(), Kind::Anime, 3, false).pushed == 0);
        CHECK(TrackerFanout::push(both, key, Kind::Anime, 0, false).pushed == 0);
        CHECK(TrackerFanout::push(both, key, Kind::Anime, -1, false).pushed == 0);
        CHECK(a.got.size() == aBefore);

        // THE LINK'S KIND, not the caller's. A series linked as manga on one tracker and anime on the other
        // must be pushed to each as what it IS there, or the write goes to the wrong endpoint.
        TrackerLinks::clear(Id::MyAnimeList, key);
        TrackerLinks::set(Id::MyAnimeList, key, QStringLiteral("2"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        a.got.clear();
        m.got.clear();
        TrackerFanout::push(both, key, Kind::Anime, 11, false);
        CHECK(a.got.size() == 1);
        CHECK(m.got.size() == 1);
        if (a.got.size() == 1 && m.got.size() == 1)
        {
            CHECK(a.got[0].kind == Kind::Anime);
            CHECK(m.got[0].kind == Kind::Manga);
        }
    }

    // ===== §17  the ONE queue, per tracker ==============================================================
    // Tracker.h forbids a second queue, so increment 2 moved the persistence out of AniListTracker into
    // TrackerQueue rather than writing MyAnimeList a queue of its own. Same rules, same keys, one
    // implementation — and keyed by Id, so two accounts do not share a rate limit either.
    {
        TrackerQueue::forgetAccount(Id::AniList);
        TrackerQueue::forgetAccount(Id::MyAnimeList);
        CHECK(TrackerQueue::count(Id::AniList) == 0);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 0);

        Update u;
        u.itemKey = QStringLiteral("marks:series:berserk");
        u.mediaId = QStringLiteral("30002");
        u.kind = Kind::Manga;
        u.unit = 3;
        u.atMs = 1'700'000'000'000LL;

        CHECK(TrackerQueue::enqueue(Id::AniList, u));
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        // SEPARATE. A chapter queued for one account is not queued for the other.
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 0);

        Update um = u;
        um.mediaId = QStringLiteral("2");
        CHECK(TrackerQueue::enqueue(Id::MyAnimeList, um));
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 1);
        // ...and each holds ITS OWN media id after a round trip through the ini.
        CHECK(TrackerQueue::load(Id::AniList).first().mediaId == QLatin1String("30002"));
        CHECK(TrackerQueue::load(Id::MyAnimeList).first().mediaId == QLatin1String("2"));

        // The shared rules are the increment-1 ones, unchanged: furthest wins, and an earlier unit arriving
        // late writes nothing.
        Update later = u;
        later.unit = 9;
        CHECK(TrackerQueue::enqueue(Id::AniList, later));
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        CHECK(TrackerQueue::load(Id::AniList).first().unit == 9);
        Update earlier = u;
        earlier.unit = 4;
        CHECK(!TrackerQueue::enqueue(Id::AniList, earlier));
        CHECK(TrackerQueue::load(Id::AniList).first().unit == 9);

        // TWO ACCOUNTS DO NOT SHARE A RATE LIMIT. A send on one starts that one's debounce and nothing
        // else's — the pin for "a progress update goes to every configured tracker independently".
        const qint64 now = 1'700'000'100'000LL;
        TrackerQueue::noteSent(Id::AniList, u.itemKey, now);
        qint64 wait = -1;
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::AniList), Id::AniList, now, &wait) == -1);
        CHECK(wait > 0);
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::MyAnimeList), Id::MyAnimeList, now, &wait)
              == 0);
        // ...and the window opens on the shared 30-second rule, not on a second one.
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::AniList), Id::AniList,
                                         now + kDebounceMs, &wait) == 0);

        // Delivery drops the row BY IDENTITY, and only on the tracker it was delivered to.
        TrackerQueue::removeDelivered(Id::MyAnimeList, um);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 0);
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        // A FURTHER update coalesced onto the item during the request survives a delivery of the older one.
        Update far = um;
        far.unit = 40;
        CHECK(TrackerQueue::enqueue(Id::MyAnimeList, far));
        TrackerQueue::removeDelivered(Id::MyAnimeList, um);   // the OLD row, unit 3
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 1);
        CHECK(TrackerQueue::load(Id::MyAnimeList).first().unit == 40);

        // The last-error line is per tracker too, and clearing one does not clear the other.
        TrackerQueue::setLastError(Id::AniList, QStringLiteral("A failed"));
        TrackerQueue::setLastError(Id::MyAnimeList, QStringLiteral("M failed"));
        CHECK(TrackerQueue::lastError(Id::AniList) == QLatin1String("A failed"));
        CHECK(TrackerQueue::lastError(Id::MyAnimeList) == QLatin1String("M failed"));
        TrackerQueue::setLastError(Id::AniList, QString());
        CHECK(TrackerQueue::lastError(Id::AniList).isEmpty());
        CHECK(TrackerQueue::lastError(Id::MyAnimeList) == QLatin1String("M failed"));

        // Disconnecting ONE account drops that account's pending progress and nothing else's.
        TrackerQueue::forgetAccount(Id::MyAnimeList);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 0);
        CHECK(TrackerQueue::lastError(Id::MyAnimeList).isEmpty());
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        TrackerQueue::forgetAccount(Id::AniList);
    }

    // ===== §18  ANILIST IS UNCHANGED ====================================================================
    // The floor this increment stands on: with MyAnimeList not configured, AniList behaves exactly as it
    // did. Two halves — the BYTES it puts on the wire, and the STATE it keeps — because a second tracker
    // could break either one without touching a line of AniList's code.
    {
        // ---- the wire. Exact bodies, not shapes: a shared helper "cleaned up" later must not be able to
        // move a byte of what AniList sends.
        CHECK(anilist::defaultApiUrl() == QLatin1String("https://graphql.anilist.co"));
        CHECK(anilist::defaultAuthBase() == QLatin1String("https://anilist.co/api/v2/oauth"));
        CHECK(anilist::tokenExchangeBody(QStringLiteral("cid"), QStringLiteral("sec"),
                                         QStringLiteral("http://127.0.0.1:1"), QStringLiteral("code"))
              == QByteArray(R"({"client_id":"cid","client_secret":"sec","code":"code",)"
                            R"("grant_type":"authorization_code","redirect_uri":"http://127.0.0.1:1"})"));
        CHECK(anilist::tokenRefreshBody(QStringLiteral("cid"), QStringLiteral("sec"),
                                        QStringLiteral("rt"))
              == QByteArray(R"({"client_id":"cid","client_secret":"sec",)"
                            R"("grant_type":"refresh_token","refresh_token":"rt"})"));
        // AniList's push is still GraphQL JSON with AniList's variable names — nothing about MAL's form
        // encoding or its num_watched_episodes leaked into it.
        Update au;
        au.itemKey = QStringLiteral("k");
        au.mediaId = QStringLiteral("30002");
        au.kind = Kind::Manga;
        au.unit = 12;
        const QByteArray asave = anilist::saveBody(au, 364);
        CHECK(asave.startsWith('{'));
        CHECK(asave.contains(R"("variables":{"mediaId":30002,"progress":12,"status":"CURRENT"})"));
        CHECK(!asave.contains("num_chapters_read"));
        CHECK(!asave.contains("status=reading"));
        CHECK(anilist::statusToken(Status::Current) == QLatin1String("CURRENT"));   // not "watching"
        CHECK(anilist::searchBody(QStringLiteral("Berserk"), 0, Kind::Manga)
                  .contains(R"("variables":{"search":"Berserk","type":"MANGA"})"));
        CHECK(anilist::entryBody(QStringLiteral("30002"))
                  .contains(R"("variables":{"mediaId":30002})"));

        // ---- the state. Every key AniList uses is distinct from MyAnimeList's, so nothing MAL stores can
        // be read back as AniList's — the reason Tracker.h RESERVED the id rather than adding it later.
        CHECK(queueKey(QString(), Id::AniList) != queueKey(QString(), Id::MyAnimeList));
        CHECK(queueKey(QString(), Id::AniList).contains(QLatin1String("/anilist/")));
        CHECK(queueKey(QString(), Id::MyAnimeList).contains(QLatin1String("/mal/")));
        CHECK(lastErrorKey(QString(), Id::AniList) != lastErrorKey(QString(), Id::MyAnimeList));
        CHECK(lastSentKey(QString(), Id::AniList, QStringLiteral("k"))
              != lastSentKey(QString(), Id::MyAnimeList, QStringLiteral("k")));
        CHECK(clientIdKey(Id::AniList) != clientIdKey(Id::MyAnimeList));
        CHECK(accessKey(Id::MyAnimeList) == QLatin1String("tracker/mal/access"));
        // MAL's credentials are in the SAME carve-outs AniList's are — device-local, never synced, and the
        // tokens (but not the typed id/secret) out of scope for a settings Discard.
        CHECK(isDeviceLocalKey(clientIdKey(Id::MyAnimeList)));
        CHECK(isDeviceLocalKey(clientSecretKey(Id::MyAnimeList)));
        CHECK(isDeviceLocalKey(accessKey(Id::MyAnimeList)));
        CHECK(isDeviceLocalKey(refreshKey(Id::MyAnimeList)));
        CHECK(isDeviceLocalKey(queueKey(QString(), Id::MyAnimeList)));
        CHECK(isBackgroundStateKey(accessKey(Id::MyAnimeList)));
        CHECK(isBackgroundStateKey(queueKey(QString(), Id::MyAnimeList)));
        CHECK(!isBackgroundStateKey(clientIdKey(Id::MyAnimeList)));
        CHECK(!isBackgroundStateKey(clientSecretKey(Id::MyAnimeList)));

        // ---- and the proof by construction: write EVERYTHING MyAnimeList owns, then read AniList's back.
        QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                    QSettings::IniFormat);
        s.setValue(clientIdKey(Id::AniList), QString::fromLatin1(kFixtureClientId));
        s.setValue(accessKey(Id::AniList), QStringLiteral("ACCESS-TOKEN-FIXTURE"));
        const QString aKey = QStringLiteral("tt-unchanged");
        TrackerLinks::clear(Id::AniList, aKey);
        TrackerLinks::clear(Id::MyAnimeList, aKey);
        TrackerLinks::set(Id::AniList, aKey, QStringLiteral("30002"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        TrackerQueue::forgetAccount(Id::AniList);
        Update qu;
        qu.itemKey = aKey;
        qu.mediaId = QStringLiteral("30002");
        qu.kind = Kind::Manga;
        qu.unit = 6;
        qu.atMs = 1'700'000'000'000LL;
        TrackerQueue::enqueue(Id::AniList, qu);
        s.sync();
        const QString aLinkBefore = TrackerLinks::encode(TrackerLinks::get(Id::AniList, aKey));
        const QByteArray aQueueBefore = encodeQueue(TrackerQueue::load(Id::AniList));

        // Now MyAnimeList arrives, in full: credentials, a token, a link on the SAME item and a queue.
        s.setValue(clientIdKey(Id::MyAnimeList), QString::fromLatin1(kMalClientId));
        s.setValue(clientSecretKey(Id::MyAnimeList), QString::fromLatin1(kMalSecret));
        s.setValue(accessKey(Id::MyAnimeList), QStringLiteral("MAL-ACCESS-TOKEN-FIXTURE"));
        TrackerLinks::set(Id::MyAnimeList, aKey, QStringLiteral("2"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        Update mu = qu;
        mu.mediaId = QStringLiteral("2");
        mu.unit = 99;
        TrackerQueue::enqueue(Id::MyAnimeList, mu);
        TrackerQueue::setLastError(Id::MyAnimeList, QStringLiteral("MyAnimeList is unhappy"));
        s.sync();

        // ...and AniList's side is byte-for-byte what it was.
        CHECK(TrackerLinks::encode(TrackerLinks::get(Id::AniList, aKey)) == aLinkBefore);
        CHECK(encodeQueue(TrackerQueue::load(Id::AniList)) == aQueueBefore);
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        CHECK(TrackerQueue::lastError(Id::AniList).isEmpty());
        CHECK(s.value(accessKey(Id::AniList)).toString() == QLatin1String("ACCESS-TOKEN-FIXTURE"));
        CHECK(s.value(clientIdKey(Id::AniList)).toString() == QString::fromLatin1(kFixtureClientId));

        // ===== the MyAnimeList credential byte-scan =====================================================
        // The same rule §3 holds for AniList, held for MAL: the secret is on disk in EXACTLY ONE place, and
        // that place is inside the device-local carve-out. Nothing this probe prints contains it.
        QFile f(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile));
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray ini = f.readAll();
        f.close();
        CHECK(!ini.isEmpty());   // a scan of nothing passes trivially; assert the corpus first

        int occurrences = 0;
        for (int at = 0; (at = ini.indexOf(kMalSecret, at)) >= 0; ++at) ++occurrences;
        CHECK(occurrences == 1);
        const int at = ini.indexOf(kMalSecret);
        const int lineStart = ini.lastIndexOf('\n', at) + 1;
        CHECK(ini.mid(lineStart, at - lineStart).contains("clientSecret"));
        // The token, likewise: exactly once, on the access key.
        int tokenHits = 0;
        for (int p = 0; (p = ini.indexOf("MAL-ACCESS-TOKEN-FIXTURE", p)) >= 0; ++p) ++tokenHits;
        CHECK(tokenHits == 1);
        // The three artefacts that DO travel or get shown carry neither.
        const QByteArray mq = encodeQueue(TrackerQueue::load(Id::MyAnimeList));
        CHECK(!mq.contains(kMalSecret));
        CHECK(!mq.contains("MAL-ACCESS-TOKEN-FIXTURE"));
        const QByteArray mblob = TrackerLinks::encode(TrackerLinks::get(Id::MyAnimeList, aKey)).toUtf8();
        CHECK(!mblob.contains(kMalSecret));
        CHECK(!mblob.contains("MAL-ACCESS-TOKEN-FIXTURE"));
        CHECK(!TrackerQueue::lastError(Id::MyAnimeList).contains(QString::fromLatin1(kMalSecret)));
        // The REQUESTS carry no credential at all: MAL authenticates with a header, by construction, and
        // the URLs are built from ids and field lists only.
        CHECK(!mal::searchUrl(QStringLiteral("https://api.myanimelist.net/v2"),
                              QStringLiteral("Berserk"), Kind::Manga, 8).contains(QLatin1String(kMalSecret)));
        CHECK(!mal::entryUrl(QStringLiteral("https://api.myanimelist.net/v2"),
                             QStringLiteral("2"), Kind::Manga).contains(QLatin1String(kMalSecret)));
        CHECK(!mal::saveUrl(QStringLiteral("https://api.myanimelist.net/v2"),
                            QStringLiteral("2"), Kind::Manga).contains(QLatin1String(kMalSecret)));
        CHECK(!mal::saveBody(mu, 364).contains(kMalSecret));
        CHECK(!mal::saveBody(mu, 364).contains("MAL-ACCESS-TOKEN-FIXTURE"));

        TrackerQueue::forgetAccount(Id::AniList);
        TrackerQueue::forgetAccount(Id::MyAnimeList);
    }

    // ===== §19  ONE RETRY POLICY, BOTH TRACKERS (issue #326) ==========================================
    // The bug: increment 2 taught MyAnimeList that 400/404/422 is a refusal that can never become a success
    // and dropped the row; AniList retried EVERYTHING, so one such row at the head of an ordered queue
    // blocked every update behind it for ever, with no signal beyond a sync that quietly never landed.
    //
    // Four things are pinned here, in this order: the classification table for both providers, AniList's
    // "a refused mutation is an HTTP 200" reading, the drain loop actually UNWEDGING, and — the regression
    // that would matter — a run with no permanently-refused response behaving exactly as it did before.
    {
        // ---- the table. One function, two policies, and only the codes differ. -------------------------
        const SendPolicy al = anilist::sendPolicy();
        const SendPolicy ml = mal::sendPolicy();

        // The base is the flat 60 seconds increment 1 already waited, to the millisecond, on both.
        CHECK(anilist::kBackoffBaseMs == 60000);
        CHECK(anilist::kBackoffMaxMs == 1800000);
        CHECK(al.baseMs == 60000);
        CHECK(ml.baseMs == 60000);
        CHECK(al.maxMs == 1800000);
        CHECK(ml.maxMs == 1800000);

        // PERMANENT on both: a request the account can never make succeed.
        for (int code : { 400, 404 })
        {
            CHECK(classifySend(al, code, 0, 1).permanent);
            CHECK(!classifySend(al, code, 0, 1).retry);
            CHECK(classifySend(ml, code, 0, 1).permanent);
            CHECK(!classifySend(ml, code, 0, 1).retry);
        }
        // THE ONE GENUINE DIFFERENCE. 422 is a REST validation status: MAL really answers a rejected field
        // with it, and AniList's GraphQL endpoint cannot. Naming it on AniList would be a rule about a
        // response that cannot arrive, and the safe reading of a status we have no rule for is RETRY.
        CHECK(classifySend(ml, 422, 0, 1).permanent);
        CHECK(!classifySend(al, 422, 0, 1).permanent);
        CHECK(classifySend(al, 422, 0, 1).retry);

        // 401 is the TOKEN on both: refresh, keep the row, and do NOT let a Retry-After lengthen it.
        for (const SendPolicy& p : { al, ml })
        {
            const SendVerdict v = classifySend(p, 401, 300, 1);
            CHECK(v.retry);
            CHECK(v.reauth);
            CHECK(!v.permanent);
            CHECK(v.delayMs == 60000);
        }
        // 429 on both: a minute at least, Retry-After honoured UPWARD only.
        for (const SendPolicy& p : { al, ml })
        {
            CHECK(classifySend(p, 429, 0, 1).delayMs == 60000);
            CHECK(classifySend(p, 429, 300, 1).delayMs == 300000);
            CHECK(classifySend(p, 429, 1, 1).delayMs == 60000);
            CHECK(classifySend(p, 429, -5, 1).delayMs == 60000);
            // Doubling, capped, and an exponent that cannot overflow into a negative delay.
            CHECK(classifySend(p, 429, 0, 2).delayMs == 120000);
            CHECK(classifySend(p, 429, 0, 3).delayMs == 240000);
            CHECK(classifySend(p, 429, 0, 30).delayMs == 1800000);
            CHECK(classifySend(p, 429, 0, 1000).delayMs == 1800000);
            CHECK(classifySend(p, 429, 0, 0).delayMs >= 60000);
            // 5xx, a dead socket, and a 4xx nobody has a rule for: all the same waiting problem, and none
            // of them costs anybody their queue.
            for (int code : { 500, 503, 0, 403, 418 })
            {
                CHECK(classifySend(p, code, 0, 1).retry);
                CHECK(!classifySend(p, code, 0, 1).permanent);
            }
            // A success decides nothing.
            CHECK(!classifySend(p, 200, 0, 1).retry);
            CHECK(!classifySend(p, 200, 0, 1).permanent);
            CHECK(classifySend(p, 204, 0, 1).delayMs == 0);
        }

        // MOVED, NOT REWRITTEN: mal::backoffFor is now a forwarder, and §14's numbers are these numbers.
        for (int code : { 200, 204, 0, 400, 401, 403, 404, 418, 422, 429, 500, 503 })
        {
            for (int n : { 1, 2, 7, 1000 })
            {
                const mal::Backoff b = mal::backoffFor(code, 45, n);
                const SendVerdict v = classifySend(ml, code, 45, n);
                CHECK(b.retry == v.retry);
                CHECK(b.reauth == v.reauth);
                CHECK(b.permanent == v.permanent);
                CHECK(b.delayMs == v.delayMs);
            }
        }

        // ---- AniList answers a REFUSED MUTATION with HTTP 200 ------------------------------------------
        const QByteArray okBody = QByteArray(
            "{\"data\":{\"SaveMediaListEntry\":{\"id\":1,\"progress\":3}}}");
        const QByteArray goneBody = QByteArray(
            "{\"errors\":[{\"message\":\"Not Found.\",\"status\":404}],\"data\":null}");
        const QByteArray voiceless = QByteArray("{\"errors\":[{\"message\":\"Internal.\"}]}");

        CHECK(anilist::saveAccepted(200, okBody));
        CHECK(!anilist::saveAccepted(200, goneBody));
        CHECK(!anilist::saveAccepted(200, voiceless));
        CHECK(!anilist::saveAccepted(404, okBody));   // the status has to agree too
        CHECK(!anilist::saveAccepted(0, QByteArray()));
        // The status the POLICY judges: the transport's when there was one, else AniList's own.
        CHECK(anilist::effectiveStatus(404, goneBody) == 404);
        CHECK(anilist::effectiveStatus(429, QByteArray()) == 429);
        CHECK(anilist::effectiveStatus(0, QByteArray()) == 0);
        CHECK(anilist::effectiveStatus(200, goneBody) == 404);
        // AN ERROR SHAPE WE CANNOT READ IS RETRYABLE, never a drop. This is the safety net under the whole
        // change: 0 falls through classifySend's default, which is exactly what increment 1 did with every
        // failure it could not name.
        CHECK(anilist::effectiveStatus(200, voiceless) == 0);
        CHECK(anilist::effectiveStatus(200, QByteArray("not json at all")) == 0);
        CHECK(!classifySend(al, anilist::effectiveStatus(200, voiceless), 0, 1).permanent);
        CHECK(classifySend(al, anilist::effectiveStatus(200, goneBody), 0, 1).permanent);

        // ---- the loopback callback, shared by both trackers --------------------------------------------
        const LoopbackCallback good = parseLoopbackRequest(
            "GET /?code=AUTHCODE-1&state=abc123 HTTP/1.1\r\nHost: 127.0.0.1:1234\r\n\r\n");
        CHECK(good.code == QLatin1String("AUTHCODE-1"));
        CHECK(good.state == QLatin1String("abc123"));
        CHECK(good.error.isEmpty());
        const LoopbackCallback denied = parseLoopbackRequest(
            "GET /?error=access_denied HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(denied.error == QLatin1String("access_denied"));
        CHECK(denied.code.isEmpty());
        // TOTAL: bytes that are not a request line cost nothing.
        CHECK(parseLoopbackRequest(QByteArray()).code.isEmpty());
        CHECK(parseLoopbackRequest("garbage").code.isEmpty());
        CHECK(parseLoopbackRequest("GET").code.isEmpty());
        // The reply is a PLAIN page with no redirect and no referrer, and its Content-Length agrees with
        // the body it carries — the browser must not sit waiting on a connection close.
        for (bool signedIn : { true, false })
        {
            const QByteArray rep = loopbackResponse(signedIn);
            CHECK(rep.startsWith("HTTP/1.1 200 OK\r\n"));
            CHECK(rep.contains("Referrer-Policy: no-referrer"));
            CHECK(rep.contains("Cache-Control: no-store"));
            CHECK(!rep.contains("Location:"));
            const int sep = rep.indexOf("\r\n\r\n");
            CHECK(sep > 0);
            CHECK(rep.contains("Content-Length: " + QByteArray::number(rep.size() - sep - 4)));
        }

        // ---- THE UNWEDGE ------------------------------------------------------------------------------
        // A permanently-refused row at the HEAD of the queue no longer blocks the rows behind it. This is
        // the issue, stated as a property: the ones behind it must actually land.
        const qint64 T = 1'800'000'000'000LL;
        TrackerLinks::set(Id::AniList, QStringLiteral("326:a"), QStringLiteral("ma"), Kind::Anime,
                          QStringLiteral("A Deleted Series"), 12);
        seedQueue(Id::AniList, { { QStringLiteral("326:a"), 3 },
                                 { QStringLiteral("326:b"), 5 },
                                 { QStringLiteral("326:c"), 7 } }, T - 1000);
        {
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::AniList, al,
                        { { 404, 0, false }, { 200, 0, true }, { 200, 0, true } }, at, sent, pushed, T,
                        waits, QStringLiteral("AniList refused the update for %1 and it has been dropped; "
                                              "the rest are still queued."));
            // All three were attempted, in order, in ONE drain: the drop does not stop the loop.
            CHECK(sent.size() == 3);
            CHECK(sent.value(0) == QLatin1String("326:a@3"));
            CHECK(sent.value(1) == QLatin1String("326:b@5"));
            CHECK(sent.value(2) == QLatin1String("326:c@7"));
            // THE ROWS BEHIND IT LANDED. Not "were retried" — landed.
            CHECK(pushed.size() == 2);
            CHECK(pushed.value(0) == QLatin1String("326:b@5"));
            CHECK(pushed.value(1) == QLatin1String("326:c@7"));
            CHECK(TrackerQueue::count(Id::AniList) == 0);
            // ...and the refused one is not still sitting there, and did not get a sent stamp.
            CHECK(TrackerQueue::lastSentMs(Id::AniList, QStringLiteral("326:a")) == 0);
            CHECK(TrackerQueue::lastSentMs(Id::AniList, QStringLiteral("326:b")) == T);
        }

        // THE BUG ITSELF, kept as a property so it cannot come back by another route: with no permanent
        // codes at all — which is literally what AniList's policy was — the same run wedges. One attempt,
        // nothing delivered, three rows still queued.
        seedQueue(Id::AniList, { { QStringLiteral("326:a"), 3 },
                                 { QStringLiteral("326:b"), 5 },
                                 { QStringLiteral("326:c"), 7 } }, T - 1000);
        {
            SendPolicy retryEverything = al;
            retryEverything.permanent.clear();
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::AniList, retryEverything,
                        { { 404, 0, false }, { 200, 0, true }, { 200, 0, true } }, at, sent, pushed, T,
                        waits);
            CHECK(sent.size() == 1);                      // it never got past the head
            CHECK(pushed.isEmpty());                      // ...so nothing behind it landed
            CHECK(TrackerQueue::count(Id::AniList) == 3); // ...and the whole queue is still there
            CHECK(waits.value(0) == 60000);               // waiting for a 404 that will never change
        }

        // ---- the drop is VISIBLE, and it says which one ------------------------------------------------
        seedQueue(Id::AniList, { { QStringLiteral("326:a"), 3 } }, T - 1000);
        {
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::AniList, al, { { 400, 0, false } }, at, sent, pushed, T, waits,
                        QStringLiteral("AniList refused the update for %1 and it has been dropped; "
                                       "the rest are still queued."));
            const QString line = TrackerQueue::lastError(Id::AniList);
            CHECK(!line.isEmpty());
            CHECK(line.contains(QLatin1String("A Deleted Series")));   // WHICH item
            CHECK(line.contains(QLatin1String("dropped")));            // ...and what happened to it
            // Never a credential, never a request, never a response body — the rule both tracker headers
            // carry, held against the one message this change adds.
            CHECK(!line.contains(QString::fromLatin1(kFixtureSecret)));
            CHECK(!line.contains(QLatin1String("Bearer")));
            CHECK(!line.contains(QLatin1String("http")));
        }

        // ---- every other arm, driven through the loop, on BOTH trackers --------------------------------
        // 422 is the arm that separates them: MAL drops it, AniList keeps it.
        struct Arm { Id id; SendPolicy p; int status; qint64 retryAfter; bool drops; qint64 wait; };
        const QVector<Arm> arms = {
            { Id::AniList,     al, 400, 0,   true,  0 },
            { Id::AniList,     al, 404, 0,   true,  0 },
            { Id::AniList,     al, 422, 0,   false, 60000 },
            { Id::AniList,     al, 429, 0,   false, 60000 },
            { Id::AniList,     al, 429, 900, false, 900000 },
            { Id::AniList,     al, 500, 0,   false, 60000 },
            { Id::AniList,     al, 401, 0,   false, 60000 },
            { Id::AniList,     al, 0,   0,   false, 60000 },
            { Id::MyAnimeList, ml, 400, 0,   true,  0 },
            { Id::MyAnimeList, ml, 404, 0,   true,  0 },
            { Id::MyAnimeList, ml, 422, 0,   true,  0 },
            { Id::MyAnimeList, ml, 429, 0,   false, 60000 },
            { Id::MyAnimeList, ml, 429, 900, false, 900000 },
            { Id::MyAnimeList, ml, 503, 0,   false, 60000 },
            { Id::MyAnimeList, ml, 401, 0,   false, 60000 },
            { Id::MyAnimeList, ml, 0,   0,   false, 60000 },
        };
        for (const Arm& a : arms)
        {
            seedQueue(a.id, { { QStringLiteral("326:one"), 4 } }, T - 1000);
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(a.id, a.p, { { a.status, a.retryAfter, false } }, at, sent, pushed, T, waits);
            CHECK(sent.size() == 1);
            CHECK(pushed.isEmpty());
            if (a.drops)
            {
                // Gone from the queue, and the loop carried on to find it empty (so the timer was stopped).
                CHECK(TrackerQueue::count(a.id) == 0);
                CHECK(waits.value(0) == 0);
            }
            else
            {
                // Still queued, and the wait is the documented one.
                CHECK(TrackerQueue::count(a.id) == 1);
                CHECK(waits.size() == 1);
                CHECK(waits.value(0) == a.wait);
            }
            // Either way the user is told something.
            CHECK(!TrackerQueue::lastError(a.id).isEmpty());
        }
        // ...and a SUCCESS clears the line and empties the queue, on both.
        for (Id id : { Id::AniList, Id::MyAnimeList })
        {
            TrackerQueue::setLastError(id, QStringLiteral("something old"));
            seedQueue(id, { { QStringLiteral("326:two"), 9 } }, T - 1000);
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(id, id == Id::AniList ? al : ml, { { 200, 0, true } }, at, sent, pushed,
                        T + kDebounceMs * 4, waits);
            CHECK(pushed.value(0) == QLatin1String("326:two@9"));
            CHECK(TrackerQueue::count(id) == 0);
            CHECK(TrackerQueue::lastError(id).isEmpty());
        }

        // ---- THE IDENTITY ASSERTION -------------------------------------------------------------------
        // A run with NO permanently-refused response must behave exactly as increment 1's loop did. The
        // comparison is against legacyDrain above — a transcription of the code this issue deleted — not
        // against a description of it: same rows attempted in the same order, same queue left behind, same
        // status line, same first wait.
        //
        // The two runs use the SAME item keys and are separated in time by four debounce windows, so the
        // sent-stamps the first run wrote cannot suppress a row in the second.
        const QVector<QPair<QString, int>> rows = { { QStringLiteral("326:i1"), 2 },
                                                    { QStringLiteral("326:i2"), 4 },
                                                    { QStringLiteral("326:i3"), 6 } };
        // A script with a FAILURE in it, because the retry path is the half that could have drifted. 500 is
        // retryable under both the old rule (everything was) and the new one.
        const QVector<ScriptedReply> mixed = { { 200, 0, true }, { 500, 0, false }, { 200, 0, true } };

        seedQueue(Id::AniList, rows, T - 1000);
        QStringList legacySent;
        QVector<qint64> legacyWaits;
        int legacyAt = 0;
        legacyDrain(mixed, legacyAt, legacySent, T, legacyWaits);
        const QByteArray legacyQueue = encodeQueue(TrackerQueue::load(Id::AniList));
        const QString legacyLine = TrackerQueue::lastError(Id::AniList);

        seedQueue(Id::AniList, rows, T - 1000);
        QStringList nowSent, nowPushed;
        QVector<qint64> nowWaits;
        int nowAt = 0;
        senderDrain(Id::AniList, al, mixed, nowAt, nowSent, nowPushed, T + kDebounceMs * 4, nowWaits);
        const QByteArray nowQueue = encodeQueue(TrackerQueue::load(Id::AniList));

        CHECK(nowSent == legacySent);                       // the same rows, in the same order
        CHECK(nowSent.size() == 2);                         // ...and the run really did do something
        CHECK(nowQueue == legacyQueue);                     // the same queue left on disk, byte for byte
        CHECK(!nowQueue.isEmpty());
        CHECK(TrackerQueue::lastError(Id::AniList) == legacyLine);   // the same sentence
        CHECK(legacyLine == QString::fromLatin1(kLegacyRetryLine));
        CHECK(nowWaits == legacyWaits);                     // ...and the same first wait: 60 seconds
        CHECK(nowWaits.value(0) == 60000);

        // The same, with no failure at all: both drain to empty and both stop the timer.
        const QVector<ScriptedReply> allGood = { { 200, 0, true }, { 200, 0, true }, { 200, 0, true } };
        seedQueue(Id::AniList, rows, T - 1000);
        QStringList lSent; QVector<qint64> lWaits; int lAt = 0;
        legacyDrain(allGood, lAt, lSent, T + kDebounceMs * 8, lWaits);
        const QByteArray lQueue = encodeQueue(TrackerQueue::load(Id::AniList));
        seedQueue(Id::AniList, rows, T - 1000);
        QStringList nSent, nPushed; QVector<qint64> nWaits; int nAt = 0;
        senderDrain(Id::AniList, al, allGood, nAt, nSent, nPushed, T + kDebounceMs * 12, nWaits);
        CHECK(nSent == lSent);
        CHECK(nSent.size() == 3);
        CHECK(encodeQueue(TrackerQueue::load(Id::AniList)) == lQueue);
        CHECK(nWaits == lWaits);
        CHECK(nWaits.value(0) == 0);   // nothing pending: the timer is STOPPED, not re-armed
        CHECK(TrackerQueue::lastError(Id::AniList).isEmpty());

        TrackerQueue::forgetAccount(Id::AniList);
        TrackerQueue::forgetAccount(Id::MyAnimeList);
    }


    // ===== §20  KITSU, THE THIRD PROVIDER ON THE SHARED LAYER (issue #156, increment 3) ================
    // The increment's real question is not "does Kitsu work" but "did #326's hoist hold for a provider it
    // was not written against". So this section is deliberately split: everything Kitsu SUPPLIES is
    // asserted here, and everything it CONSUMES is asserted by driving the SHARED functions
    // (tracker::classifySend, TrackerQueue::Sender, TrackerQueue's queue and credential store) with
    // Id::Kitsu — never a Kitsu copy of them, because there is not one.
    //
    // NO KITSU ACCOUNT WAS CREATED, no API client was registered, and nothing here or in the live drive
    // that accompanies it contacted Kitsu. Every fixture is written from Kitsu's published JSON:API
    // reference and was answered by a local fixture server.
    {
        // ---- auth: the PASSWORD GRANT, which is the one place Kitsu is unlike the other two ----------
        // There is no client to register, so there is no authorize URL, no loopback listener, no PKCE
        // verifier and no `state`: the whole sign-in is one POST carrying the account's own credentials.
        const QByteArray grant = kitsu::passwordGrantBody(QString::fromLatin1(kKitsuEmail),
                                                          QString::fromLatin1(kKitsuPassword));
        CHECK(grant.contains("grant_type=password"));
        CHECK(grant.contains("username=fixture-kitsu%40example.invalid"));
        // THE THINGS THAT ARE NOT THERE, each of which would be a bug rather than an omission: Kitsu's
        // password grant takes no client credentials and no redirect, and sending an empty client_id is a
        // different request to an OAuth server than sending none.
        CHECK(!grant.contains("client_id"));
        CHECK(!grant.contains("client_secret"));
        CHECK(!grant.contains("redirect_uri"));
        CHECK(!grant.contains("code_verifier"));
        // PERCENT-ENCODED BY HAND, and this is the case that proves why: QUrlQuery leaves '+' alone, and a
        // '+' in a password decodes on the far side as a SPACE — which presents to the user as "wrong
        // password" on a password that is perfectly right.
        const QByteArray awkward = kitsu::passwordGrantBody(QStringLiteral("a b@c.d"),
                                                            QStringLiteral("p+q&r s"));
        CHECK(awkward.contains("password=p%2Bq%26r%20s"));
        CHECK(!awkward.contains("password=p+q"));
        // The username IS trimmed (a pasted email drags whitespace); the password is NOT, because
        // whitespace is legal in one and trimming it would sign in as something the user did not type.
        CHECK(kitsu::passwordGrantBody(QStringLiteral("  who@x.y  "), QStringLiteral(" pw "))
                  .contains("username=who%40x.y"));
        CHECK(kitsu::passwordGrantBody(QStringLiteral("who@x.y"), QStringLiteral(" pw "))
                  .contains("password=%20pw%20"));

        const QByteArray refresh = kitsu::tokenRefreshBody(QStringLiteral("RT-KITSU"));
        CHECK(refresh.contains("grant_type=refresh_token"));
        CHECK(refresh.contains("refresh_token=RT-KITSU"));
        // A refresh carries NO password. If it did, the credential would have had to be stored to be
        // available at refresh time, which is the whole thing this design avoids.
        CHECK(!refresh.contains("password"));
        CHECK(!refresh.contains("username"));

        // A real reply.
        kitsu::TokenReply kt = kitsu::parseTokenReply(
            R"({"access_token":"KA","refresh_token":"KR","expires_in":2592000,)"
            R"("created_at":1700000000,"scope":"all","token_type":"bearer"})");
        CHECK(kt.ok);
        CHECK(kt.accessToken == QLatin1String("KA"));
        CHECK(kt.refreshToken == QLatin1String("KR"));
        CHECK(kt.expiresInSec == 2592000);
        // THE ONE THAT PERMANENTLY UNLINKS AN ACCOUNT, held for the third provider: each of these is a
        // body that is not a token reply, and each must come back ok=false so the caller stores nothing.
        CHECK(!kitsu::parseTokenReply(
                  R"({"error":"invalid_grant","error_description":"bad password"})").ok);
        CHECK(!kitsu::parseTokenReply(R"({"access_token":""})").ok);
        CHECK(!kitsu::parseTokenReply("<html>captive portal</html>").ok);
        CHECK(!kitsu::parseTokenReply("[]").ok);
        CHECK(!kitsu::parseTokenReply(QByteArray()).ok);
        CHECK(kitsu::parseTokenReply(R"({"error":"x"})").accessToken.isEmpty());
        // A refresh that omits the refresh token is legal; the caller keeps the old one.
        kt = kitsu::parseTokenReply(R"({"access_token":"KB","expires_in":"3600"})");
        CHECK(kt.ok);
        CHECK(kt.refreshToken.isEmpty());
        CHECK(kt.expiresInSec == 3600);

        // ---- who the token belongs to ----------------------------------------------------------------
        CHECK(kitsu::selfUrl(QStringLiteral("https://k/api/edge"))
              == QLatin1String("https://k/api/edge/users?filter%5Bself%5D=true"));
        CHECK(kitsu::parseSelfId(kSelfReply) == QLatin1String("42"));
        // TOTAL: a body that is not one, an empty collection, and a row with no id all read back "".
        CHECK(kitsu::parseSelfId(R"({"data":[]})").isEmpty());
        CHECK(kitsu::parseSelfId(R"({"errors":[{"status":"401"}]})").isEmpty());
        CHECK(kitsu::parseSelfId("not json").isEmpty());
        CHECK(kitsu::parseSelfId(QByteArray()).isEmpty());

        // ---- search ----------------------------------------------------------------------------------
        const QString api = QStringLiteral("https://kitsu.app/api/edge");
        const QString su = kitsu::searchUrl(api, QStringLiteral("  My Hero  "), 0, Kind::Anime, 8);
        CHECK(su.startsWith(QLatin1String("https://kitsu.app/api/edge/anime?")));
        CHECK(su.contains(QLatin1String("filter%5Btext%5D=My%20Hero")));   // trimmed, and encoded
        CHECK(su.contains(QLatin1String("page%5Blimit%5D=8")));
        CHECK(su.contains(QLatin1String("episodeCount")));                 // the COMPLETED rule's input
        // OMITTED, not sent as 0. filter[year]=0 matches nothing, so a caller with no year would get an
        // empty list rather than an unfiltered one.
        CHECK(!su.contains(QLatin1String("filter%5Byear%5D")));
        CHECK(kitsu::searchUrl(api, QStringLiteral("Berserk"), 1989, Kind::Manga, 8)
                  .contains(QLatin1String("filter%5Byear%5D=1989")));
        // The kind reaches the wire in BOTH the path and the field list.
        CHECK(kitsu::searchUrl(api, QStringLiteral("Berserk"), 0, Kind::Manga, 8)
                  .startsWith(QLatin1String("https://kitsu.app/api/edge/manga?")));
        CHECK(kitsu::searchUrl(api, QStringLiteral("Berserk"), 0, Kind::Manga, 8)
                  .contains(QLatin1String("chapterCount")));
        // Too short to ask about: "we did not ask" and "Kitsu said nothing" are the same empty result.
        CHECK(kitsu::searchable(QStringLiteral("abc")));
        CHECK(!kitsu::searchable(QStringLiteral("ab")));
        CHECK(!kitsu::searchable(QStringLiteral("   a   ")));
        CHECK(kitsu::searchUrl(api, QStringLiteral("ab"), 0, Kind::Anime, 8).isEmpty());

        QVector<Match> km = kitsu::parseSearch(kKitsuSearchReply, Kind::Anime);
        // Three rows in, TWO out: the third has no id and is skipped without costing the other two.
        CHECK(km.size() == 2);
        if (km.size() == 2)
        {
            CHECK(km[0].mediaId == QLatin1String("7442"));
            CHECK(km[0].title == QLatin1String("My Hero Academia"));       // titles.en preferred
            CHECK(km[0].altTitle == QLatin1String("Boku no Hero Academia"));
            CHECK(km[0].year == 2016);
            CHECK(km[0].kind == Kind::Anime);
            CHECK(km[0].totalUnits == 13);
            CHECK(km[0].coverUrl == QLatin1String("https://media.kitsu.test/1-original.jpg"));
            // One title only: the canonical one is used and there is no second line to show.
            CHECK(km[1].mediaId == QLatin1String("1712"));
            CHECK(km[1].title == QLatin1String("Berserk"));
            CHECK(km[1].altTitle.isEmpty());
            CHECK(km[1].kind == Kind::Manga);      // the COUNT decides the kind, not the endpoint
            CHECK(km[1].totalUnits == 364);
            CHECK(km[1].coverUrl == QLatin1String("https://media.kitsu.test/2-medium.jpg"));
        }
        // A row Kitsu has no count for is filed under the kind the caller ASKED for.
        km = kitsu::parseSearch(kKitsuUnreleasedReply, Kind::Manga);
        CHECK(km.size() == 1);
        if (km.size() == 1)
        {
            CHECK(km[0].kind == Kind::Manga);
            CHECK(km[0].totalUnits == 0);
            CHECK(km[0].year == 0);
        }
        // EMPTY, MALFORMED, and an ERROR DOCUMENT: all three are an empty list, never a partial one.
        CHECK(kitsu::parseSearch(R"({"data":[]})", Kind::Anime).isEmpty());
        CHECK(kitsu::parseSearch(kKitsuErrorReply, Kind::Anime).isEmpty());
        CHECK(kitsu::parseSearch("<html>", Kind::Anime).isEmpty());
        CHECK(kitsu::parseSearch(QByteArray(), Kind::Anime).isEmpty());
        CHECK(kitsu::parseSearch("[]", Kind::Anime).isEmpty());

        // PAGINATION, and the same-origin rule that keeps the account's bearer token off a host a response
        // body chose. `links.next` is attacker-controlled input by definition.
        CHECK(kitsu::nextPageUrl(kKitsuSearchReply, api)
                  .startsWith(QLatin1String("https://kitsu.app/api/edge/anime?page%5Boffset%5D=8")));
        CHECK(kitsu::nextPageUrl(kKitsuSearchReply, QStringLiteral("https://kitsu.app.evil.test/api/edge"))
                  .isEmpty());
        CHECK(kitsu::nextPageUrl(R"({"links":{"next":"http://kitsu.app/api/edge/anime"}})", api).isEmpty());
        CHECK(kitsu::nextPageUrl(R"({"links":{"next":"https://kitsu.app:8443/api/edge/anime"}})", api)
                  .isEmpty());
        CHECK(kitsu::nextPageUrl(R"({"data":[]})", api).isEmpty());
        CHECK(kitsu::nextPageUrl("garbage", api).isEmpty());

        // ---- the account's entry ---------------------------------------------------------------------
        const QString eu = kitsu::entryUrl(api, QStringLiteral("42"), QStringLiteral("1712"), Kind::Manga);
        CHECK(eu.startsWith(QLatin1String("https://kitsu.app/api/edge/library-entries?")));
        CHECK(eu.contains(QLatin1String("filter%5Buser_id%5D=42")));
        CHECK(eu.contains(QLatin1String("filter%5Bmedia_id%5D=1712")));
        CHECK(eu.contains(QLatin1String("filter%5Bkind%5D=manga")));
        CHECK(eu.contains(QLatin1String("include=manga")));   // what brings the unit count back
        // EMPTY IN, EMPTY OUT. A blank user filter would answer with somebody else's library and a blank
        // media filter with the whole of ours, so neither request is ever built.
        CHECK(kitsu::entryUrl(api, QString(), QStringLiteral("1712"), Kind::Manga).isEmpty());
        CHECK(kitsu::entryUrl(api, QStringLiteral("42"), QString(), Kind::Manga).isEmpty());

        Entry ke;
        QString keId = QStringLiteral("stale");
        CHECK(kitsu::parseEntry(kKitsuEntryReply, QStringLiteral("1712"), Kind::Manga, ke, &keId));
        CHECK(ke.exists);
        CHECK(ke.mediaId == QLatin1String("1712"));
        CHECK(ke.progress == 12);
        CHECK(ke.status == Status::Current);
        CHECK(ke.score == 85);                 // ratingTwenty 17 -> 85 at the seam
        CHECK(ke.totalUnits == 364);           // off the INCLUDED media, not off the entry
        CHECK(keId == QLatin1String("551"));   // the library entry's OWN id — what a PATCH is addressed to

        // ASKED, ANSWERED, AND THE ACCOUNT HAS NO ROW. An empty `data` array is a SUCCESS, and it is what
        // selects the create path — distinct from "progress 0" and distinct from a failed request.
        keId = QStringLiteral("stale");
        CHECK(kitsu::parseEntry(R"({"data":[],"included":[]})", QStringLiteral("1712"), Kind::Manga,
                                ke, &keId));
        CHECK(!ke.exists);
        CHECK(ke.progress == 0);
        CHECK(keId.isEmpty());
        // ...and "this body was not an entry reply at all", which is a failed request and leaves the row
        // queued. Each of these must be false, not an empty entry.
        CHECK(!kitsu::parseEntry(kKitsuErrorReply, QStringLiteral("1712"), Kind::Manga, ke, &keId));
        CHECK(!kitsu::parseEntry(R"({"data":{"id":"551"}})", QStringLiteral("1712"), Kind::Manga,
                                 ke, &keId));   // a single resource, not a collection
        CHECK(!kitsu::parseEntry("not json", QStringLiteral("1712"), Kind::Manga, ke, &keId));
        CHECK(!kitsu::parseEntry(QByteArray(), QStringLiteral("1712"), Kind::Manga, ke, &keId));
        // A NULL out-parameter is legal — fetchEntry does not want the entry id.
        CHECK(kitsu::parseEntry(kKitsuEntryReply, QStringLiteral("1712"), Kind::Manga, ke, nullptr));

        // ---- the push, and its IDEMPOTENCE -----------------------------------------------------------
        Update ku;
        ku.itemKey = QStringLiteral("marks:series:berserk");
        ku.mediaId = QStringLiteral("1712");
        ku.kind = Kind::Manga;
        ku.unit = 13;
        ku.atMs = 1'700'000'000'000LL;

        // The verb and the URL come off ONE emptiness test, so they cannot disagree about which of the two
        // this is.
        CHECK(kitsu::saveMethod(QString()) == QByteArray("POST"));
        CHECK(kitsu::saveMethod(QStringLiteral("551")) == QByteArray("PATCH"));
        CHECK(kitsu::saveUrl(api, QString()) == api + QLatin1String("/library-entries"));
        CHECK(kitsu::saveUrl(api, QStringLiteral("551")) == api + QLatin1String("/library-entries/551"));

        // THE CREATE. Reached only when the read said the account really has no row.
        const QByteArray create = kitsu::saveBody(ku, 364, QString(), QStringLiteral("42"));
        const QJsonObject cdata = QJsonDocument::fromJson(create).object()
                                      .value(QStringLiteral("data")).toObject();
        CHECK(cdata.value(QStringLiteral("type")).toString() == QLatin1String("libraryEntries"));
        CHECK(!cdata.contains(QStringLiteral("id")));   // there is nothing to address yet
        const QJsonObject crels = cdata.value(QStringLiteral("relationships")).toObject();
        CHECK(crels.value(QStringLiteral("user")).toObject().value(QStringLiteral("data")).toObject()
                   .value(QStringLiteral("id")).toString() == QLatin1String("42"));
        CHECK(crels.value(QStringLiteral("manga")).toObject().value(QStringLiteral("data")).toObject()
                   .value(QStringLiteral("id")).toString() == QLatin1String("1712"));
        CHECK(crels.value(QStringLiteral("manga")).toObject().value(QStringLiteral("data")).toObject()
                   .value(QStringLiteral("type")).toString() == QLatin1String("manga"));
        CHECK(!crels.contains(QStringLiteral("anime")));   // the KIND reaches the relationship name

        // THE UPDATE. It carries the entry's id and NO relationships: re-stating them on a PATCH is how an
        // entry gets re-pointed at another user's library or at another series.
        const QByteArray patch = kitsu::saveBody(ku, 364, QStringLiteral("551"), QStringLiteral("42"));
        const QJsonObject pdata = QJsonDocument::fromJson(patch).object()
                                      .value(QStringLiteral("data")).toObject();
        CHECK(pdata.value(QStringLiteral("id")).toString() == QLatin1String("551"));
        CHECK(!pdata.contains(QStringLiteral("relationships")));
        CHECK(!patch.contains("\"42\""));   // the user id is not on the wire on an update at all

        // IDEMPOTENT BY CONSTRUCTION, twice over:
        //   * the same update produces byte-identical bytes, so a replay after a restart is the same
        //     request and not a second, different one;
        CHECK(kitsu::saveBody(ku, 364, QStringLiteral("551"), QStringLiteral("42")) == patch);
        //   * and once the row exists the write is a PATCH addressed to it, so a replayed queue row can
        //     never create a SECOND library entry for the same series. That is the property; the pair
        //     below is what makes it hold.
        {
            Entry existing;
            QString existingId;
            kitsu::parseEntry(kKitsuEntryReply, QStringLiteral("1712"), Kind::Manga, existing, &existingId);
            CHECK(existing.exists);
            CHECK(kitsu::saveMethod(existingId) == QByteArray("PATCH"));
            CHECK(!kitsu::saveBody(ku, 364, existingId, QStringLiteral("42"))
                       .contains("relationships"));
        }

        // THE THREE SAFETY RULES, restated against Kitsu's spellings.
        //   1. No rating unless the app really has one. Kitsu reads a present ratingTwenty as a rating the
        //      user gave, so sending one they did not give overwrites the one they did.
        CHECK(!patch.contains("ratingTwenty"));
        ku.hasScore = true;
        ku.score = 85;
        CHECK(kitsu::saveBody(ku, 364, QStringLiteral("551"), QStringLiteral("42"))
                  .contains("\"ratingTwenty\":17"));
        ku.hasScore = false;
        //   2. COMPLETED needs the app's claim AND Kitsu's own count to agree.
        ku.completes = true;
        CHECK(kitsu::saveBody(ku, 364, QStringLiteral("551"), QString()).contains("\"status\":\"current\""));
        CHECK(kitsu::saveBody(ku, 13, QStringLiteral("551"), QString()).contains("\"status\":\"completed\""));
        CHECK(kitsu::saveBody(ku, 0, QStringLiteral("551"), QString()).contains("\"status\":\"completed\""));
        ku.completes = false;
        CHECK(kitsu::saveBody(ku, 13, QStringLiteral("551"), QString()).contains("\"status\":\"current\""));
        //   3. Progress is never below 1: a 0 tells the account you have read nothing.
        ku.unit = 0;
        CHECK(kitsu::saveBody(ku, 364, QStringLiteral("551"), QString()).contains("\"progress\":1"));
        ku.unit = -4;
        CHECK(kitsu::saveBody(ku, 364, QStringLiteral("551"), QString()).contains("\"progress\":1"));
        ku.unit = 13;

        // ---- statuses and the score conversion -------------------------------------------------------
        // NOT kind-dependent, unlike MAL's — and asserted so, because copying MAL's watching/reading split
        // over would be a silently wrong write.
        CHECK(kitsu::statusToken(Status::Current) == QLatin1String("current"));
        CHECK(kitsu::statusToken(Status::Planning) == QLatin1String("planned"));
        CHECK(kitsu::statusToken(Status::Completed) == QLatin1String("completed"));
        CHECK(kitsu::statusToken(Status::Dropped) == QLatin1String("dropped"));
        CHECK(kitsu::statusToken(Status::Paused) == QLatin1String("on_hold"));
        CHECK(kitsu::statusToken(Status::Repeating) == QLatin1String("current"));
        CHECK(kitsu::statusFromToken(QStringLiteral("completed")) == Status::Completed);
        CHECK(kitsu::statusFromToken(QStringLiteral("planned")) == Status::Planning);
        CHECK(kitsu::statusFromToken(QStringLiteral("on_hold")) == Status::Paused);
        CHECK(kitsu::statusFromToken(QStringLiteral("dropped")) == Status::Dropped);
        CHECK(kitsu::statusFromToken(QStringLiteral("current")) == Status::Current);
        CHECK(kitsu::statusFromToken(QStringLiteral("something new")) == Status::Current);
        CHECK(kitsu::statusFromToken(QString()) == Status::Current);
        // ROUNDED, and CLAMPED UP to 2 — Kitsu's scale starts at 2 and refuses a 0. 85 is a 17, and a 17
        // read back is 85, so a score survives a push/pull round trip.
        CHECK(kitsu::scoreToKitsu(100) == 20);
        CHECK(kitsu::scoreToKitsu(85) == 17);
        CHECK(kitsu::scoreToKitsu(83) == 17);
        CHECK(kitsu::scoreToKitsu(1) == 2);
        CHECK(kitsu::scoreToKitsu(0) == 2);
        CHECK(kitsu::scoreToKitsu(500) == 20);
        CHECK(kitsu::scoreToKitsu(-5) == 2);
        CHECK(kitsu::scoreFromKitsu(17) == 85);
        CHECK(kitsu::scoreFromKitsu(20) == 100);
        CHECK(kitsu::scoreFromKitsu(0) == 0);     // no rating on the entry stays "unrated"
        CHECK(kitsu::scoreFromKitsu(-3) == 0);
        CHECK(kitsu::scoreFromKitsu(99) == 100);
        CHECK(kitsu::scoreToKitsu(kitsu::scoreFromKitsu(17)) == 17);

        // ---- THE HOIST HELD: Kitsu's failures go through the SHARED classification -------------------
        // Not a Kitsu backoff — tracker::classifySend, asked with kitsu::sendPolicy(). The only thing this
        // provider adds is a set of status codes.
        const SendPolicy kp = kitsu::sendPolicy();
        CHECK(kitsu::kBackoffBaseMs == 60000);
        CHECK(kitsu::kBackoffMaxMs == 1800000);
        CHECK(kp.baseMs == 60000);
        CHECK(kp.maxMs == 1800000);
        // MAL's set, not AniList's, and for a structural reason: Kitsu is JSON:API over REST, so 422 is a
        // status it really can answer with, where AniList's single GraphQL endpoint cannot.
        for (int code : { 400, 404, 422 })
        {
            CHECK(classifySend(kp, code, 0, 1).permanent);
            CHECK(!classifySend(kp, code, 0, 1).retry);
        }
        CHECK(kp.permanent == mal::sendPolicy().permanent);
        CHECK(kp.permanent != anilist::sendPolicy().permanent);
        // 401 is the token: refresh, keep the row, and a Retry-After does not lengthen it.
        {
            const SendVerdict v = classifySend(kp, 401, 300, 1);
            CHECK(v.retry);
            CHECK(v.reauth);
            CHECK(!v.permanent);
            CHECK(v.delayMs == 60000);
        }
        // 429: a minute at least, Retry-After honoured UPWARD only, doubling, capped.
        CHECK(classifySend(kp, 429, 0, 1).delayMs == 60000);
        CHECK(classifySend(kp, 429, 300, 1).delayMs == 300000);
        CHECK(classifySend(kp, 429, 1, 1).delayMs == 60000);
        CHECK(classifySend(kp, 429, 0, 3).delayMs == 240000);
        CHECK(classifySend(kp, 429, 0, 1000).delayMs == 1800000);
        // 5xx, a dead socket, and a 4xx nobody has a rule for: all a waiting problem, none of them costs
        // anybody their queue. 403 is deliberately retried — a suspended account and a temporarily
        // refused client share it.
        for (int code : { 500, 503, 0, 403, 418 })
        {
            CHECK(classifySend(kp, code, 0, 1).retry);
            CHECK(!classifySend(kp, code, 0, 1).permanent);
        }
        CHECK(!classifySend(kp, 200, 0, 1).retry);
        CHECK(!classifySend(kp, 201, 0, 1).retry);   // a create answers 201, and that is not a failure
        CHECK(!classifySend(kp, 201, 0, 1).permanent);

        // ---- ...and through the SHARED DRAIN LOOP, with Id::Kitsu ------------------------------------
        // TrackerQueue::Sender, unmodified, driven with Kitsu's policy: every arm behaves as it does for
        // the other two, and the permanently-refused row is DROPPED rather than left to wedge the head of
        // an ordered queue.
        const qint64 KT = 1'900'000'000'000LL;
        struct KArm { int status; qint64 retryAfter; bool drops; qint64 wait; };
        const QVector<KArm> karms = {
            { 400, 0,   true,  0 },
            { 404, 0,   true,  0 },
            { 422, 0,   true,  0 },
            { 429, 0,   false, 60000 },
            { 429, 900, false, 900000 },
            { 503, 0,   false, 60000 },
            { 401, 0,   false, 60000 },
            { 0,   0,   false, 60000 },
        };
        for (const KArm& a : karms)
        {
            seedQueue(Id::Kitsu, { { QStringLiteral("156c:one"), 4 } }, KT - 1000);
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::Kitsu, kp, { { a.status, a.retryAfter, false } }, at, sent, pushed, KT, waits);
            CHECK(sent.size() == 1);
            CHECK(pushed.isEmpty());
            if (a.drops)
            {
                CHECK(TrackerQueue::count(Id::Kitsu) == 0);
                CHECK(waits.value(0) == 0);
            }
            else
            {
                CHECK(TrackerQueue::count(Id::Kitsu) == 1);
                CHECK(waits.value(0) == a.wait);
            }
            CHECK(!TrackerQueue::lastError(Id::Kitsu).isEmpty());   // the user is told something either way
        }
        // THE UNWEDGE, on Kitsu's queue: a permanently-refused row at the HEAD does not block the rows
        // behind it, and the ones behind it LAND — not "are retried", land.
        TrackerLinks::set(Id::Kitsu, QStringLiteral("156c:a"), QStringLiteral("mk"), Kind::Anime,
                          QStringLiteral("A Deleted Series"), 12);
        seedQueue(Id::Kitsu, { { QStringLiteral("156c:a"), 3 },
                               { QStringLiteral("156c:b"), 5 },
                               { QStringLiteral("156c:c"), 7 } }, KT - 1000);
        {
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::Kitsu, kp, { { 422, 0, false }, { 200, 0, true }, { 201, 0, true } }, at,
                        sent, pushed, KT, waits,
                        QStringLiteral("Kitsu refused the update for %1 and it has been dropped; "
                                       "the rest are still queued."));
            CHECK(sent.size() == 3);
            CHECK(pushed.size() == 2);
            CHECK(pushed.value(0) == QLatin1String("156c:b@5"));
            CHECK(pushed.value(1) == QLatin1String("156c:c@7"));
            CHECK(TrackerQueue::count(Id::Kitsu) == 0);
            // ...and the run ENDED on a success, which correctly clears the status line. The drop's own
            // sentence is asserted in its own run below — a combined assertion here would be checking the
            // last thing that happened, not the drop.
            CHECK(TrackerQueue::lastError(Id::Kitsu).isEmpty());
        }
        // THE DROP IS VISIBLE, and it says WHICH one. A queue that quietly discards somebody's progress is
        // worse than one that wedges, because at least a wedge is eventually noticed.
        seedQueue(Id::Kitsu, { { QStringLiteral("156c:a"), 3 } }, KT - 1000);
        {
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::Kitsu, kp, { { 400, 0, false } }, at, sent, pushed, KT, waits,
                        QStringLiteral("Kitsu refused the update for %1 and it has been dropped; "
                                       "the rest are still queued."));
            const QString line = TrackerQueue::lastError(Id::Kitsu);
            CHECK(!line.isEmpty());
            CHECK(line.contains(QLatin1String("A Deleted Series")));   // WHICH item
            CHECK(line.contains(QLatin1String("dropped")));            // ...and what happened to it
            CHECK(line.contains(QLatin1String("Kitsu")));              // ...and on which tracker
            // Never a credential, never a request, never a response body.
            CHECK(!line.contains(QString::fromLatin1(kKitsuPassword)));
            CHECK(!line.contains(QLatin1String("Bearer")));
            CHECK(!line.contains(QLatin1String("http")));
        }
        // A SUCCESS clears the line and empties the queue, on Kitsu's queue as on the other two — and a
        // 201 (a created library entry) counts as one.
        TrackerQueue::setLastError(Id::Kitsu, QStringLiteral("something old"));
        seedQueue(Id::Kitsu, { { QStringLiteral("156c:two"), 9 } }, KT - 1000);
        {
            QStringList sent, pushed;
            QVector<qint64> waits;
            int at = 0;
            senderDrain(Id::Kitsu, kp, { { 201, 0, true } }, at, sent, pushed, KT + kDebounceMs * 4, waits);
            CHECK(pushed.value(0) == QLatin1String("156c:two@9"));
            CHECK(TrackerQueue::count(Id::Kitsu) == 0);
            CHECK(TrackerQueue::lastError(Id::Kitsu).isEmpty());
        }
        TrackerQueue::forgetAccount(Id::Kitsu);

        // ---- THE KITSU CREDENTIAL BYTE-SCAN ----------------------------------------------------------
        // §3 asserts AniList's secret is on disk EXACTLY ONCE and §18 asserts the same for MyAnimeList.
        // Kitsu's claim is STRONGER and is the right one for this design: the account password is never
        // stored at all, so it must occur ZERO times. Nothing this probe prints contains it.
        {
            QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                        QSettings::IniFormat);
            // Everything the Kitsu path really does write: the token pair and an expiry, through the
            // SHARED credential store — there is no Kitsu store.
            TrackerQueue::storeTokens(Id::Kitsu, QStringLiteral("KITSU-ACCESS-TOKEN-FIXTURE"),
                                      QStringLiteral("KITSU-REFRESH-TOKEN-FIXTURE"), 2592000, 1700000000);
            const QString kKey = QStringLiteral("tt-kitsu-scan");
            TrackerLinks::set(Id::Kitsu, kKey, QStringLiteral("1712"), Kind::Manga,
                              QStringLiteral("Berserk"), 364);
            Update qu;
            qu.itemKey = kKey;
            qu.mediaId = QStringLiteral("1712");
            qu.kind = Kind::Manga;
            qu.unit = 12;
            qu.atMs = 1'700'000'000'000LL;
            TrackerQueue::enqueue(Id::Kitsu, qu);
            TrackerQueue::setLastError(Id::Kitsu, QStringLiteral("Kitsu did not accept the update; "
                                                                "it is queued and will be retried."));
            s.sync();

            QFile f(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile));
            CHECK(f.open(QIODevice::ReadOnly));
            const QByteArray ini = f.readAll();
            f.close();
            CHECK(!ini.isEmpty());   // a scan of nothing passes trivially; assert the corpus first
            // ...and assert the credential EXISTS somewhere first, or "zero occurrences" is a statement
            // about a string that was never anywhere. It is in the GRANT BODY, which goes over TLS and is
            // never written down.
            CHECK(kitsu::passwordGrantBody(QString::fromLatin1(kKitsuEmail),
                                           QString::fromLatin1(kKitsuPassword)).contains(kKitsuPassword));

            // ZERO. Not once — never.
            CHECK(ini.indexOf(kKitsuPassword) < 0);
            // The email is not written either: it is a sign-in field held in memory, not a setting.
            CHECK(ini.indexOf(kKitsuEmail) < 0);
            // The TOKEN, by contrast, is stored — exactly once, on the access key inside the device-local
            // carve-out, which is where a token is supposed to be.
            int tokenHits = 0;
            for (int p = 0; (p = ini.indexOf("KITSU-ACCESS-TOKEN-FIXTURE", p)) >= 0; ++p) ++tokenHits;
            CHECK(tokenHits == 1);
            const int at = ini.indexOf("KITSU-ACCESS-TOKEN-FIXTURE");
            const int lineStart = ini.lastIndexOf('\n', at) + 1;
            CHECK(ini.mid(lineStart, at - lineStart).contains("access"));
            CHECK(isDeviceLocalKey(accessKey(Id::Kitsu)));
            CHECK(isDeviceLocalKey(refreshKey(Id::Kitsu)));
            CHECK(isDeviceLocalKey(queueKey(QString(), Id::Kitsu)));
            CHECK(isBackgroundStateKey(accessKey(Id::Kitsu)));
            CHECK(isBackgroundStateKey(queueKey(QString(), Id::Kitsu)));
            // NOTHING WAS WRITTEN UNDER THE CLIENT KEYS. Kitsu has no client to register, so those two
            // keys stay empty for it — asserted rather than assumed, because a copy-paste of MAL's
            // accessors would fill them in and nothing else would notice.
            CHECK(TrackerQueue::clientId(Id::Kitsu).isEmpty());
            CHECK(TrackerQueue::clientSecret(Id::Kitsu).isEmpty());

            // The artefacts that TRAVEL or get SHOWN carry neither the password nor the token.
            const QByteArray kq = encodeQueue(TrackerQueue::load(Id::Kitsu));
            CHECK(!kq.contains(kKitsuPassword));
            CHECK(!kq.contains("KITSU-ACCESS-TOKEN-FIXTURE"));
            const QByteArray kblob = TrackerLinks::encode(TrackerLinks::get(Id::Kitsu, kKey)).toUtf8();
            CHECK(!kblob.contains(kKitsuPassword));
            CHECK(!kblob.contains("KITSU-ACCESS-TOKEN-FIXTURE"));
            CHECK(!TrackerQueue::lastError(Id::Kitsu).contains(QString::fromLatin1(kKitsuPassword)));
            // The REQUESTS carry no credential at all: Kitsu authenticates with a header, by construction,
            // and every URL is built from ids and filters only.
            CHECK(!kitsu::searchUrl(api, QStringLiteral("Berserk"), 0, Kind::Manga, 8)
                       .contains(QLatin1String(kKitsuPassword)));
            CHECK(!kitsu::entryUrl(api, QStringLiteral("42"), QStringLiteral("1712"), Kind::Manga)
                       .contains(QLatin1String(kKitsuPassword)));
            CHECK(!kitsu::saveUrl(api, QStringLiteral("551")).contains(QLatin1String(kKitsuPassword)));
            CHECK(!kitsu::saveBody(qu, 364, QStringLiteral("551"), QStringLiteral("42"))
                       .contains(kKitsuPassword));
            CHECK(!kitsu::saveBody(qu, 364, QStringLiteral("551"), QStringLiteral("42"))
                       .contains("KITSU-ACCESS-TOKEN-FIXTURE"));

            TrackerQueue::clearTokens(Id::Kitsu);
            TrackerQueue::forgetAccount(Id::Kitsu);
        }
    }

    // ===== §21  THREE TRACKERS AT ONCE, AND THE TWO THAT WERE ALREADY THERE ============================
    // Decision 3 of the increment: three trackers configured at once must not fight or double-count, and
    // one failing must not block the others — pinned with THREE, not two, because a loop that survives one
    // bad element can still be written to give up on the second.
    //
    // Decision 5: AniList and MyAnimeList must be byte-identical to today when Kitsu is not configured.
    // That is the regression that would matter, and it is the second half of this section.
    {
        const QString key = QStringLiteral("tt156c");
        for (Id id : { Id::AniList, Id::MyAnimeList, Id::Kitsu }) TrackerLinks::clear(id, key);
        // The SAME series under THREE ids on three accounts, which is exactly what the link store's
        // (Id, itemKey) keying is for. Nothing in the fan-out ever hands one tracker another's media id.
        TrackerLinks::set(Id::AniList, key, QStringLiteral("20605"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);
        TrackerLinks::set(Id::MyAnimeList, key, QStringLiteral("31964"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);
        TrackerLinks::set(Id::Kitsu, key, QStringLiteral("7442"), Kind::Anime,
                          QStringLiteral("My Hero Academia"), 13);

        FakeTracker a(Id::AniList), m(Id::MyAnimeList), k(Id::Kitsu);
        const QVector<Tracker*> three{ &a, &m, &k };

        TrackerFanout::Result r = TrackerFanout::push(three, key, Kind::Anime, 5, false);
        CHECK(r.pushed == 3);
        CHECK(r.unlinked == 0);
        CHECK(r.needLink.isEmpty());
        CHECK(a.got.size() == 1);
        CHECK(m.got.size() == 1);
        CHECK(k.got.size() == 1);
        if (k.got.size() == 1)
        {
            CHECK(k.got[0].mediaId == QLatin1String("7442"));   // ITS OWN id, not one of the others'
            CHECK(k.got[0].unit == 5);
        }
        CHECK(TrackerLinks::get(Id::Kitsu, key).localUnits == 5);

        // ONE FAILING MUST NOT BLOCK THE OTHER TWO — in every position, because "it worked" can be an
        // artefact of the order. The failing one first, in the middle, and last.
        const QVector<QVector<Tracker*>> orders = { { &k, &a, &m }, { &a, &k, &m }, { &a, &m, &k } };
        int unit = 6;
        for (const QVector<Tracker*>& order : orders)
        {
            k.refuse = true;
            const int aBefore = a.got.size(), mBefore = m.got.size(), kBefore = k.refusals;
            r = TrackerFanout::push(order, key, Kind::Anime, unit++, false);
            CHECK(r.pushed == 3);                    // all three were VISITED
            CHECK(k.refusals == kBefore + 1);        // ...the broken one really did refuse
            CHECK(a.got.size() == aBefore + 1);      // ...and BOTH healthy ones still landed
            CHECK(m.got.size() == mBefore + 1);
            k.refuse = false;
        }
        // TWO of the three failing still leaves the third delivering.
        a.refuse = true;
        m.refuse = true;
        {
            const int kBefore = k.got.size();
            r = TrackerFanout::push(three, key, Kind::Anime, unit++, false);
            CHECK(r.pushed == 3);
            CHECK(k.got.size() == kBefore + 1);
        }
        a.refuse = false;
        m.refuse = false;

        // ONE OFF is skipped and says nothing about the other two. "Off" is not a failure.
        k.on_ = false;
        r = TrackerFanout::push(three, key, Kind::Anime, unit++, false);
        CHECK(r.pushed == 2);
        CHECK(TrackerFanout::active(three).size() == 2);
        k.on_ = true;
        CHECK(TrackerFanout::active(three).size() == 3);

        // NO DOUBLE COUNTING: one progress event produces at most ONE update per tracker, and the prompt
        // is offered for at most one of them — the others wait for the next event.
        {
            a.got.clear(); m.got.clear(); k.got.clear();
            TrackerFanout::push(three, key, Kind::Anime, unit, false);
            CHECK(a.got.size() == 1);
            CHECK(m.got.size() == 1);
            CHECK(k.got.size() == 1);
            ++unit;
        }
        // Unlinked on TWO of the three: both are offered, and the caller prompts for one of them.
        TrackerLinks::clear(Id::MyAnimeList, key);
        TrackerLinks::clear(Id::Kitsu, key);
        r = TrackerFanout::push(three, key, Kind::Anime, unit++, false);
        CHECK(r.pushed == 1);
        CHECK(r.unlinked == 2);
        CHECK(r.needLink.size() == 2);
        // ...and a decline on one of them takes it out of the offer, for ever, without touching the other.
        TrackerLinks::decline(Id::Kitsu, key);
        r = TrackerFanout::push(three, key, Kind::Anime, unit++, false);
        CHECK(r.unlinked == 2);
        CHECK(r.declined == 1);
        CHECK(r.needLink.size() == 1);
        if (r.needLink.size() == 1) CHECK(r.needLink[0]->id() == Id::MyAnimeList);

        // THREE QUEUES, ONE IMPLEMENTATION. TrackerQueue is keyed by Id, so a chapter one account refused
        // stays pending on that one alone — and three accounts do not share a rate limit either.
        for (Id id : { Id::AniList, Id::MyAnimeList, Id::Kitsu }) TrackerQueue::forgetAccount(id);
        Update u;
        u.itemKey = QStringLiteral("marks:series:mha");
        u.kind = Kind::Anime;
        u.unit = 3;
        u.atMs = 1'900'000'000'000LL;
        for (const QPair<Id, QString>& p : QVector<QPair<Id, QString>>{
                 { Id::AniList, QStringLiteral("20605") },
                 { Id::MyAnimeList, QStringLiteral("31964") },
                 { Id::Kitsu, QStringLiteral("7442") } })
        {
            Update x = u;
            x.mediaId = p.second;
            CHECK(TrackerQueue::enqueue(p.first, x));
        }
        CHECK(TrackerQueue::load(Id::AniList).first().mediaId == QLatin1String("20605"));
        CHECK(TrackerQueue::load(Id::MyAnimeList).first().mediaId == QLatin1String("31964"));
        CHECK(TrackerQueue::load(Id::Kitsu).first().mediaId == QLatin1String("7442"));
        // A send on one starts THAT ONE'S debounce and nothing else's.
        const qint64 now = 1'900'000'100'000LL;
        TrackerQueue::noteSent(Id::Kitsu, u.itemKey, now);
        qint64 wait = -1;
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::Kitsu), Id::Kitsu, now, &wait) == -1);
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::AniList), Id::AniList, now, &wait) == 0);
        CHECK(TrackerQueue::nextSendable(TrackerQueue::load(Id::MyAnimeList), Id::MyAnimeList, now, &wait)
              == 0);
        // Disconnecting ONE account drops that one's pending progress and nothing else's.
        TrackerQueue::setLastError(Id::Kitsu, QStringLiteral("K failed"));
        TrackerQueue::forgetAccount(Id::Kitsu);
        CHECK(TrackerQueue::count(Id::Kitsu) == 0);
        CHECK(TrackerQueue::lastError(Id::Kitsu).isEmpty());
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 1);
        for (Id id : { Id::AniList, Id::MyAnimeList }) TrackerQueue::forgetAccount(id);

        // ---- AND THE TWO THAT WERE ALREADY THERE ARE UNTOUCHED ---------------------------------------
        // The wire first: exact bodies, not shapes. A third provider must not have moved a byte of what
        // the first two send, and a "cleaned up" shared helper is exactly how it would.
        CHECK(anilist::defaultApiUrl() == QLatin1String("https://graphql.anilist.co"));
        CHECK(mal::defaultApiUrl() == QLatin1String("https://api.myanimelist.net/v2"));
        CHECK(anilist::tokenExchangeBody(QStringLiteral("cid"), QStringLiteral("sec"),
                                         QStringLiteral("http://127.0.0.1:1"), QStringLiteral("code"))
              == QByteArray(R"({"client_id":"cid","client_secret":"sec","code":"code",)"
                            R"("grant_type":"authorization_code","redirect_uri":"http://127.0.0.1:1"})"));
        CHECK(mal::tokenRefreshBody(QStringLiteral("cid"), QString(), QStringLiteral("rt"))
              == QByteArray("client_id=cid&grant_type=refresh_token&refresh_token=rt"));
        Update au;
        au.itemKey = QStringLiteral("k");
        au.mediaId = QStringLiteral("30002");
        au.kind = Kind::Manga;
        au.unit = 12;
        CHECK(anilist::saveBody(au, 364)
                  .contains(R"("variables":{"mediaId":30002,"progress":12,"status":"CURRENT"})"));
        CHECK(mal::saveBody(au, 364) == QByteArray("status=reading&num_chapters_read=12"));
        // ...and nothing of Kitsu's leaked into either: no JSON:API document, no ratingTwenty, no
        // library-entries.
        CHECK(!anilist::saveBody(au, 364).contains("ratingTwenty"));
        CHECK(!anilist::saveBody(au, 364).contains("libraryEntries"));
        CHECK(!mal::saveBody(au, 364).contains("ratingTwenty"));
        CHECK(!mal::saveBody(au, 364).contains("progress"));
        // The three policies stay three: AniList still refuses to call 422 permanent, which is the one
        // difference #326 documented and the one a third provider could have flattened.
        CHECK(!classifySend(anilist::sendPolicy(), 422, 0, 1).permanent);
        CHECK(classifySend(mal::sendPolicy(), 422, 0, 1).permanent);
        CHECK(classifySend(kitsu::sendPolicy(), 422, 0, 1).permanent);

        // The state second: every key each tracker uses is distinct from the other two's, so nothing one
        // stores can be read back as another's — the reason Tracker.h RESERVED Kitsu's id in increment 1
        // rather than inventing it now.
        CHECK(queueKey(QString(), Id::Kitsu).contains(QLatin1String("/kitsu/")));
        CHECK(queueKey(QString(), Id::Kitsu) != queueKey(QString(), Id::AniList));
        CHECK(queueKey(QString(), Id::Kitsu) != queueKey(QString(), Id::MyAnimeList));
        CHECK(accessKey(Id::Kitsu) == QLatin1String("tracker/kitsu/access"));
        CHECK(TrackerLinks::hashFor(Id::Kitsu, key) != TrackerLinks::hashFor(Id::AniList, key));
        CHECK(TrackerLinks::hashFor(Id::Kitsu, key) != TrackerLinks::hashFor(Id::MyAnimeList, key));

        // ---- the proof by construction: write EVERYTHING Kitsu owns, then read the other two back ----
        QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                    QSettings::IniFormat);
        const QString bothKey = QStringLiteral("tt-before-kitsu");
        for (Id id : { Id::AniList, Id::MyAnimeList, Id::Kitsu }) TrackerLinks::clear(id, bothKey);
        s.setValue(clientIdKey(Id::AniList), QString::fromLatin1(kFixtureClientId));
        s.setValue(accessKey(Id::AniList), QStringLiteral("ACCESS-TOKEN-FIXTURE"));
        s.setValue(clientIdKey(Id::MyAnimeList), QString::fromLatin1(kMalClientId));
        s.setValue(accessKey(Id::MyAnimeList), QStringLiteral("MAL-ACCESS-TOKEN-FIXTURE"));
        TrackerLinks::set(Id::AniList, bothKey, QStringLiteral("30002"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        TrackerLinks::set(Id::MyAnimeList, bothKey, QStringLiteral("2"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        for (Id id : { Id::AniList, Id::MyAnimeList }) TrackerQueue::forgetAccount(id);
        Update qu;
        qu.itemKey = bothKey;
        qu.kind = Kind::Manga;
        qu.unit = 6;
        qu.atMs = 1'700'000'000'000LL;
        qu.mediaId = QStringLiteral("30002");
        TrackerQueue::enqueue(Id::AniList, qu);
        qu.mediaId = QStringLiteral("2");
        TrackerQueue::enqueue(Id::MyAnimeList, qu);
        s.sync();
        const QString aLinkBefore = TrackerLinks::encode(TrackerLinks::get(Id::AniList, bothKey));
        const QString mLinkBefore = TrackerLinks::encode(TrackerLinks::get(Id::MyAnimeList, bothKey));
        const QByteArray aQueueBefore = encodeQueue(TrackerQueue::load(Id::AniList));
        const QByteArray mQueueBefore = encodeQueue(TrackerQueue::load(Id::MyAnimeList));

        // Now Kitsu arrives, in full: a token pair, a link on the SAME item, a queue and an error line.
        TrackerQueue::storeTokens(Id::Kitsu, QStringLiteral("KITSU-ACCESS-TOKEN-FIXTURE"),
                                  QStringLiteral("KITSU-REFRESH-TOKEN-FIXTURE"), 2592000, 1700000000);
        TrackerLinks::set(Id::Kitsu, bothKey, QStringLiteral("1712"), Kind::Manga,
                          QStringLiteral("Berserk"), 364);
        qu.mediaId = QStringLiteral("1712");
        qu.unit = 99;
        TrackerQueue::enqueue(Id::Kitsu, qu);
        TrackerQueue::setLastError(Id::Kitsu, QStringLiteral("Kitsu is unhappy"));
        s.sync();

        // ...and both of the others are byte-for-byte what they were.
        CHECK(TrackerLinks::encode(TrackerLinks::get(Id::AniList, bothKey)) == aLinkBefore);
        CHECK(TrackerLinks::encode(TrackerLinks::get(Id::MyAnimeList, bothKey)) == mLinkBefore);
        CHECK(encodeQueue(TrackerQueue::load(Id::AniList)) == aQueueBefore);
        CHECK(encodeQueue(TrackerQueue::load(Id::MyAnimeList)) == mQueueBefore);
        CHECK(TrackerQueue::count(Id::AniList) == 1);
        CHECK(TrackerQueue::count(Id::MyAnimeList) == 1);
        CHECK(TrackerQueue::lastError(Id::AniList).isEmpty());
        CHECK(TrackerQueue::lastError(Id::MyAnimeList).isEmpty());
        CHECK(s.value(accessKey(Id::AniList)).toString() == QLatin1String("ACCESS-TOKEN-FIXTURE"));
        CHECK(s.value(accessKey(Id::MyAnimeList)).toString() == QLatin1String("MAL-ACCESS-TOKEN-FIXTURE"));
        CHECK(s.value(clientIdKey(Id::AniList)).toString() == QString::fromLatin1(kFixtureClientId));
        CHECK(s.value(clientIdKey(Id::MyAnimeList)).toString() == QString::fromLatin1(kMalClientId));

        for (Id id : { Id::AniList, Id::MyAnimeList, Id::Kitsu }) TrackerQueue::forgetAccount(id);
        TrackerQueue::clearTokens(Id::Kitsu);
    }

    if (failures == 0) { std::puts("TRACKER-OK"); return 0; }
    std::fprintf(stderr, "TRACKER: %d check(s) failed\n", failures);
    return 1;
}
