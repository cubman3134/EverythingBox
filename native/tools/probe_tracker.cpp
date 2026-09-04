// Headless check of the tracker seam and the AniList rules layer (issue #156, increment 1).
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
#include "TrackerLinks.h"
#include "TrackerRules.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <cstdio>

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

static QJsonObject varsOf(const QByteArray& body)
{
    return QJsonDocument::fromJson(body).object().value(QStringLiteral("variables")).toObject();
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

    if (failures == 0) { std::puts("TRACKER-OK"); return 0; }
    std::fprintf(stderr, "TRACKER: %d check(s) failed\n", failures);
    return 1;
}
