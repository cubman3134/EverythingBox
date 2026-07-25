// Auto-downloads an external subtitle (.srt) from OpenSubtitles.com (the current REST API) for a movie or
// TV episode when the video has no subtitle in the user's preferred language. Matching walks a precision
// chain: the OSDb moviehash of the local file (exact rip), then the IMDB id (+ season/episode for TV), then
// a title query. Everything runs async on the GUI thread; the result callback fires with a local .srt path
// (empty on failure or when unconfigured). This class is pure transport — the download cache is app-owned.
//
// OpenSubtitles requires an app API key (register once, free) for search, and the user's account (a login
// token is required to download). Credentials come from Settings; the feature is dormant until they're set.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>        // std::shared_ptr (the tier walkers)

class QNetworkAccessManager;

// One search result row, for the manual picker: the file to download plus what the user needs to choose by.
struct SubtitleCandidate
{
    qint64  fileId = 0;
    QString language;
    QString release;      // the release name (falls back to the file name) — how a user recognises the rip
    int     downloads = 0;
};

class SubtitleFetcher : public QObject
{
    Q_OBJECT
public:
    explicit SubtitleFetcher(QObject* parent = nullptr);

    // True once an API key + username + password are all present in Settings.
    static bool configured();

    // Fetch a subtitle. imdbStreamId: "tt123" (movie) or "ttShow:season:episode" (episode); title is used
    // for a query search when there's no IMDB id. langCode is the ISO-639 code from Settings ("eng"/"en"…),
    // mapped to the API's 2-letter form. localPath, when non-empty, is the video file on disk: it enables the
    // most precise tier (the OSDb moviehash, which matches THIS exact rip). cb receives a local .srt path, or
    // "" on any failure / when unconfigured.
    void fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
               const QString& localPath, std::function<void(const QString& srtPath)> cb);
    // Streaming callers with no file on disk: same as above with an empty localPath.
    void fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
               std::function<void(const QString& srtPath)> cb);

    // Same match chain as fetch(), but returns every row of the first tier that matched (most-downloaded
    // first) instead of auto-picking one — the manual picker's source of choices. Empty on any failure.
    void searchList(const QString& imdbStreamId, const QString& title, const QString& langCode,
                    const QString& localPath, std::function<void(const QVector<SubtitleCandidate>&)> cb);
    // Download one specific row the user chose from searchList().
    void downloadChoice(qint64 fileId, const QString& langCode,
                        std::function<void(const QString& srtPath)> cb);

    // The identifier the download cache should key on for this request: whichever tier will actually match —
    // "hash:<osdb>" when the file is hashable, else the imdb stream id, else "title:<title>". The hash:/title:
    // prefixes keep a title from colliding with an IMDB id in the same key space.
    static QString cacheIdentifier(const QString& imdbStreamId, const QString& title,
                                   const QString& localPath);

    // The ordered search queries for one request, most precise first (moviehash → imdb → title). PUBLIC for
    // probe coverage: this is the heart of the match chain (tier ORDER + which tiers are even emitted), it is
    // pure (strings in, strings out — the only I/O is hashing the file it is handed), so probe_subs asserts it
    // directly rather than through the network transport.
    static QStringList buildQueries(const QString& imdbStreamId, const QString& title,
                                    const QString& lang, const QString& localPath);

signals:
    void log(const QString& line); // status for the debug log; credentials are never included

private:
    void ensureLogin(std::function<void(bool ok)> done);
    // Run a /subtitles GET with the given query string (already URL-encoded); parse the best file id.
    void searchQuery(const QString& query, const QString& lang,
                     std::function<void(qint64 fileId)> done);
    // The same GET, keeping every parsed row (most-downloaded first) instead of only the winner.
    void searchCandidates(const QString& query,
                          std::function<void(const QVector<SubtitleCandidate>&)> done);
    void download(qint64 fileId, const QString& lang,
                  std::function<void(const QString& srtPath)> done);
    // Tier walkers. NAMED members, not self-referencing std::functions: a lambda captured inside the
    // shared_ptr that owns it is a reference cycle and leaks on every call. The shared_ptr here holds
    // only the query list (plain data), so each async hop keeps it alive without any cycle.
    void stepFetch(std::shared_ptr<QStringList> queries, int i, const QString& lang,
                   std::function<void(const QString& srtPath)> cb);
    void stepSearch(std::shared_ptr<QStringList> queries, int i,
                    std::function<void(const QVector<SubtitleCandidate>&)> cb);

    QNetworkAccessManager* nam_ = nullptr;
    QString token_;   // login token (in-memory; re-fetched on expiry / 401)
    QString apiHost_; // API host from /login (defaults to api.opensubtitles.com)
};
