#include "SteamLibrary.h"

#include <QSettings>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QPointer>
#include <QDateTime>
#include <QtGlobal>
#include <algorithm>

// Locate the Steam install directory (cached for the process).
static QString steamPath()
{
    static QString cached;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    QString p = reg.value(QStringLiteral("SteamPath")).toString();
    if (p.isEmpty())
    {
        QSettings reg2(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"), QSettings::NativeFormat);
        p = reg2.value(QStringLiteral("InstallPath")).toString();
    }
    cached = QDir::fromNativeSeparators(p);
#elif defined(Q_OS_MAC)
    cached = QDir::homePath() + QStringLiteral("/Library/Application Support/Steam");
#else
    for (const QString& c : { QDir::homePath() + QStringLiteral("/.steam/steam"),
                              QDir::homePath() + QStringLiteral("/.local/share/Steam"),
                              QDir::homePath() + QStringLiteral("/.steam/root") })
        if (QDir(c).exists()) { cached = c; break; }
#endif
    if (!cached.isEmpty() && !QDir(cached).exists()) cached.clear();
    return cached;
}

bool SteamLibrary::isAvailable() { return !steamPath().isEmpty(); }

// Every Steam library root (the main install plus any extra drives added in Steam), from libraryfolders.vdf.
static QStringList libraryRoots()
{
    QStringList roots;
    const QString steam = steamPath();
    if (steam.isEmpty()) return roots;
    roots << steam; // the primary library lives in the Steam dir itself

    QFile f(steam + QStringLiteral("/steamapps/libraryfolders.vdf"));
    if (f.open(QIODevice::ReadOnly))
    {
        const QString text = QString::fromUtf8(f.readAll());
        QRegularExpression re(QStringLiteral("\"path\"\\s*\"([^\"]*)\""));
        auto it = re.globalMatch(text);
        while (it.hasNext())
        {
            QString p = it.next().captured(1);
            p.replace(QStringLiteral("\\\\"), QStringLiteral("/"));
            p.replace(QLatin1Char('\\'), QLatin1Char('/')); // vdf escapes backslashes
            if (!p.isEmpty()) roots << QDir::cleanPath(p);
        }
    }
    roots.removeDuplicates();
    return roots;
}

// Steam's own tools / redistributables show up as "apps" - keep them out of the games list.
static bool isHiddenTool(const QString& name)
{
    static const char* junk[] = { "Steamworks Common Redistributables", "Steamworks SDK Redist",
                                  "Steam Linux Runtime", "Proton", "SteamVR", "Steam Controller" };
    for (const char* j : junk)
        if (name.contains(QLatin1String(j), Qt::CaseInsensitive)) return true;
    return false;
}

QVector<SteamGame> SteamLibrary::installedGames()
{
    QHash<QString, QString> byId; // appid -> name (dedupes a game that appears in two libraries)
    const QRegularExpression reId(QStringLiteral("\"appid\"\\s*\"(\\d+)\""),
                                  QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression reName(QStringLiteral("\"name\"\\s*\"([^\"]*)\""));

    for (const QString& root : libraryRoots())
    {
        QDir apps(root + QStringLiteral("/steamapps"));
        const QStringList manifests = apps.entryList({ QStringLiteral("appmanifest_*.acf") }, QDir::Files);
        for (const QString& m : manifests)
        {
            QFile f(apps.filePath(m));
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QString text = QString::fromUtf8(f.readAll());
            const auto idm = reId.match(text);
            if (!idm.hasMatch()) continue;
            const auto nm = reName.match(text);
            const QString name = nm.hasMatch() ? nm.captured(1) : idm.captured(1);
            if (isHiddenTool(name)) continue;
            byId.insert(idm.captured(1), name);
        }
    }

    QVector<SteamGame> out;
    out.reserve(byId.size());
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it)
        out.push_back({ it.key(), it.value() });
    std::sort(out.begin(), out.end(), [](const SteamGame& a, const SteamGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString SteamLibrary::posterUrl(const QString& appid)
{
    const QString steam = steamPath();
    if (!steam.isEmpty())
    {
        // Steam pre-downloads the vertical capsule into its cache; prefer it (offline + reliable).
        const QStringList locals = {
            steam + QStringLiteral("/appcache/librarycache/") + appid + QStringLiteral("_library_600x900.jpg"),
            steam + QStringLiteral("/appcache/librarycache/") + appid + QStringLiteral("/library_600x900.jpg"),
        };
        for (const QString& p : locals)
            if (QFile::exists(p)) return p;
    }
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/") + appid
           + QStringLiteral("/library_600x900.jpg");
}

QString SteamLibrary::launchUrl(const QString& appid)
{
    return QStringLiteral("steam://rungameid/") + appid;
}

QString SteamLibrary::installUrl(const QString& appid)
{
    // Hands an owned-but-not-installed game to the Steam client, which shows its own install UI.
    return QStringLiteral("steam://install/") + appid;
}

QVector<SteamGame> SteamLibrary::parseOwnedGames(const QByteArray& json)
{
    QVector<SteamGame> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    const QJsonArray games = doc.object().value(QStringLiteral("response")).toObject()
                                 .value(QStringLiteral("games")).toArray();
    for (const QJsonValue& v : games)
    {
        if (!v.isObject()) continue;
        const QJsonObject g = v.toObject();
        // appid arrives as a JSON number; tolerate a string form too. Skip an entry with no id.
        const QJsonValue av = g.value(QStringLiteral("appid"));
        QString appid = av.isDouble() ? QString::number((qint64)av.toDouble()) : av.toString();
        if (appid.isEmpty()) continue;
        const QString name = g.value(QStringLiteral("name")).toString();
        out.push_back({ appid, name.isEmpty() ? appid : name });
    }
    std::sort(out.begin(), out.end(), [](const SteamGame& a, const SteamGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

bool SteamLibrary::ownedCacheFresh(qint64 cachedTs, qint64 now, int ttlSecs)
{
    // A zero (never-cached) or future timestamp is not fresh; otherwise fresh while inside the TTL window.
    return cachedTs > 0 && now >= cachedTs && (now - cachedTs) < ttlSecs;
}

// The one shared owned-list cache (GUI thread only): the instant read and the async store both key off it.
namespace {
static const int kOwnedTtlSecs = 30 * 60; // owned-list TTL (the catalogCache precedent)
struct OwnedCache { QString key, id; qint64 ts = 0; QVector<SteamGame> games; };
OwnedCache& ownedCache() { static OwnedCache c; return c; }
}

SteamLibrary::OwnedFetch SteamLibrary::ownedFetchDecision(
    const QString& cacheKey, const QString& cacheId, qint64 cacheTs,
    const QString& reqKey, const QString& reqId, qint64 now, int ttlSecs)
{
    if (reqKey.isEmpty() || reqId.isEmpty()) return OwnedFetch::NotConfigured;
    if (cacheKey == reqKey && cacheId == reqId && ownedCacheFresh(cacheTs, now, ttlSecs))
        return OwnedFetch::CacheHit; // same key+id, still fresh -> nothing new to fetch
    return OwnedFetch::Fetch;
}

QVector<SteamGame> SteamLibrary::ownedGamesCached(const QString& apiKey, const QString& steamId)
{
    const QString key = apiKey.trimmed();
    const QString id  = steamId.trimmed();
    const OwnedCache& c = ownedCache();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // Reuse the pure decision: a fresh hit for the same key+id returns the cached list; anything else -> {}.
    if (ownedFetchDecision(c.key, c.id, c.ts, key, id, now, kOwnedTtlSecs) == OwnedFetch::CacheHit)
        return c.games;
    return {};
}

void SteamLibrary::ownedGamesFetch(const QString& apiKey, const QString& steamId, QObject* context,
                                   std::function<void(const QVector<SteamGame>&)> onReady)
{
    const QString key = apiKey.trimmed();
    const QString id  = steamId.trimmed();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const OwnedCache& c = ownedCache();
    // Not configured -> no network, no callback. Already fresh -> the caller's cached read already has it; no
    // callback (this is also what breaks the re-present loop: a callback re-runs the console builder, which
    // calls back into fetch, which now sees a fresh cache and stops).
    if (ownedFetchDecision(c.key, c.id, c.ts, key, id, now, kOwnedTtlSecs) != OwnedFetch::Fetch)
        return;

    // QNAM is parented to `context` so its lifetime is bound to the caller: if the caller dies, the manager
    // (and its pending reply) go with it and the finished lambda never runs.
    QNetworkAccessManager* nam = new QNetworkAccessManager(context);
    QUrl u(QStringLiteral("https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("key"), key);
    q.addQueryItem(QStringLiteral("steamid"), id);
    q.addQueryItem(QStringLiteral("include_appinfo"), QStringLiteral("1"));      // names, not just appids
    q.addQueryItem(QStringLiteral("include_played_free_games"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    u.setQuery(q);
    QNetworkRequest req(u);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam->get(req);

    // Async 8s bound: a stalled reply is aborted, which fires finished(OperationCanceledError) -> the single
    // completion path below treats it as a (silent, cached) failure. No event loop, no GUI-thread block.
    QTimer* timer = new QTimer(nam);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply] {
        if (!reply->isFinished()) reply->abort();
    });
    timer->start(8000);

    QPointer<QObject> ctx(context);
    QObject::connect(reply, &QNetworkReply::finished, nam, [=]() {
        timer->stop();
        const bool ok = reply->error() == QNetworkReply::NoError;
        QVector<SteamGame> games = ok ? parseOwnedGames(reply->readAll()) : QVector<SteamGame>{};

        // Outcome-only logging: NEVER log the key or the request URL (it embeds the key).
        if (!ok || games.isEmpty())
            qWarning("SteamLibrary::ownedGamesFetch: no owned games (offline/invalid/empty) - installed-only");

        // Cache even an empty result (a silent, TTL-bounded fallback): don't hammer the API while a key is
        // bad/offline. Stored on the shared cache the instant read consults.
        OwnedCache& cw = ownedCache();
        cw.key = key; cw.id = id; cw.ts = QDateTime::currentSecsSinceEpoch(); cw.games = games;

        if (ctx && onReady) onReady(games);   // still-alive guard; the caller further guards staleness
        nam->deleteLater();                    // reply + timer are children of nam -> torn down with it
    });
}
