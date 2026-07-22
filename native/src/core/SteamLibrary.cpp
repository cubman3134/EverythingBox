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
#include <QEventLoop>
#include <QTimer>
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

QVector<SteamGame> SteamLibrary::ownedGames(const QString& apiKey, const QString& steamId)
{
    static const int kTtlSecs = 30 * 60; // owned-list TTL (the catalogCache precedent)
    struct Cache { QString key, id; qint64 ts = 0; QVector<SteamGame> games; };
    static Cache cache;

    const QString key = apiKey.trimmed();
    const QString id  = steamId.trimmed();
    if (key.isEmpty() || id.isEmpty()) return {}; // not configured -> installed-only, no network at all

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (cache.key == key && cache.id == id && ownedCacheFresh(cache.ts, now, kTtlSecs))
        return cache.games; // fresh TTL hit -> no network

    QNetworkAccessManager nam;
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
    QNetworkReply* reply = nam.get(req);

    // Synchronous, TTL-guarded (so it hits the network at most ~once per 30 min): a bounded event loop with a
    // timeout so a stalled request can never wedge the caller.
    QEventLoop loop;
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(8000);
    loop.exec();

    QVector<SteamGame> games;
    const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError;
    if (ok) games = parseOwnedGames(reply->readAll());
    if (!reply->isFinished()) reply->abort();
    reply->deleteLater();

    // Outcome-only logging: NEVER log the key or the request URL (it embeds the key).
    if (!ok || games.isEmpty())
        qWarning("SteamLibrary::ownedGames: no owned games (offline/invalid/empty) - installed-only");

    // Cache even an empty result (a silent, TTL-bounded fallback): don't hammer the API while a key is bad/offline.
    cache.key = key; cache.id = id; cache.ts = now; cache.games = games;
    return games;
}
