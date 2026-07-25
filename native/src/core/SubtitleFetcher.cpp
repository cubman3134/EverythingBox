#include "SubtitleFetcher.h"
#include "Settings.h"
#include "SubtitleHash.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QHash>
#include <algorithm>
#include <memory>
#include <utility>

namespace {
constexpr const char* kDefaultHost = "api.opensubtitles.com";

// The API wants ISO 639-1 (2-letter). Settings stores 3-letter codes ("eng"); map the ones the UI offers,
// pass a 2-letter code through unchanged, and default to English when there's no preference.
QString apiLang(const QString& code)
{
    const QString c = code.trimmed().toLower();
    if (c.isEmpty()) return QStringLiteral("en");
    if (c.size() == 2) return c;
    static const QHash<QString, QString> m = {
        { QStringLiteral("eng"), QStringLiteral("en") }, { QStringLiteral("spa"), QStringLiteral("es") },
        { QStringLiteral("fra"), QStringLiteral("fr") }, { QStringLiteral("fre"), QStringLiteral("fr") },
        { QStringLiteral("deu"), QStringLiteral("de") }, { QStringLiteral("ger"), QStringLiteral("de") },
        { QStringLiteral("ita"), QStringLiteral("it") }, { QStringLiteral("por"), QStringLiteral("pt") },
        { QStringLiteral("nld"), QStringLiteral("nl") }, { QStringLiteral("dut"), QStringLiteral("nl") },
        { QStringLiteral("rus"), QStringLiteral("ru") }, { QStringLiteral("jpn"), QStringLiteral("ja") },
        { QStringLiteral("kor"), QStringLiteral("ko") }, { QStringLiteral("zho"), QStringLiteral("zh") },
        { QStringLiteral("chi"), QStringLiteral("zh") }, { QStringLiteral("ara"), QStringLiteral("ar") },
    };
    return m.value(c, c.left(2));
}
} // namespace

SubtitleFetcher::SubtitleFetcher(QObject* parent)
    : QObject(parent), apiHost_(QString::fromLatin1(kDefaultHost)) {}

bool SubtitleFetcher::configured()
{
    return !Settings::openSubApiKey().isEmpty()
        && !Settings::openSubUsername().isEmpty()
        && !Settings::openSubPassword().isEmpty();
}

// Every request carries the API key + a descriptive User-Agent (OpenSubtitles requires one). GET search and
// POST download both need it; authenticated calls add the bearer token.
static QNetworkRequest makeRequest(const QString& host, const QString& path, bool withToken,
                                   const QString& token)
{
    QNetworkRequest rq{ QUrl(QStringLiteral("https://%1/api/v1%2").arg(host, path)) };
    rq.setRawHeader("Api-Key", Settings::openSubApiKey().toUtf8());
    rq.setHeader(QNetworkRequest::UserAgentHeader,
                 QStringLiteral("MyMediaVault v%1").arg(QCoreApplication::applicationVersion()));
    rq.setRawHeader("Accept", "application/json");
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setTransferTimeout(20000);
    if (withToken && !token.isEmpty())
        rq.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    return rq;
}

// Ordered search queries for this request, most precise first: OSDb moviehash (exact release — only when we
// have local bytes), then IMDB id (+season/episode), then a title query. Each already carries languages=.
QStringList SubtitleFetcher::buildQueries(const QString& imdbStreamId, const QString& title,
                                          const QString& lang, const QString& localPath)
{
    QStringList out;
    if (!localPath.isEmpty())
    {
        const QString h = SubtitleHash::ofFile(localPath);
        if (!h.isEmpty())
        {
            QUrlQuery q; q.addQueryItem(QStringLiteral("moviehash"), h);
            q.addQueryItem(QStringLiteral("languages"), lang);
            out << q.toString(QUrl::FullyEncoded);
        }
    }
    // "tt123"          -> a movie:   imdb_id=123
    // "ttShow:s:e"     -> an episode: parent_imdb_id=Show, season_number=s, episode_number=e
    if (!imdbStreamId.isEmpty())
    {
        const QStringList parts = imdbStreamId.split(QLatin1Char(':'));
        const QString num = QString(parts.value(0)).remove(QStringLiteral("tt"));
        QUrlQuery q;
        if (parts.size() >= 3)
        {
            q.addQueryItem(QStringLiteral("parent_imdb_id"), num);
            q.addQueryItem(QStringLiteral("season_number"), parts.value(1));
            q.addQueryItem(QStringLiteral("episode_number"), parts.value(2));
        }
        else if (!num.isEmpty()) q.addQueryItem(QStringLiteral("imdb_id"), num);
        if (!q.isEmpty()) { q.addQueryItem(QStringLiteral("languages"), lang); out << q.toString(QUrl::FullyEncoded); }
    }
    if (!title.trimmed().isEmpty())
    {
        QUrlQuery q; q.addQueryItem(QStringLiteral("query"), title.trimmed());
        q.addQueryItem(QStringLiteral("languages"), lang);
        out << q.toString(QUrl::FullyEncoded);
    }
    return out;
}

