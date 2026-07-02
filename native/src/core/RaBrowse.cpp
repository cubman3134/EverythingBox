#include "RaBrowse.h"
#include "AppPaths.h"

#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QMap>
#include <algorithm>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}
static QString raUser() { return store().value(QStringLiteral("ra/user")).toString(); }
static QString raKey()  { return store().value(QStringLiteral("ra/apikey")).toString(); }

bool RaBrowse::configured() { return !raUser().isEmpty() && !raKey().isEmpty(); }

RaBrowse::RaBrowse(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

// Lowercase, alphanumerics only, with region/format tags in ()/[] dropped - so "Super Mario Bros. (USA)"
// and RA's "Super Mario Bros." collapse to the same key.
static QString normTitle(const QString& s)
{
    QString t = s;
    t.replace(QRegularExpression(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]")), QStringLiteral(" "));
    QString o;
    for (const QChar c : t) if (c.isLetterOrNumber()) o += c.toLower();
    return o;
}

// Per-session cache of a console's games-with-achievements (normalized title -> RA game id).
struct RaGame { QString norm; int id = 0; };
static QMap<unsigned, QList<RaGame>>& gameListCache() { static QMap<unsigned, QList<RaGame>> c; return c; }

void RaBrowse::fetch(const QString& title, unsigned consoleId, std::function<void(QList<Ach>)> cb)
{
    if (!configured() || consoleId == 0 || title.trimmed().isEmpty()) { cb({}); return; }
    const QString want = normTitle(title);
    if (want.size() < 3) { cb({}); return; }

    auto progress = [this, want, cb](const QList<RaGame>& games) {
        int gameId = 0;
        for (const RaGame& g : games) if (g.norm == want) { gameId = g.id; break; }      // exact
        if (!gameId)                                                                       // containment
            for (const RaGame& g : games)
                if (g.norm.size() >= 4 && (g.norm.contains(want) || want.contains(g.norm))) { gameId = g.id; break; }
        if (!gameId) { cb({}); return; }

        QUrl u(QStringLiteral("https://retroachievements.org/API/API_GetGameInfoAndUserProgress.php"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("z"), raUser()); q.addQueryItem(QStringLiteral("y"), raKey());
        q.addQueryItem(QStringLiteral("u"), raUser()); q.addQueryItem(QStringLiteral("g"), QString::number(gameId));
        u.setQuery(q);
        QNetworkRequest rq(u); rq.setTransferTimeout(20000);
        QNetworkReply* rep = nam_->get(rq);
        connect(rep, &QNetworkReply::finished, this, [rep, cb] {
            rep->deleteLater();
            QList<Ach> out;
            if (rep->error() == QNetworkReply::NoError)
            {
                const QJsonObject achs = QJsonDocument::fromJson(rep->readAll()).object()
                                             .value(QStringLiteral("Achievements")).toObject();
                for (auto it = achs.begin(); it != achs.end(); ++it)
                {
                    const QJsonObject a = it.value().toObject();
                    Ach x;
                    x.title  = a.value(QStringLiteral("Title")).toString();
                    x.badge  = a.value(QStringLiteral("BadgeName")).toString();
                    x.earned = !a.value(QStringLiteral("DateEarned")).toString().isEmpty()
                            || !a.value(QStringLiteral("DateEarnedHardcore")).toString().isEmpty();
                    if (!x.badge.isEmpty()) out << x;
                }
            }
            std::stable_sort(out.begin(), out.end(),
                             [](const Ach& a, const Ach& b) { return a.earned && !b.earned; }); // earned first
            cb(out);
        });
    };

    if (gameListCache().contains(consoleId)) { progress(gameListCache()[consoleId]); return; }

    // First time for this console: pull its list of games that HAVE achievements (f=1) and cache it.
    QUrl u(QStringLiteral("https://retroachievements.org/API/API_GetGameList.php"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("z"), raUser()); q.addQueryItem(QStringLiteral("y"), raKey());
    q.addQueryItem(QStringLiteral("i"), QString::number(consoleId)); q.addQueryItem(QStringLiteral("f"), QStringLiteral("1"));
    u.setQuery(q);
    QNetworkRequest rq(u); rq.setTransferTimeout(30000);
    QNetworkReply* rep = nam_->get(rq);
    connect(rep, &QNetworkReply::finished, this, [rep, consoleId, progress] {
        rep->deleteLater();
        QList<RaGame> games;
        if (rep->error() == QNetworkReply::NoError)
        {
            const QJsonArray arr = QJsonDocument::fromJson(rep->readAll()).array();
            for (const QJsonValue& v : arr)
            {
                const QJsonObject g = v.toObject();
                RaGame e; e.id = g.value(QStringLiteral("ID")).toInt(); e.norm = normTitle(g.value(QStringLiteral("Title")).toString());
                if (e.id && !e.norm.isEmpty()) games << e;
            }
        }
        gameListCache()[consoleId] = games;
        progress(games);
    });
}
