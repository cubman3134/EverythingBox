#include "PlayStats.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QObject>
#include <QDateTime>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}

// Play history is per-profile (each user's playtime is their own), keyed by a hash of the game identity so
// the (base64/punctuation-laden) id can't create bogus QSettings subgroups.
static QString keyFor(const QString& identity)
{
    const QString profile = ProfileStore::currentId();
    const QByteArray h = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("playstats/") + (profile.isEmpty() ? QStringLiteral("default") : profile)
           + QStringLiteral("/") + QString::fromLatin1(h);
}

QString PlayStats::identity(const QString& key, const QString& path)
{
    return key.isEmpty() ? path : key;
}

PlayStats::Stat PlayStats::get(const QString& identity)
{
    Stat st;
    if (identity.isEmpty()) return st;
    const QString k = keyFor(identity);
    st.lastPlayed   = store().value(k + QStringLiteral("/last"), 0).toLongLong();
    st.totalSeconds = store().value(k + QStringLiteral("/total"), 0).toLongLong();
    st.sessions     = store().value(k + QStringLiteral("/sessions"), 0).toInt();
    return st;
}

void PlayStats::markPlayed(const QString& identity)
{
    if (identity.isEmpty()) return;
    store().setValue(keyFor(identity) + QStringLiteral("/last"), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

void PlayStats::addSession(const QString& identity, qint64 seconds)
{
    if (identity.isEmpty() || seconds <= 0) return;
    const QString k = keyFor(identity);
    const Stat st = get(identity);
    store().setValue(k + QStringLiteral("/total"), st.totalSeconds + seconds);
    store().setValue(k + QStringLiteral("/sessions"), st.sessions + 1);
    store().setValue(k + QStringLiteral("/last"), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

QString PlayStats::formatLastPlayed(qint64 epochSecs)
{
    if (epochSecs <= 0) return QString();
    const QDateTime when = QDateTime::fromSecsSinceEpoch(epochSecs);
    const QDate today = QDate::currentDate();
    const qint64 days = when.date().daysTo(today);
    if (days <= 0)  return QObject::tr("Today");
    if (days == 1)  return QObject::tr("Yesterday");
    if (days < 7)   return QObject::tr("%n day(s) ago", "", int(days));
    if (days < 30)  return QObject::tr("%n week(s) ago", "", int(days / 7));
    if (days < 365) return QObject::tr("%n month(s) ago", "", int(days / 30));
    // Older than a year: show the actual date rather than a vague count.
    return when.date().toString(QStringLiteral("MMM d, yyyy"));
}

QString PlayStats::formatDuration(qint64 seconds)
{
    if (seconds <= 0) return QString();
    if (seconds < 60) return QObject::tr("under a minute");
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    if (h <= 0) return QObject::tr("%1m").arg(m);
    if (m <= 0) return QObject::tr("%1h").arg(h);
    return QObject::tr("%1h %2m").arg(h).arg(m);
}