QString SubtitleFetcher::cacheIdentifier(const QString& imdbStreamId, const QString& title,
                                         const QString& localPath)
{
    if (!localPath.isEmpty())
    {
        const QString h = SubtitleHash::ofFile(localPath);
        if (!h.isEmpty()) return QStringLiteral("hash:") + h;
    }
    if (!imdbStreamId.isEmpty()) return imdbStreamId;
    return QStringLiteral("title:") + title.trimmed();
}

void SubtitleFetcher::fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
                            const QString& localPath, std::function<void(const QString&)> cb)
{
    if (!configured()) { cb(QString()); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    const QStringList queries = buildQueries(imdbStreamId, title, lang, localPath);
    if (queries.isEmpty()) { cb(QString()); return; }

    auto walk = std::make_shared<QStringList>(queries);
    ensureLogin([this, walk, lang, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        stepFetch(walk, 0, lang, cb);   // walk the tiers in precision order; the first file id wins
    });
}

// Try tier `i`; on a miss, recurse to `i+1`. Deliberately a NAMED member rather than a self-referencing
// std::function: a lambda captured inside the shared_ptr that owns it forms a reference cycle and leaks the
// function object plus its captures on every call. Here the shared_ptr holds only DATA.
void SubtitleFetcher::stepFetch(std::shared_ptr<QStringList> queries, int i, const QString& lang,
                                std::function<void(const QString&)> cb)
{
    if (i >= queries->size()) { emit log(QStringLiteral("subs: no match")); cb(QString()); return; }
    searchQuery(queries->at(i), lang, [this, queries, i, lang, cb](qint64 fileId) {
        if (fileId > 0) { download(fileId, lang, cb); return; }
        stepFetch(queries, i + 1, lang, cb);
    });
}

// Streaming callers (no bytes on disk) keep the old 4-arg shape: the chain simply starts at the IMDB tier.
void SubtitleFetcher::fetch(const QString& imdbStreamId, const QString& title, const QString& langCode,
                            std::function<void(const QString&)> cb)
{
    fetch(imdbStreamId, title, langCode, QString(), std::move(cb));
}

void SubtitleFetcher::searchList(const QString& imdbStreamId, const QString& title, const QString& langCode,
                                 const QString& localPath,
                                 std::function<void(const QVector<SubtitleCandidate>&)> cb)
{
    if (!configured()) { cb({}); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    const QStringList queries = buildQueries(imdbStreamId, title, lang, localPath);
    if (queries.isEmpty()) { cb({}); return; }
    auto walk = std::make_shared<QStringList>(queries);
    ensureLogin([this, walk, cb](bool ok) {
        if (!ok) { cb({}); return; }
        // Same precision order as fetch(); the first tier with ANY row is the one the user picks from.
        stepSearch(walk, 0, cb);
    });
}

// The searchList counterpart of stepFetch — same no-self-owning-function rule (see stepFetch's comment).
void SubtitleFetcher::stepSearch(std::shared_ptr<QStringList> queries, int i,
                                 std::function<void(const QVector<SubtitleCandidate>&)> cb)
{
    if (i >= queries->size()) { cb({}); return; }
    searchCandidates(queries->at(i), [this, queries, i, cb](const QVector<SubtitleCandidate>& list) {
        if (!list.isEmpty()) { cb(list); return; }
        stepSearch(queries, i + 1, cb);
    });
}

void SubtitleFetcher::downloadChoice(qint64 fileId, const QString& langCode,
                                     std::function<void(const QString&)> cb)
{
    if (!configured() || fileId <= 0) { cb(QString()); return; }
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    const QString lang = apiLang(langCode);
    ensureLogin([this, fileId, lang, cb](bool ok) {
        if (!ok) { cb(QString()); return; }
        download(fileId, lang, cb);
    });
}

void SubtitleFetcher::ensureLogin(std::function<void(bool)> done)
{
    if (!token_.isEmpty()) { done(true); return; }
    QNetworkRequest rq = makeRequest(QString::fromLatin1(kDefaultHost), QStringLiteral("/login"), false, {});
    rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body{ { QStringLiteral("username"), Settings::openSubUsername() },
                      { QStringLiteral("password"), Settings::openSubPassword() } };
    QNetworkReply* reply = nam_->post(rq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit log(QStringLiteral("subs: login failed (%1)").arg(reply->errorString()));
            done(false);
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        token_ = o.value(QStringLiteral("token")).toString();
        // /login returns the host to use for subsequent calls (e.g. a VIP host); default to the standard one.
        const QString base = o.value(QStringLiteral("base_url")).toString();
        if (!base.isEmpty()) apiHost_ = QString(base).remove(QStringLiteral("https://")).remove(QStringLiteral("/"));
        if (token_.isEmpty()) { emit log(QStringLiteral("subs: login returned no token")); done(false); return; }
        emit log(QStringLiteral("subs: logged in to OpenSubtitles"));
        done(true);
    });
}

void SubtitleFetcher::searchQuery(const QString& query, const QString& lang,
                                  std::function<void(qint64)> done)
{
    QNetworkRequest rq = makeRequest(apiHost_, QStringLiteral("/subtitles?") + query, true, token_);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, lang, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit log(QStringLiteral("subs: search failed (%1)").arg(reply->errorString()));
            done(0);
            return;
        }
        const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("data")).toArray();
        // Prefer an exact language match with the most downloads; fall back to the top result otherwise.
        qint64 bestId = 0; int bestDownloads = -1;
        qint64 anyId = 0;
        for (const QJsonValue& v : data)
        {
            const QJsonObject a = v.toObject().value(QStringLiteral("attributes")).toObject();
            const QJsonArray files = a.value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) continue;
            const qint64 fid = files.first().toObject().value(QStringLiteral("file_id")).toVariant().toLongLong();
            if (fid <= 0) continue;
            if (anyId == 0) anyId = fid;
            if (a.value(QStringLiteral("language")).toString().compare(lang, Qt::CaseInsensitive) != 0) continue;
            const int dl = a.value(QStringLiteral("download_count")).toInt();
            if (dl > bestDownloads) { bestDownloads = dl; bestId = fid; }
        }
        done(bestId != 0 ? bestId : anyId);
    });
}

