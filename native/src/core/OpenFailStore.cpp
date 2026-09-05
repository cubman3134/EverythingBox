#include "OpenFailStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per profile, mirroring the recents/favourites namespacing exactly: one household member's dead link is not
// a mark on another's shelf, and the two of them may well be browsing different sources.
QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

// "openfail/<profile>/items". The PREFIX is the load-bearing part: CloudSync::isDeviceLocalKey matches on
// "openfail/", so renaming it here without renaming it there would silently start syncing this.
QString itemsKey()
{
    return QStringLiteral("openfail/") + profileId() + QStringLiteral("/items");
}

qint64 clock(qint64 now)
{
    return now > 0 ? now : QDateTime::currentSecsSinceEpoch();
}

QVector<OpenFailure> parseFrom(const QString& key)
{
    QVector<OpenFailure> out;
    const QByteArray json = store().value(key).toString().toUtf8();
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        OpenFailure f;
        f.id      = o.value(QStringLiteral("id")).toString();
        f.title   = o.value(QStringLiteral("title")).toString();
        f.message = o.value(QStringLiteral("msg")).toString();
        f.ts      = (qint64)o.value(QStringLiteral("ts")).toDouble();
        if (!f.id.isEmpty()) out.push_back(f);
    }
    return out;
}

// The hot copy. See OpenFailStore.h for why there is one: the browse model asks marked() once per row, and
// a console folder can hold nine hundred of them. Keyed by the profile-qualified ini key, so switching
// profile re-reads without anyone having to remember to invalidate.
struct Hot
{
    bool valid = false;
    QString key;
    QVector<OpenFailure> rows;
};

Hot& hot()
{
    static Hot h;
    return h;
}

const QVector<OpenFailure>& readAll()
{
    Hot& h = hot();
    const QString k = itemsKey();
    if (h.valid && h.key == k) return h.rows;
    h.rows = parseFrom(k);
    h.key = k;
    h.valid = true;
    return h.rows;
}

void writeAll(const QVector<OpenFailure>& rows)
{
    QJsonArray arr;
    for (const OpenFailure& f : rows)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), f.id);
        if (!f.title.isEmpty())   o.insert(QStringLiteral("title"), f.title);
        if (!f.message.isEmpty()) o.insert(QStringLiteral("msg"), f.message);
        o.insert(QStringLiteral("ts"), (double)f.ts);
        arr.append(o);
    }
    const QString k = itemsKey();
    store().setValue(k, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    // Adopt what was just written rather than merely dropping the cache: every writer here is immediately
    // followed by a read (the surfaces repaint), and a drop would send that read back to the file.
    Hot& h = hot();
    h.rows = rows;
    h.key = k;
    h.valid = true;
}

// Has this record stopped answering the question it exists to answer? A row with no timestamp at all counts
// as expired rather than as eternal: an undated record is the one shape that could never clear itself.
bool expired(const OpenFailure& f, qint64 nowSecs)
{
    return f.ts <= 0 || (nowSecs - f.ts) >= OpenFailStore::kExpirySecs;
}

} // namespace

void OpenFailStore::record(const QString& id, const QString& title, const QString& message, qint64 now)
{
    if (id.isEmpty()) return;   // see the header: no identity, no surface, and never a title-keyed row
    const qint64 t = clock(now);

    QVector<OpenFailure> rows = readAll();  // by value: about to be edited
    // The newest attempt replaces the older one for the same item, rather than accumulating: the page shows
    // "why it did not open just now", not a history.
    for (int i = rows.size() - 1; i >= 0; --i)
        if (rows[i].id == id) rows.remove(i);

    // Expired rows are dropped on the way past. This is the only write path, so the file cannot grow a tail
    // of week-old records that every reader ignores.
    for (int i = rows.size() - 1; i >= 0; --i)
        if (expired(rows[i], t)) rows.remove(i);

    OpenFailure f;
    f.id = id; f.title = title; f.message = message; f.ts = t;
    rows.prepend(f);                       // newest first
    while (rows.size() > kMaxEntries) rows.removeLast();
    writeAll(rows);
}

void OpenFailStore::clear(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<OpenFailure> rows = readAll();  // by value: about to be edited
    const int before = rows.size();
    for (int i = rows.size() - 1; i >= 0; --i)
        if (rows[i].id == id) rows.remove(i);
    if (rows.size() != before) writeAll(rows);
}

OpenFailure OpenFailStore::lookup(const QString& id, qint64 now)
{
    if (id.isEmpty()) return {};
    const qint64 t = clock(now);
    for (const OpenFailure& f : readAll())
        if (f.id == id) return expired(f, t) ? OpenFailure{} : f;
    return {};
}

bool OpenFailStore::marked(const QString& id, qint64 now)
{
    return !lookup(id, now).isNull();
}

QVector<OpenFailure> OpenFailStore::list(qint64 now)
{
    const qint64 t = clock(now);
    QVector<OpenFailure> out;
    for (const OpenFailure& f : readAll())
        if (!expired(f, t)) out.push_back(f);
    return out;
}

int OpenFailStore::purgeExpired(qint64 now)
{
    const qint64 t = clock(now);
    QVector<OpenFailure> rows = readAll();  // by value: about to be edited
    const int before = rows.size();
    for (int i = rows.size() - 1; i >= 0; --i)
        if (expired(rows[i], t)) rows.remove(i);
    const int dropped = before - rows.size();
    if (dropped) writeAll(rows);
    return dropped;
}

void OpenFailStore::invalidate()
{
    hot().valid = false;
}
