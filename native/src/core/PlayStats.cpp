#include "PlayStats.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Settings.h"           // deviceId() — the per-device namespace (mdsync T3)

#include <QSettings>
#include <QCryptographicHash>
#include <QObject>
#include <QDateTime>
#include <algorithm>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
    return s;
}

// Post-upgrade schema: 1 = device-namespaced (playstats/<profile>/<deviceId>/<hash>/...).
static constexpr int kPlaySchema = 1;

static QString resolvedProfileId()
{
    const QString p = ProfileStore::currentId();
    return p.isEmpty() ? QStringLiteral("default") : p;
}

static QString profileRoot()  { return QStringLiteral("playstats/") + resolvedProfileId(); }
static QString sha1Of(const QString& identity)
{
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// THIS device's slot for a game: "playstats/<profile>/<deviceId>/<sha1>". Writers target this; readers sum
// across every device namespace.
static QString deviceGameKey(const QString& identity)
{
    return profileRoot() + QLatin1Char('/') + Settings::deviceId() + QLatin1Char('/') + sha1Of(identity);
}

static QString playSchemaKey(const QString& profile)
{
    return QStringLiteral("playstats/") + profile + QStringLiteral("/schema");
}

// Fold one profile's legacy un-namespaced games (playstats/<p>/<sha1>/{last,total,sessions}) into this
// device's namespace, once (guarded by the per-profile schema stamp — the PlaylistStore precedent). A legacy
// game subgroup carries the leaf keys directly; a device namespace carries game SUBGROUPS, so we only move
// groups that have a direct "total"/"last"/"sessions" leaf (never a device namespace). Move-then-remove is
// exact at first migration (the device namespace is empty; every writer folds before it writes) and a second
// call short-circuits on the stamp.
static void migratePlayProfile(const QString& profile)
{
    QSettings& s = store();
    if (s.value(playSchemaKey(profile)).toInt() >= kPlaySchema) return;

    const QString dev  = Settings::deviceId();
    const QString base = QStringLiteral("playstats/") + profile;

    s.beginGroup(base);
    const QStringList groups = s.childGroups();
    s.endGroup();

    for (const QString& g : groups)
    {
        if (g == dev) continue; // already this device's namespace
        const QString gk = base + QLatin1Char('/') + g;
        const bool isLegacyGame = s.contains(gk + QStringLiteral("/total"))
                               || s.contains(gk + QStringLiteral("/last"))
                               || s.contains(gk + QStringLiteral("/sessions"));
        if (!isLegacyGame) continue; // a nested device namespace — leave it
        const QString dst = base + QLatin1Char('/') + dev + QLatin1Char('/') + g;
        for (const char* leaf : {"/total", "/last", "/sessions"})
        {
            const QString lk = gk + QLatin1String(leaf);
            if (s.contains(lk)) s.setValue(dst + QLatin1String(leaf), s.value(lk));
        }
        s.remove(gk); // legacy game folded
    }

    if (s.status() == QSettings::NoError) { s.setValue(playSchemaKey(profile), kPlaySchema); s.sync(); }
}

QString PlayStats::identity(const QString& key, const QString& path)
{
    return key.isEmpty() ? path : key;
}

PlayStats::Stat PlayStats::get(const QString& identity)
{
    Stat st;
    if (identity.isEmpty()) return st;
    migratePlayProfile(resolvedProfileId());

    // Sum total/sessions across every device namespace for this game; last-played is the MAX.
    const QString root = profileRoot();
    const QString sha = sha1Of(identity);
    QSettings& s = store();
    s.beginGroup(root);
    const QStringList devices = s.childGroups();
    s.endGroup();
    for (const QString& dev : devices)
    {
        const QString gk = root + QLatin1Char('/') + dev + QLatin1Char('/') + sha;
        st.totalSeconds += s.value(gk + QStringLiteral("/total"), 0).toLongLong();
        st.sessions     += s.value(gk + QStringLiteral("/sessions"), 0).toInt();
        st.lastPlayed    = std::max(st.lastPlayed, s.value(gk + QStringLiteral("/last"), 0).toLongLong());
    }
    return st;
}

void PlayStats::markPlayed(const QString& identity)
{
    if (identity.isEmpty()) return;
    migratePlayProfile(resolvedProfileId());
    store().setValue(deviceGameKey(identity) + QStringLiteral("/last"), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

void PlayStats::addSession(const QString& identity, qint64 seconds)
{
    if (identity.isEmpty() || seconds <= 0) return;
    migratePlayProfile(resolvedProfileId());
    // Read-modify-write THIS device's slot only (not the cross-device aggregate get() returns).
    const QString k = deviceGameKey(identity);
    const qint64 curTotal    = store().value(k + QStringLiteral("/total"), 0).toLongLong();
    const int    curSessions = store().value(k + QStringLiteral("/sessions"), 0).toInt();
    store().setValue(k + QStringLiteral("/total"), curTotal + seconds);
    store().setValue(k + QStringLiteral("/sessions"), curSessions + 1);
    store().setValue(k + QStringLiteral("/last"), QDateTime::currentSecsSinceEpoch());
    store().sync();
}

qint64 PlayStats::profileTotalSeconds()
{
    migratePlayProfile(resolvedProfileId());
    // Each device namespace holds game subgroups carrying /total; sum every game's /total across all devices.
    const QString root = profileRoot();
    QSettings& s = store();
    s.beginGroup(root);
    const QStringList devices = s.childGroups();
    qint64 total = 0;
    for (const QString& dev : devices)
    {
        s.beginGroup(dev);
        for (const QString& g : s.childGroups())
            total += s.value(g + QStringLiteral("/total"), 0).toLongLong();
        s.endGroup();
    }
    s.endGroup();
    return total;
}

void PlayStats::migrate()
{
    // Fold EVERY profile's legacy games into this device's namespace (guarded per profile). The child groups
    // of "playstats" are the profile ids. Run once at startup before any CloudMerge serialize.
    QSettings& s = store();
    s.beginGroup(QStringLiteral("playstats"));
    const QStringList profiles = s.childGroups();
    s.endGroup();
    for (const QString& p : profiles) migratePlayProfile(p);
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
