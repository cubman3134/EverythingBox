// Auto-downloads an external subtitle (.srt) from OpenSubtitles.com (the current REST API) for a movie or
// TV episode when the video has no subtitle in the user's preferred language. Matching walks a precision
// chain: the OSDb moviehash of the local file (exact rip), then the IMDB id (+ season/episode for TV), then
// a title query. Everything runs async on the GUI thread; the result callback fires with a local .srt path
// (empty on failure or when unconfigured). This class is pure transport — the download cache is app-owned.
//
// OpenSubtitles needs an application API key for everything, and the user's own account only to DOWNLOAD
// (#81). So the two are separate questions here: canSearch() is "an API key is in use" (the one the user typed,
// else the one built into this release), and canDownload() adds "and a username + password are stored". A
// search never logs in; the first download without a login reports NeedsLogin so the window can ask for one.
#pragma once
#include "BuiltinSecretBlob.h"   // BuiltinSecret::Resolved: the API key in use, and whether it is built in
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

    // #81's split. canSearch: an API key is in use (the user's, else the built-in one); no account needed.
    // canDownload: that, plus the user's OpenSubtitles username and password. The automatic on-open fetch
    // asks canDownload (it downloads without asking anybody); the manual picker asks canSearch and prompts
    // for a login at the first download.
    static bool canSearch();
    static bool canDownload();
    // The API key every request carries, resolved by BuiltinSecret::resolve over the user's typed key and the
    // built-in one. usingBuiltinKey() is what the settings row asks before it says "Built in".
    static BuiltinSecret::Resolved apiKey();
    static bool usingBuiltinKey();

    // Where requests go. Production is always https://api.opensubtitles.com/api/v1. A test rig may point the
    // app at a LOOPBACK fake instead (EB_UITEST_OPENSUBTITLES_BASE, e.g. http://127.0.0.1:8123), and only
    // with EB_UITEST set: an ordinary run ignores the variable, as #98's EB_UITEST_BUILDBOT_BASE is ignored,
    // and a non-loopback value is ignored even under EB_UITEST, because this decides where an API key and a
    // password are sent. apiRootFor is the pure policy; apiRoot() applies it to the environment.
    static QString apiRootFor(bool uitest, const QString& overrideBase);
    static QString apiRoot();

    // How a download ended, so the window can tell "needs your login" from "failed".
    enum class DownloadResult { Ok, NeedsLogin, LoginRefused, Failed };

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
    // Download one specific row the user chose from searchList(). With no login stored this makes NO request
    // and answers NeedsLogin; a login OpenSubtitles refuses answers LoginRefused. srtPath is set only for Ok.
    void downloadChoice(qint64 fileId, const QString& langCode,
                        std::function<void(const QString& srtPath, DownloadResult result)> cb);

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
    // Log in once and keep the token. refused is true when OpenSubtitles answered and said no (401/403), as
    // opposed to not answering at all.
    void ensureLogin(std::function<void(bool ok, bool refused)> done);
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
    QString root_;    // where /login goes: apiRoot() at construction
    QString apiBase_; // where everything else goes: root_, or the host /login named (a VIP host)
};
