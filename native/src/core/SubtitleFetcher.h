// Auto-downloads an external subtitle (.srt) from OpenSubtitles.com (the current REST API) for a movie or
// TV episode when the video has no subtitle in the user's preferred language. Matching is by IMDB id
// (+ season/episode for TV), falling back to a title query. Everything runs async on the GUI thread; the
// result callback fires with a local .srt path (empty on failure or when unconfigured).
//
// OpenSubtitles requires an app API key (register once, free) for search, and the user's account (a login
// token is required to download). Credentials come from Settings; the feature is dormant until they're set.
#pragma once
#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;

class SubtitleFetcher : public QObject
{
    Q_OBJECT
public:
    explicit SubtitleFetcher(QObject* parent = nullptr);

    // True once an API key + username + password are all present in Settings.
    static bool configured();

    // Fetch a subtitle. imdbStreamId: "tt123" (movie) or "ttShow:season:episode" (episode); title is used
    // for a query search when there's no IMDB id. langCode is the ISO-639 code from Settings ("eng"/"en"…),
    // mapped to the API's 2-letter form. cb receives a local .srt path, or "" on any failure / when unconfigured.
    void fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
               std::function<void(const QString& srtPath)> cb);

signals:
    void log(const QString& line); // status for the debug log; credentials are never included

private:
    void ensureLogin(std::function<void(bool ok)> done);
    // Run a /subtitles GET with the given query string (already URL-encoded); parse the best file id.
    void searchQuery(const QString& query, const QString& lang,
                     std::function<void(qint64 fileId)> done);
    void download(qint64 fileId, const QString& lang,
                  std::function<void(const QString& srtPath)> done);

    QNetworkAccessManager* nam_ = nullptr;
    QString token_;   // login token (in-memory; re-fetched on expiry / 401)
    QString apiHost_; // API host from /login (defaults to api.opensubtitles.com)
};
