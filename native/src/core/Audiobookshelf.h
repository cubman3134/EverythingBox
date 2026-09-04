// THE AUDIOBOOKSHELF PROTOCOL, AS PURE DECISIONS (issue #197, increment 1).
//
// Everything about talking to an Audiobookshelf server that can be decided WITHOUT a socket: the id
// scheme, the URL rules, the request bodies, the readers over its JSON, the chapter arithmetic, and the
// question "is this position worth telling the server about yet". AbsClient.h is what is left after that —
// which server, which request, what to do with the reply.
//
// The split is Subsonic.h's, deliberately and for the same reason: probe_absclient can drive the whole of
// this against fixture bytes in a few lines, with no Qt beyond QtCore, and the parts that genuinely need
// the world are driven separately against a fixture HTTP stub.
//
// ==================================================================================================
// THE ID SCHEME, AND WHY IT IS SERVER-QUALIFIED FROM THE FIRST COMMIT
// ==================================================================================================
//
//      abs:<serverId>:<itemId>              a library item — a book, or a podcast
//      abs:<serverId>:<itemId>#<episodeId>  one episode of a podcast
//
// An Audiobookshelf item id is unique ON ONE SERVER. Two servers hand out ids from the same space and
// nothing in the id says which one it came from. So the moment two servers can be configured — which is
// the moment this feature ships, not some later increment — an unqualified id is ambiguous, and every
// resume mark, favourite, playlist row and Recents entry already written under one is unqualifiable after
// the fact. There is no migration for that: the information was never there.
//
// SubsonicServerStore.h makes the same argument at length for music servers, and #160 sets it as the rule
// for anything with a server behind it. This is that rule, applied on day one.
//
// WHAT `serverId` IS. The identity of the SAVED SERVER — a uuid AbsServerStore mints when the server is
// added, or, when the server publishes an id of its own on `/api/authorize`, that. Never the URL: a URL is
// a location and a location changes (a LAN address, a reverse proxy moving, http becoming https), and an
// id that changed would orphan every mark keyed by it. Fixed once, at add time, and never rewritten —
// including when the server later starts publishing an id it did not before, because rewriting it then
// would orphan exactly the marks it was supposed to protect.
//
// (Audiobookshelf as of this writing does not publish a distinct per-INSTANCE id: `/api/authorize` carries
// `serverSettings`, whose own `id` is the constant string "server-settings" on every install. serverIdOf()
// therefore refuses a value that is not distinguishing, and the minted uuid stands. The reader is written
// anyway, because the decision is "the server's id if it has one", and a server that grows one should be
// adopted by new rows without this file changing.)
//
// WHY THE KIND IS NOT IN THE ID. A browse row already carries its routing kind in its `mime` prefix —
// "absseries:", "absbook:", "absepisode:" — which is how every other synthetic category in this app routes
// (AudiobookCatalogs.h states the contract). Putting the kind in the ID as well would be two spellings of
// one fact, and the one that would rot is the one nothing reads back. So an id names a THING on a SERVER,
// and what the row does with it is the row's business.
//
// ==================================================================================================
// THE TOKEN, AND THE THREE PLACES IT MAY BE
// ==================================================================================================
// A user types a URL, a username and a password. `/login` turns those into an API TOKEN and the password is
// then dropped on the floor — it is never stored, never held past the login call, and AbsServer has no
// field it could go in. What is stored is the token, under the "audiobookshelf/" ini prefix that
// CloudSync::isDeviceLocalKey carves out of the synced settings bundle (a synced bundle is a zip in
// somebody's Drive folder; a token in it is a token on a third party's disk).
//
// The token reaches the server three ways and no others:
//
//   1. `Authorization: Bearer <token>` on every API request. A HEADER, so it is not in the URL, so it
//      cannot reach a log line, a status bar, or QNetworkReply::errorString() — which embeds the URL, and
//      which SubsonicClient.h documents as the exact way a credential gets into a bug report.
//   2. `?token=<token>` on the STREAM url, because that url is handed to mpv and mpv is not given headers
//      on this path (openRemoteAudiobook says why: a header list bound to part one's origin is wrong for
//      every other part). This url is minted at the moment a part is reached and is never written down.
//   3. `?token=<token>` on the COVER url, whose bytes go into MetaCache — which records the FILE NAME, not
//      the url it came from.
//
// Nothing else. In particular there is no function here that renders a request, and `logSafeUrl` is what
// the caller uses if it must name one at all.
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Abs
{
// ==================================================================================================
// The id scheme
// ==================================================================================================
inline const char* kScheme = "abs:";
inline QChar episodeSep() { return QLatin1Char('#'); }

// What a qualified id names.
struct Ref
{
    bool    ok = false;
    QString serverId;    // the SAVED server's id (see the header) — never a URL
    QString itemId;      // the Audiobookshelf library-item id
    QString episodeId;   // a podcast episode inside that item, or empty
    bool isEpisode() const { return ok && !episodeId.isEmpty(); }
};

// "abs:<serverId>:<itemId>", or empty when either half is unusable.
//
// REFUSES a serverId or itemId carrying ':' or '#' rather than escaping it, because an escape is a second
// thing parse() would have to get right and every id in play here is a uuid or an Audiobookshelf id
// ("li_8x9…") — neither contains either character. A refusal is visible at the one moment it can happen
// (adding a server); an escape bug is a wrong book, months later.
QString qualify(const QString& serverId, const QString& itemId);
QString qualifyEpisode(const QString& serverId, const QString& itemId, const QString& episodeId);

// The inverse. `ok` is false for anything that is not one of ours — a file path, a http url, a Subsonic
// key — so a caller can hand it every queue entry and let it decide.
Ref parse(const QString& s);

inline bool    isQualified(const QString& s) { return parse(s).ok; }
inline QString serverOf(const QString& s)    { const Ref r = parse(s); return r.ok ? r.serverId : QString(); }
inline QString itemOf(const QString& s)      { const Ref r = parse(s); return r.ok ? r.itemId   : QString(); }

// The id of the ITEM a qualified episode id belongs to ("abs:s:i#e" -> "abs:s:i"). The progress a podcast
// episode reports is keyed by BOTH, but the cover and the item fetch are the item's.
QString itemIdOf(const QString& qualified);

// ==================================================================================================
// The server address
// ==================================================================================================
// Same shape and the same refusal as Subsonic::checkUrl: plain HTTP is a decision the user makes out loud,
// not a downgrade this code performs quietly. A password is posted to /login over this URL.
enum class UrlVerdict { Ok, Malformed, NotHttp, InsecureRefused };
UrlVerdict checkUrl(const QString& url, bool allowPlainHttp);

// The URL with its trailing slashes removed, ready to have a path appended. Empty for a URL checkUrl
// refuses — there is no fallback, because there is no other server this could mean.
QString normalizeRoot(const QString& url, bool allowPlainHttp);

// ==================================================================================================
// The endpoints
// ==================================================================================================
// Spelled once, here, because two spellings of an endpoint is how a client stops recognising its own URLs.
QString loginPath();                                              // POST, no auth: username+password -> token
QString authorizePath();                                          // POST, Bearer: who am I / which server
QString librariesPath();                                          // GET
QString libraryItemsPath(const QString& libraryId, int limit);     // GET
QString librarySeriesPath(const QString& libraryId);               // GET
QString libraryAuthorsPath(const QString& libraryId);              // GET
QString itemPath(const QString& itemId);                           // GET, expanded
QString playPath(const QString& itemId, const QString& episodeId); // POST -> a play session
QString progressPath(const QString& itemId, const QString& episodeId); // GET / PATCH
QString coverPath(const QString& itemId);                          // GET (token in the query)

// The login body. The password appears HERE and nowhere else in the codebase's own data: it is built at
// the moment of the request out of what the user just typed, and the caller holds no copy.
QJsonObject loginBody(const QString& username, const QString& password);

// The progress body for PATCH /api/me/progress/<id>. `duration` of 0 is legal (a live/unknown-length
// item) and simply omits the fraction, because a fraction over an unknown total is a made-up number.
QJsonObject progressBody(double currentTime, double duration);

// The stream url for one track of a play session. `contentUrl` is the server's own path
// ("/api/items/li_x/file/af_y"); this only joins it to the root and attaches the token.
//
// CREDENTIAL-BEARING BY CONSTRUCTION. It is minted at the moment the player is handed a part and is never
// stored, logged, or written into a queue — see RemoteAudiobook.h, whose whole argument is this one.
QString streamUrl(const QString& root, const QString& contentUrl, const QString& token);
QString coverUrl(const QString& root, const QString& itemId, const QString& token);

// ==================================================================================================
// The payloads
// ==================================================================================================
struct Login
{
    bool    ok = false;
    QString token;      // DEVICE-LOCAL from here on. Never logged, never synced, never shown.
    QString username;   // echoed back by the server; what the settings row displays
};
Login readLogin(const QByteArray& body);

// The server's own identity out of /api/authorize, or empty when it publishes nothing DISTINGUISHING.
// "server-settings" — the constant every Audiobookshelf install answers with — is treated as nothing,
// because an id shared by every server in the world qualifies nothing. See the header.
QString serverIdOf(const QByteArray& body);

struct Library
{
    QString id;
    QString name;
    QString mediaType;                                             // "book" or "podcast"
    bool isPodcast() const { return mediaType == QLatin1String("podcast"); }
};
QVector<Library> readLibraries(const QByteArray& body);

// One row of a library listing: a book, or a podcast. The fields are what a SHELF needs and nothing more —
// no tracks, no chapters, because a listing of four hundred books that carried them would be megabytes.
struct Item
{
    QString id;
    QString title;
    QString author;
    QString narrator;
    QString series;
    QString seriesSequence;
    double  duration = 0.0;      // seconds; 0 when the server has not scanned it
    int     trackCount = 0;
    int     episodeCount = 0;    // podcasts only
    bool    isPodcast = false;
    bool    hasCover = false;
};
QVector<Item> readLibraryItems(const QByteArray& body);

struct SeriesRow { QString id; QString name; int bookCount = 0; };
QVector<SeriesRow> readSeries(const QByteArray& body);

struct AuthorRow { QString id; QString name; int bookCount = 0; };
QVector<AuthorRow> readAuthors(const QByteArray& body);

// One episode of a podcast. `id` is the EPISODE id, which is what progress is keyed by alongside the item.
struct Episode
{
    QString id;
    QString title;
    QString subtitle;
    QString published;
    double  duration = 0.0;
};
// One audio file of a book, in play order.
struct Track
{
    int     index = 0;
    QString title;         // the server's own file title — the queue's display row
    QString contentUrl;    // the server's path; streamUrl() turns it into something mpv can open
    double  startOffset = 0.0;  // where this track begins in the WHOLE book
    double  duration = 0.0;
    QString mimeType;
};
// One chapter, in BOOK time — Audiobookshelf's chapter list spans the whole item, not one file.
struct Chapter { double start = 0.0; double end = 0.0; QString title; };

// The expanded item: its own row, its files and chapters, plus (for a podcast) its episodes. Read from
// GET /api/items/<id>?expanded=1.
//
// WHY THE BOOK LEVEL IS BUILT FROM THIS AND NOT FROM A PLAY SESSION. The two carry the same tracks, and
// POST /play would give them in one call — but /play OPENS A LISTENING SESSION on the server, which shows
// up in its "listening now" surfaces and its listening statistics. Walking into a book's page to look at
// what is in it is not listening to it, and a client that recorded it as listening would be lying to the
// server about its own user. So browsing reads, and playing plays.
struct ItemDetail
{
    bool             ok = false;
    Item             item;
    QVector<Track>   tracks;
    QVector<Chapter> chapters;
    QVector<Episode> episodes;
};
ItemDetail readItem(const QByteArray& body);

// What POST /api/items/<id>/play answers with.
//
// `currentTime` is THE SERVER'S RESUME POINT, and it arriving here is why seeding a resume from the server
// costs no extra request: opening the book and asking where the user was are the same call.
//
// `title` matters for one route and it is not the obvious one. Opening a book by BROWSING to it means the
// expanded item is already cached and its title is right there — so the field looks redundant, which is
// exactly how it was missed. Re-opening from RECENTS reaches this call with nothing else fetched, and a
// book that came back titled "01 - One.mp3" (the first track) is what the first live drive showed.
struct Session
{
    bool             ok = false;
    QString          id;
    QString          title;      // the ITEM's own title, off the reply's `libraryItem`
    QVector<Track>   tracks;
    QVector<Chapter> chapters;
    double           duration = 0.0;
    double           currentTime = 0.0;
};
Session readPlaySession(const QByteArray& body);

struct Progress
{
    bool   found = false;
    double currentTime = 0.0;
    double duration = 0.0;
    bool   finished = false;
};
Progress readProgress(const QByteArray& body);

// ==================================================================================================
// The book's timeline
// ==================================================================================================
// An Audiobookshelf book is one item and one chapter list, and it is between one and several hundred audio
// FILES. The player holds one file at a time. Everything below turns "where am I in this file" into "where
// am I in this book" and back, so the chapter list, the resume seed and the progress report can all be in
// the book's own time — which is the only time the server speaks.
//
// EXACT, not estimated. #218's BookTimeline prices a position out of PART SIZES because a torrent release
// says how big its files are and nothing else; a play session says how LONG each track is and where it
// starts, so there is nothing here to estimate and the two must not be confused.

// The absolute position in the book, given the track that is playing and the position within it.
double absoluteTime(const QVector<Track>& tracks, int trackIndex, double within);

// The inverse: which track an absolute book position falls in, and how far into it. Returns -1 for an
// empty track list. Clamps rather than refusing — a server position past the end of the book (the item was
// re-scanned shorter) resolves to the last track rather than to nothing.
int    trackAtTime(const QVector<Track>& tracks, double absolute);
double offsetWithinTrack(const QVector<Track>& tracks, double absolute);

// The chapters that fall inside ONE track, rebased so their times are positions IN THAT TRACK.
//
// WHY REBASED. The player's chapter navigation, the sleep timer's "end of chapter" and the per-book speed
// memory all read a chapter list in the time base of the file that is open (MpvWidget::chapters() is mpv's
// own list, which is exactly that). Handing them the book's list would put every chapter of a
// fifteen-hour book at a time the six-minute file that is playing has never heard of, and "end of chapter"
// would then be either instant or never.
//
// A chapter STRADDLING the track boundary is kept, clamped to the track. That is deliberate: the region
// the listener is in has to be in the list or the sleep timer cannot find it, and a chapter that begins in
// the previous file is still the chapter they are listening to.
QVector<Chapter> chaptersForTrack(const QVector<Chapter>& chapters, double trackStart, double trackDuration);

// ==================================================================================================
// When to tell the server
// ==================================================================================================
// The hook this rides is PlaybackSession::persistResume, which is already throttled to a 5-second position
// change — right for an ini write and far too chatty for a network round trip against somebody's Raspberry
// Pi. So there is a second gate, and it is stated here rather than inside the client so a probe can pin it
// without a socket.
//
// THREE RULES, and the first two are what make the third safe to be as slow as it is:
//
//   * NOTHING SENT YET -> send. The first position of a session is what makes a resume point exist at all,
//     and a listener who plays ten seconds and closes the app must not lose them.
//   * A JUMP -> send. A position that moved further than playback could have moved is a SEEK, which is the
//     one user action whose whole point is that the position changed; waiting ten seconds to report it
//     means a second device that syncs in that window gets the old place.
//   * OTHERWISE, once every kReportIntervalMs. Ordinary listening.
inline constexpr qint64 kReportIntervalMs = 10000;
inline constexpr double kSeekJumpS        = 30.0;

bool shouldReport(bool everSent, double lastSentPos, qint64 lastSentMs, double pos, qint64 nowMs);
} // namespace Abs
