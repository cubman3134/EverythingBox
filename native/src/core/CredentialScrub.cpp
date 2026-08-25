#include "CredentialScrub.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "StoredUrl.h"
#include "Tombstones.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

const int kSchema = 1;

// The identity the recents merge de-duplicates on: key, else path. Spelled here rather than reached for,
// because this pass runs over RAW json rows (some written by builds older than the current struct) and must
// agree with RecentStore::identOf and CloudMerge's ingest exactly.
QString identOf(const QJsonObject& o)
{
    const QString k = o.value(QStringLiteral("key")).toString();
    return k.isEmpty() ? o.value(QStringLiteral("path")).toString() : k;
}

// One recents list, cleaned. Returns false when nothing in it needed changing, so an install that has never
// played a stream is not rewritten at all (and the probe can tell "swept" from "ran").
bool scrubRecentList(const QString& iniKey, const QString& profile)
{
    const QString raw = store().value(iniKey).toString();
    if (raw.isEmpty()) return false;
    const QJsonArray arr = QJsonDocument::fromJson(raw.toUtf8()).array();
    if (arr.isEmpty()) return false;

    bool changed = false;
    QJsonArray out;
    QVector<QString> ids;   // identity per row already emitted, in order (the list is capped at 40)
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        QJsonObject o = v.toObject();
        const QString path  = o.value(QStringLiteral("path")).toString();
        const QString key   = o.value(QStringLiteral("key")).toString();
        const QString title = o.value(QStringLiteral("title")).toString();
        const QString thumb = o.value(QStringLiteral("thumb")).toString();
        const QString np = StoredUrl::location(path);
        const QString nk = StoredUrl::location(key);
        const QString nt = StoredUrl::title(title, np);
        const QString nb = StoredUrl::artwork(thumb);
        if (np != path || nk != key || nt != title || nb != thumb)
        {
            changed = true;
            o.insert(QStringLiteral("path"), np);
            if (nk.isEmpty()) o.remove(QStringLiteral("key")); else o.insert(QStringLiteral("key"), nk);
            if (nt.isEmpty()) o.remove(QStringLiteral("title")); else o.insert(QStringLiteral("title"), nt);
            if (nb.isEmpty()) o.remove(QStringLiteral("thumb")); else o.insert(QStringLiteral("thumb"), nb);
        }
        // A row is never DROPPED for being scrubbed — that would be data loss wearing a security fix's
        // clothes, and it cannot happen anyway: taking the query off a url always leaves a non-empty
        // scheme+host+path. The one case that needs a decision is two KEYLESS rows whose urls differed only
        // in their queries: they are now one item by the store's own identity rule, so they are collapsed
        // here, newest kept — leaving both would put a duplicate in front of the user that no later add()
        // could ever heal (nothing re-opens a row it cannot tell apart from its twin).
        const QString id = identOf(o);
        if (id.isEmpty()) { changed = true; continue; }   // no identity at all: unreachable by every reader
        const int prior = ids.indexOf(id);
        if (prior < 0) { ids.push_back(id); out.append(o); continue; }
        changed = true;
        const qint64 mine  = qint64(o.value(QStringLiteral("ts")).toDouble());
        const qint64 there = qint64(out.at(prior).toObject().value(QStringLiteral("ts")).toDouble());
        if (mine > there) out.replace(prior, o);          // newest wins; the list stays newest-first anyway
    }
    if (!changed) return false;
    store().setValue(iniKey, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));

    // The tombstones for this profile hold the SAME identities (issue #150 keys them key-else-path), so a
    // keyless removed row filed its tombstone under the tokenised url — a credential in deleted/*, which is
    // a per-item store and syncs like the rest. Re-file it under the scrubbed identity: that both removes
    // the value and keeps the tombstone pointing at the entry the merge is holding, which is the whole
    // reason it exists. record(ts) never downgrades, so a re-file cannot make a removal look newer.
    const QString tombStore = QStringLiteral("recent/") + profile;
    for (const Tombstones::Entry& e : Tombstones::all(tombStore))
    {
        const QString clean = StoredUrl::location(e.key);
        if (clean == e.key) continue;
        Tombstones::record(tombStore, clean, e.ts);
        Tombstones::remove(tombStore, e.key);
    }
    return true;
}

} // namespace

const char* CredentialScrub::stampKey() { return "device/migrations/storedCredentials"; }

bool CredentialScrub::run()
{
    if (store().value(QLatin1String(stampKey())).toInt() >= kSchema) return false;

    bool changed = false;
    // ONE allKeys() pass. The three families are told apart by shape rather than by walking groups, because
    // the profile and device segments are variable and a group walk would need the same string surgery with
    // more places to get it wrong.
    const QStringList keys = store().allKeys();
    for (const QString& k : keys)
    {
        if (k.startsWith(QStringLiteral("recent/")) && k.endsWith(QStringLiteral("/items")))
        {
            const QString profile = k.mid(7, k.size() - 7 - 6);
            if (!profile.isEmpty() && !profile.contains(QLatin1Char('/')))
                changed = scrubRecentList(k, profile) || changed;
        }
        else if (k.startsWith(QStringLiteral("resume/")) && k.endsWith(QStringLiteral("/title")))
        {
            // The live finding: a resume title of the shape "<uuid>?token=<…>", left by the
            // completeBaseName() idiom #193 replaced. Not a url — no scheme — so only label() sees it.
            const QString v = store().value(k).toString();
            const QString nv = StoredUrl::label(v);
            if (nv != v) { store().setValue(k, nv); changed = true; }
        }
        else if (k.startsWith(QStringLiteral("stats/")) && k.contains(QStringLiteral("/items/")))
        {
            // The same label again, inside the consumption-stats blob. Only "title" is touched: the counters
            // are the user's own data and the key is already hashed.
            const QString v = store().value(k).toString();
            QJsonObject o = QJsonDocument::fromJson(v.toUtf8()).object();
            if (o.isEmpty()) continue;
            const QString t = o.value(QStringLiteral("title")).toString();
            const QString nt = StoredUrl::label(t);
            if (nt == t) continue;
            o.insert(QStringLiteral("title"), nt);
            store().setValue(k, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
            changed = true;
        }
    }
    store().sync();
    // A failed write must NOT stamp (PlaylistStore::migrateToCategories' rule). Stamping a lost sweep would
    // mark this install clean while the token is still on disk, and no later run would retry.
    if (store().status() != QSettings::NoError) return changed;
    store().setValue(QLatin1String(stampKey()), kSchema);
    store().sync();
    return changed;
}