// The picker's search: identical GET to searchQuery(), but every usable row is kept (most-downloaded first)
// so the user can override the auto-pick. Language filtering already happened server-side via languages=.
void SubtitleFetcher::searchCandidates(const QString& query,
                                       std::function<void(const QVector<SubtitleCandidate>&)> done)
{
    QNetworkRequest rq = makeRequest(apiHost_, QStringLiteral("/subtitles?") + query, true, token_);
    QNetworkReply* reply = nam_->get(rq);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit log(QStringLiteral("subs: search failed (%1)").arg(reply->errorString()));
            done({});
            return;
        }
        const QJsonArray data = QJsonDocument::fromJson(reply->readAll()).object()
                                    .value(QStringLiteral("data")).toArray();
        QVector<SubtitleCandidate> out;
        for (const QJsonValue& v : data)
        {
            const QJsonObject a = v.toObject().value(QStringLiteral("attributes")).toObject();
            const QJsonArray files = a.value(QStringLiteral("files")).toArray();
            if (files.isEmpty()) continue;
            SubtitleCandidate c;
            c.fileId = files.first().toObject().value(QStringLiteral("file_id")).toVariant().toLongLong();
            if (c.fileId <= 0) continue;
            c.language = a.value(QStringLiteral("language")).toString();
            c.release = a.value(QStringLiteral("release")).toString();
            if (c.release.isEmpty())
                c.release = files.first().toObject().value(QStringLiteral("file_name")).toString();
            c.downloads = a.value(QStringLiteral("download_count")).toInt();
            out.push_back(c);
        }
        std::sort(out.begin(), out.end(), [](const SubtitleCandidate& a, const SubtitleCandidate& b) {
            return a.downloads > b.downloads;                       // most-downloaded first
        });
        done(out);
    });
}

void SubtitleFetcher::download(qint64 fileId, const QString& lang,
                               std::function<void(const QString&)> done)
{
    QNetworkRequest rq = makeRequest(apiHost_, QStringLiteral("/download"), true, token_);
    rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body{ { QStringLiteral("file_id"), fileId },
                      { QStringLiteral("sub_format"), QStringLiteral("srt") } };
    QNetworkReply* reply = nam_->post(rq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId, lang, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            // A 401 means the token expired: drop it so the next fetch re-logs in.
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 401 || code == 403) token_.clear();
            emit log(QStringLiteral("subs: download request failed (%1)").arg(reply->errorString()));
            done(QString());
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString link = o.value(QStringLiteral("link")).toString();
        if (link.isEmpty())
        {
            const QString msg = o.value(QStringLiteral("message")).toString();
            emit log(QStringLiteral("subs: no download link%1").arg(msg.isEmpty() ? QString() : QStringLiteral(" — ") + msg));
            done(QString());
            return;
        }
        // Fetch the actual .srt from the (temporary) link and cache it by file id.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                            + QStringLiteral("/subs");
        QDir().mkpath(dir);
        const QString path = QStringLiteral("%1/%2-%3.srt").arg(dir, lang).arg(fileId);
        QNetworkRequest getReq{ QUrl(link) };
        getReq.setHeader(QNetworkRequest::UserAgentHeader,
                         QStringLiteral("MyMediaVault v%1").arg(QCoreApplication::applicationVersion()));
        getReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        getReq.setTransferTimeout(20000);
        QNetworkReply* dl = nam_->get(getReq);
        connect(dl, &QNetworkReply::finished, this, [this, dl, path, done] {
            dl->deleteLater();
            if (dl->error() != QNetworkReply::NoError)
            {
                emit log(QStringLiteral("subs: subtitle download failed (%1)").arg(dl->errorString()));
                done(QString());
                return;
            }
            const QByteArray bytes = dl->readAll();
            QFile f(path);
            if (bytes.isEmpty() || !f.open(QIODevice::WriteOnly)) { done(QString()); return; }
            f.write(bytes);
            f.close();
            emit log(QStringLiteral("subs: saved %1 (%2 bytes)").arg(QFileInfo(path).fileName()).arg(bytes.size()));
            done(path);
        });
    });
}
