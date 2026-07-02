#include "SteamAchievements.h"
#include "AppPaths.h"

#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <algorithm>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}
static QString steamKey() { return store().value(QStringLiteral("steam/apikey")).toString(); }

bool SteamAchievements::configured() { return !steamKey().isEmpty(); }

SteamAchievements::SteamAchievements(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

static int readAppIdFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    bool ok = false;
    const int id = QString::fromLatin1(f.readAll()).trimmed().toInt(&ok);
    return (ok && id > 0) ? id : 0;
}

// The Steam appid a repack carries in steam_appid.txt (root or steam_settings/, else anywhere below the folder).
static int appIdFromFolder(const QString& dir)
{
    if (dir.isEmpty()) return 0;
    for (const QString& rel : { QStringLiteral("/steam_appid.txt"), QStringLiteral("/steam_settings/steam_appid.txt") })
        if (const int id = readAppIdFile(dir + rel)) return id;
    QDirIterator it(dir, QStringList{ QStringLiteral("steam_appid.txt") }, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? readAppIdFile(it.next()) : 0;
}

// Which achievement apinames the Steam emulator has recorded as unlocked. Goldberg/GSE write an
// achievements.json (in %APPDATA%\Goldberg SteamEmu Saves\<appid>\ or \GSE Saves\<appid>\, and some builds
// keep it beside the game) keyed by apiname -> { "earned": true, ... } (or a bare true).
QSet<QString> SteamAchievements::readUnlocked(int appid, const QString& gameDir) const
{
    QStringList files;
    const QString roaming = qEnvironmentVariable("APPDATA");
    if (!roaming.isEmpty())
    {
        files << roaming + QStringLiteral("/Goldberg SteamEmu Saves/") + QString::number(appid) + QStringLiteral("/achievements.json");
        files << roaming + QStringLiteral("/GSE Saves/") + QString::number(appid) + QStringLiteral("/achievements.json");
    }
    if (!gameDir.isEmpty()) // some emus save locally next to the game
    {
        QDirIterator it(gameDir, QStringList{ QStringLiteral("achievements.json") }, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
    }

    QSet<QString> out;
    for (const QString& path : files)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = o.begin(); it != o.end(); ++it)
        {
            const QJsonValue v = it.value();
            bool earned = false;
            if (v.isObject())      { const QJsonObject a = v.toObject();
                                     earned = a.value(QStringLiteral("earned")).toBool()
                                           || a.value(QStringLiteral("earned")).toInt() != 0
                                           || a.value(QStringLiteral("Achieved")).toInt() != 0; }
            else if (v.isBool())   earned = v.toBool();
            else if (v.isDouble()) earned = v.toInt() != 0;
            if (earned) out.insert(it.key());
        }
        if (!out.isEmpty()) break; // first file with real data wins
    }
    return out;
}

void SteamAchievements::fetch(const QString&, const QString& gameDir, std::function<void(QList<Ach>)> cb)
{
    if (!configured()) { cb({}); return; }
    const int appid = appIdFromFolder(gameDir);
    if (appid <= 0) { cb({}); return; } // no steam_appid.txt -> can't identify the game

    QUrl u(QStringLiteral("https://api.steampowered.com/ISteamUserStats/GetSchemaForGame/v2/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("key"), steamKey());
    q.addQueryItem(QStringLiteral("appid"), QString::number(appid));
    u.setQuery(q);
    QNetworkRequest rq(u); rq.setTransferTimeout(20000);
    QNetworkReply* rep = nam_->get(rq);
    connect(rep, &QNetworkReply::finished, this, [this, rep, appid, gameDir, cb] {
        rep->deleteLater();
        QList<Ach> out;
        if (rep->error() == QNetworkReply::NoError)
        {
            const QJsonArray achs = QJsonDocument::fromJson(rep->readAll()).object()
                                        .value(QStringLiteral("game")).toObject()
                                        .value(QStringLiteral("availableGameStats")).toObject()
                                        .value(QStringLiteral("achievements")).toArray();
            if (!achs.isEmpty())
            {
                const QSet<QString> unlocked = readUnlocked(appid, gameDir);
                for (const QJsonValue& v : achs)
                {
                    const QJsonObject a = v.toObject();
                    Ach x;
                    x.title  = a.value(QStringLiteral("displayName")).toString();
                    x.icon   = a.value(QStringLiteral("icon")).toString(); // full color-icon URL
                    x.earned = unlocked.contains(a.value(QStringLiteral("name")).toString());
                    if (x.title.isEmpty()) x.title = a.value(QStringLiteral("name")).toString();
                    if (!x.icon.isEmpty()) out << x;
                }
                std::stable_sort(out.begin(), out.end(),
                                 [](const Ach& a, const Ach& b) { return a.earned && !b.earned; }); // earned first
            }
        }
        cb(out);
    });
}
