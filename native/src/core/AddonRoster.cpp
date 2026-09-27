#include "AddonRoster.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Tombstones.h"

#include <QSettings>
#include <QSaveFile>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cstring>

// Shares the portable everythingbox.ini with AddonManager and every other store. Coherence comes from every
// writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

const QLatin1String kUrlsKey("addon.remote.urls");
const QLatin1String kEnabledPrefix("addon.enabled.");

std::function<void()> g_changeHook;
bool g_migrationsRun = false;
QString g_addonsRoot;   // empty = <data>/addons

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }

// The live URL list, normalised and de-duplicated, in its stored order.
QStringList liveUrls()
{
    QStringList out;
    const QJsonArray arr = QJsonDocument::fromJson(store().value(kUrlsKey).toByteArray()).array();
    for (const QJsonValue& v : arr)
    {
        const QString b = AddonRoster::normalizeBase(v.toString());
        if (!b.isEmpty() && !out.contains(b)) out << b;
    }
    return out;
}

void writeLiveUrls(const QStringList& urls)
{
    QJsonArray arr;
    for (const QString& u : urls) arr.append(u);
    // Byte-for-byte AddonManager's own spelling (a QByteArray of compact JSON), so the two writers agree.
    store().setValue(kUrlsKey, QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// Every live addon.enabled.<id>, id -> flag. allKeys rather than childKeys: an id carrying a '/' becomes a
// QSettings group path, and allKeys still names it in full.
QHash<QString, bool> liveFlags()
{
    QHash<QString, bool> out;
    for (const QString& k : store().allKeys())
        if (k.startsWith(kEnabledPrefix) && k.size() > kEnabledPrefix.size())
            out.insert(k.mid(kEnabledPrefix.size()), store().value(k).toBool());
    return out;
}

// The local tombstones, key -> newest ts.
QHash<QString, qint64> localTombs()
{
    QHash<QString, qint64> map;
    for (const Tombstones::Entry& e : Tombstones::all(AddonRoster::tombStore()))
        if (e.ts > map.value(e.key, 0)) map.insert(e.key, e.ts);
    return map;
}

// ---- registry sources (increment 4) ----
const QLatin1String kSourcePrefix("registrySource:");

QString listName(AddonRoster::RegistryList l)
{
    return l == AddonRoster::RegistryList::Addons ? QStringLiteral("addons") : QStringLiteral("themes");
}

// The per-list ini key each browser wrote before the roster held these, and an older peer still sends.
QString legacyListKey(AddonRoster::RegistryList l)
{
    return l == AddonRoster::RegistryList::Addons ? QStringLiteral("registry/addonsExtras")
                                                 : QStringLiteral("registry/themesExtras");
}

// "registrySource:<list>:<url>" -> list, url. The key is the authority for both.
bool parseSourceKey(const QString& key, QString* list, QString* url)
{
    if (!key.startsWith(kSourcePrefix)) return false;
    const QString rest = key.mid(kSourcePrefix.size());
    const int colon = rest.indexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    const QString l = rest.left(colon), u = rest.mid(colon + 1);
    if ((l != QLatin1String("addons") && l != QLatin1String("themes")) || u.isEmpty()) return false;
    if (list) *list = l;
    if (url) *url = u;
    return true;
}

bool suppressed(const AddonRoster::Record& r, const QHash<QString, qint64>& tombs)
{
    // The favourites/presets rule, verbatim: a REAL tombstone at-or-after the record's ts suppresses it. "No
    // tombstone" is an ABSENT key, never ts 0, so a ts-0 record is only ever swept by a real deletion.
    return tombs.contains(r.key) && tombs.value(r.key) >= r.ts;
}

int indexByUrl(const QVector<AddonRoster::Record>& recs, const QString& url)
{
    for (int i = 0; i < recs.size(); ++i)
        if (!recs[i].url.isEmpty() && recs[i].url == url) return i;
    return -1;
}

int indexByKey(const QVector<AddonRoster::Record>& recs, const QString& key)
{
    for (int i = 0; i < recs.size(); ++i)
        if (recs[i].key == key) return i;
    return -1;
}

// A key that IS a URL (the fallback for an add-on whose manifest was never cached) has no addon.enabled.<id>
// twin, because the live flag is keyed by manifest id.
bool keyIsUrl(const AddonRoster::Record& r) { return r.key.contains(QStringLiteral("://")); }

// The manifest ids of this device's SIDELOADED folder add-ons (issue #77, increment 2). Device-only: the roster
// neither records nor projects anything about them, their on/off flag included.
QSet<QString> sideloadedIds(const QVector<AddonRoster::LocalAddon>& locals)
{
    QSet<QString> out;
    for (const AddonRoster::LocalAddon& a : locals)
        if (a.sideloaded()) out.insert(a.id);
    return out;
}

} // namespace

QString AddonRoster::itemsKey()
{
    return QStringLiteral("roster/") + QLatin1String(kScope) + QStringLiteral("/items");
}

QString AddonRoster::tombStore()
{
    return QStringLiteral("roster/") + QLatin1String(kScope);
}

QString AddonRoster::normalizeBase(const QString& raw)
{
    QString b = raw.trimmed();
    if (b.endsWith(QStringLiteral("/manifest.json"))) b.chop(int(strlen("/manifest.json")));
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b;
}

QString AddonRoster::manifestCacheKey(const QString& base)
{
    const QByteArray h = QCryptographicHash::hash(base.toUtf8(), QCryptographicHash::Md5).toHex();
    return QStringLiteral("addon.remote.manifest.") + QString::fromUtf8(h);
}

QString AddonRoster::keyForUrl(const QString& base)
{
    // Both manifest dialects AddonManager accepts (Stremio's and our own) carry the add-on's id as a top-level
    // "id", and that is exactly what AddonManager::planRemoteAdd matches on.
    const QByteArray cached = store().value(manifestCacheKey(base)).toByteArray();
    const QString id = cached.isEmpty()
        ? QString() : QJsonDocument::fromJson(cached).object().value(QStringLiteral("id")).toString();
    return id.isEmpty() ? base : id;
}

bool AddonRoster::isLiveKey(const QString& key)
{
    return key == kUrlsKey || key.startsWith(kEnabledPrefix);
}

QJsonObject AddonRoster::toJson(const Record& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("key"), r.key);
    o.insert(QStringLiteral("url"), r.url);
    o.insert(QStringLiteral("enabled"), r.enabled);
    o.insert(QStringLiteral("ts"), static_cast<double>(r.ts));
    // Only a registry record carries these, so every other record serialises exactly as increment 1 wrote it.
    if (r.isRegistry())
    {
        o.insert(QStringLiteral("kind"), QStringLiteral("registry"));
        o.insert(QStringLiteral("registry"), r.registry);
        o.insert(QStringLiteral("entry"), r.entry);
        o.insert(QStringLiteral("version"), r.version);
    }
    // ...and only a registry source these. Informational: fromJson reads both back from the key.
    if (r.isRegistrySource())
    {
        o.insert(QStringLiteral("kind"), QStringLiteral("registrySource"));
        o.insert(QStringLiteral("list"), r.sourceList);
        o.insert(QStringLiteral("source"), r.sourceUrl);
    }
    return o;
}

// ---- the registries the user added (increment 4) -----------------------------------------------------------

QString AddonRoster::builtInRegistryUrl(RegistryList list)
{
    return list == RegistryList::Addons
        ? QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-addons/main/index.json")
        : QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json");
}

QString AddonRoster::normalizeRegistryUrl(const QString& raw)
{
    // Spelled out rather than through QUrl, which would re-encode a path and so change the string a provenance
    // file already compares against. Only the scheme and host are case-insensitive, so only they are folded —
    // and not when the authority carries user info, which may be case-sensitive.
    QString u = raw.trimmed();
    const int sep = u.indexOf(QStringLiteral("://"));
    if (sep <= 0) return u;
    int end = u.indexOf(QLatin1Char('/'), sep + 3);
    if (end < 0) end = u.size();
    const QString head = u.left(end);
    if (head.contains(QLatin1Char('@'))) return u;
    return head.toLower() + u.mid(end);
}

bool AddonRoster::isBuiltInRegistry(RegistryList list, const QString& url)
{
    const QString n = normalizeRegistryUrl(url);
    if (n.isEmpty()) return false;
    if (n == normalizeRegistryUrl(builtInRegistryUrl(list))) return true;
    if (list != RegistryList::Addons || !qEnvironmentVariableIsSet("EB_UITEST")) return false;
    const QString fixture = normalizeRegistryUrl(qEnvironmentVariable("EB_ADDON_REGISTRY_URL"));
    return !fixture.isEmpty() && n == fixture;
}

QString AddonRoster::registrySourceKey(RegistryList list, const QString& url)
{
    return QString(kSourcePrefix) + listName(list) + QLatin1Char(':') + normalizeRegistryUrl(url);
}

bool AddonRoster::isLegacyRegistryKey(const QString& iniKey)
{
    return iniKey == legacyListKey(RegistryList::Addons) || iniKey == legacyListKey(RegistryList::Themes);
}

bool AddonRoster::isBuiltInRegistryKey(const QString& key)
{
    QString list, url;
    if (!parseSourceKey(key, &list, &url)) return false;
    return isBuiltInRegistry(list == QLatin1String("addons") ? RegistryList::Addons : RegistryList::Themes, url);
}

QStringList AddonRoster::parseLegacyRegistryValue(const QString& value)
{
    // A settings bundle carries every value as a string. A one-entry list arrives as that URL; a JSON array or
    // an ini-style comma list is read too. A URL with a comma in it is not a registry index anyone publishes.
    QStringList out;
    const QString v = value.trimmed();
    if (v.isEmpty()) return out;
    QStringList parts;
    if (v.startsWith(QLatin1Char('[')))
        for (const QJsonValue& x : QJsonDocument::fromJson(v.toUtf8()).array()) parts << x.toString();
    else
        parts = v.split(QLatin1Char(','));
    for (const QString& p : parts)
        if (!p.trimmed().isEmpty() && !out.contains(p.trimmed())) out << p.trimmed();
    return out;
}

namespace {
// The adoption itself: ADDS at ts 0 — never over a tombstone, never a built-in, never a removal.
bool adoptSources(AddonRoster::RegistryList list, const QStringList& urls)
{
    QVector<AddonRoster::Record> recs = AddonRoster::records();
    const QHash<QString, qint64> tombs = localTombs();
    bool changed = false;
    for (const QString& raw : urls)
    {
        const QString u = AddonRoster::normalizeRegistryUrl(raw);
        if (u.isEmpty() || AddonRoster::isBuiltInRegistry(list, u)) continue;   // the built-in is never a record
        const QString key = AddonRoster::registrySourceKey(list, u);
        if (tombs.contains(key) || indexByKey(recs, key) >= 0) continue;   // removed here, or already held
        AddonRoster::Record r;
        r.key = key;
        r.sourceList = listName(list);
        r.sourceUrl = u;
        r.ts = 0;                                        // undated: any dated edit anywhere beats it
        recs.push_back(r);
        changed = true;
    }
    if (changed) AddonRoster::saveRecords(recs);
    return changed;
}

// This device's own pre-roster lists, adopted ONCE as ts-0 adds and then removed, so the roster is the one
// store. Run from reconcile(), after its backfill, so an upgrade still backfills the add-on roster at ts 0.
bool adoptLocalLegacyLists()
{
    bool changed = false;
    for (AddonRoster::RegistryList l : { AddonRoster::RegistryList::Addons, AddonRoster::RegistryList::Themes })
    {
        const QString k = legacyListKey(l);
        if (!store().contains(k)) continue;
        const QStringList urls = store().value(k).toStringList();
        store().remove(k);
        store().sync();
        if (adoptSources(l, urls)) changed = true;
    }
    return changed;
}
} // namespace

bool AddonRoster::adoptLegacyRegistrySources(RegistryList list, const QStringList& urls)
{
    reconcile();                                        // this device's own edits (and its backfill) first
    const bool changed = adoptSources(list, urls);
    if (changed && g_changeHook) g_changeHook();
    return changed;
}

QStringList AddonRoster::registrySources(RegistryList list)
{
    if (store().contains(legacyListKey(RegistryList::Addons)) || store().contains(legacyListKey(RegistryList::Themes)))
        reconcile();                                     // adopts them (after the roster's own backfill)
    const QHash<QString, qint64> tombs = localTombs();
    QVector<Record> mine;
    for (const Record& r : records())
        if (r.isRegistrySource() && r.sourceList == listName(list) && !suppressed(r, tombs)
            && !isBuiltInRegistry(list, r.sourceUrl))
            mine.push_back(r);
    // Oldest first — where the old list put a new registry, at the end — and the same order on every device.
    std::sort(mine.begin(), mine.end(), [](const Record& a, const Record& b) {
        return a.ts != b.ts ? a.ts < b.ts : a.key < b.key;
    });
    QStringList out;
    for (const Record& r : mine) out << r.sourceUrl;
    return out;
}

bool AddonRoster::addRegistrySource(RegistryList list, const QString& url, qint64 now)
{
    const QString u = normalizeRegistryUrl(url);
    if (u.isEmpty() || isBuiltInRegistry(list, u)) return false;
    if (now <= 0) now = nowSecs();
    reconcile(now);                                      // this device's other edits are stamped first
    QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const QString key = registrySourceKey(list, u);
    const int i = indexByKey(recs, key);
    if (i >= 0 && !suppressed(recs[i], tombs)) return false;   // already configured
    qint64 ts = now;                                     // strictly newer than what it undoes, so it wins
    if (i >= 0) { ts = std::max(ts, recs[i].ts + 1); recs.remove(i); }
    if (tombs.contains(key)) { ts = std::max(ts, tombs.value(key) + 1); Tombstones::remove(tombStore(), key); }
    Record r;
    r.key = key;
    r.sourceList = listName(list);
    r.sourceUrl = u;
    r.ts = ts;
    recs.push_back(r);
    saveRecords(recs);
    if (g_changeHook) g_changeHook();
    return true;
}

bool AddonRoster::removeRegistrySource(RegistryList list, const QString& url, qint64 now)
{
    const QString u = normalizeRegistryUrl(url);
    if (u.isEmpty() || isBuiltInRegistry(list, u)) return false;   // a built-in is never tombstoned
    if (now <= 0) now = nowSecs();
    reconcile(now);
    QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const QString key = registrySourceKey(list, u);
    const int i = indexByKey(recs, key);
    if (i < 0) return false;                             // not configured: nothing to remove
    qint64 ts = std::max(now, recs[i].ts + 1);
    if (tombs.contains(key)) ts = std::max(ts, tombs.value(key) + 1);
    Tombstones::record(tombStore(), key, ts);
    recs.remove(i);
    saveRecords(recs);
    if (g_changeHook) g_changeHook();
    return true;
}

AddonRoster::Record AddonRoster::fromJson(const QJsonObject& o)
{
    Record r;
    r.key = o.value(QStringLiteral("key")).toString();
    if (r.key.isEmpty()) return Record{};
    // A registry source (increment 4) is read from its KEY, not its fields: a build before this one keeps the
    // record but drops the fields it does not know, and what it hands back must still be a registry source.
    if (parseSourceKey(r.key, &r.sourceList, &r.sourceUrl))
    {
        const qint64 sts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        r.ts = sts > 0 ? sts : 0;
        return r;                                        // no url, no flag, no reference: one kind per record
    }
    r.url = normalizeBase(o.value(QStringLiteral("url")).toString());
    r.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    r.ts = ts > 0 ? ts : 0;
    // A record is ONE kind. A registry reference with a URL, or without an entry to install, is read as the
    // plain record it otherwise is rather than guessed at.
    if (o.value(QStringLiteral("kind")).toString() == QLatin1String("registry") && r.url.isEmpty())
    {
        r.registry = o.value(QStringLiteral("registry")).toString().trimmed();
        r.entry = o.value(QStringLiteral("entry")).toString();
        r.version = o.value(QStringLiteral("version")).toString();
        if (r.registry.isEmpty() || r.entry.isEmpty()) { r.registry.clear(); r.entry.clear(); r.version.clear(); }
    }
    return r;
}

// ---- the folder add-ons (issue #77, increment 2) -----------------------------------------------------------

QString AddonRoster::provenanceFileName() { return QStringLiteral(".registry-origin.json"); }

AddonRoster::Provenance AddonRoster::readProvenance(const QString& addonDir)
{
    Provenance p;
    QFile f(addonDir + QLatin1Char('/') + provenanceFileName());
    if (!f.open(QIODevice::ReadOnly)) return p;
    const QJsonObject o = QJsonDocument::fromJson(f.read(64 * 1024)).object();
    p.registry = o.value(QStringLiteral("registry")).toString().trimmed();
    p.entry = o.value(QStringLiteral("entry")).toString();
    p.version = o.value(QStringLiteral("version")).toString();
    p.installedAt = static_cast<qint64>(o.value(QStringLiteral("installedAt")).toDouble());
    if (!p.valid()) return Provenance{};
    return p;
}

bool AddonRoster::writeProvenance(const QString& addonDir, const Provenance& p)
{
    if (!p.valid()) return false;
    QJsonObject o;
    o.insert(QStringLiteral("registry"), p.registry);
    o.insert(QStringLiteral("entry"), p.entry);
    o.insert(QStringLiteral("version"), p.version);
    o.insert(QStringLiteral("installedAt"), static_cast<double>(p.installedAt));
    QSaveFile f(addonDir + QLatin1Char('/') + provenanceFileName());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool AddonRoster::isFirstPartyId(const QString& manifestId)
{
    // The previous brand's namespace counts too: a bundled add-on may still carry it until the id migration is
    // confirmed, and no third-party package may install under either (AddonManager::installPackage refuses it).
    return manifestId.startsWith(QLatin1String(AppBrand::kAddonPrefix))
        || manifestId.startsWith(QLatin1String(AppBrand::Legacy::kAddonPrefix));
}

void AddonRoster::setAddonsRoot(const QString& root) { g_addonsRoot = root; }

QString AddonRoster::addonsRoot()
{
    return g_addonsRoot.isEmpty() ? AppPaths::dataDir() + QStringLiteral("/addons") : g_addonsRoot;
}

QVector<AddonRoster::LocalAddon> AddonRoster::localAddons()
{
    QVector<LocalAddon> out;
    const QFileInfoList dirs = QDir(addonsRoot()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : dirs)
    {
        // _storage is add-on-private storage, and a dot-folder is an install's staging area; neither is an add-on.
        if (d.fileName() == QLatin1String("_storage") || d.fileName().startsWith(QLatin1Char('.'))) continue;
        QFile mf(d.absoluteFilePath() + QStringLiteral("/manifest.json"));
        if (!mf.open(QIODevice::ReadOnly)) continue;
        LocalAddon a;
        a.dir = d.absoluteFilePath();
        a.id = QJsonDocument::fromJson(mf.readAll()).object().value(QStringLiteral("id")).toString();
        if (a.id.isEmpty()) a.id = d.fileName();
        a.firstParty = isFirstPartyId(a.id);
        if (!a.firstParty) a.provenance = readProvenance(a.dir);
        out.push_back(a);
    }
    return out;
}

QVector<AddonRoster::Record> AddonRoster::pendingRegistryInstalls()
{
    const QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    QSet<QString> ids, folders;
    for (const LocalAddon& a : localAddons()) { ids.insert(a.id); folders.insert(QFileInfo(a.dir).fileName()); }
    QVector<Record> out;
    for (const Record& r : recs)
        if (r.isRegistry() && !suppressed(r, tombs) && !ids.contains(r.key) && !folders.contains(r.entry))
            out.push_back(r);
    return out;
}

void AddonRoster::recordRemoval(const QString& key)
{
    if (key.isEmpty()) return;
    const qint64 now = nowSecs();
    reconcile(now);                                     // this device's other edits are stamped first
    QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const int i = indexByKey(recs, key);
    qint64 ts = now;
    if (i >= 0) ts = std::max(ts, recs[i].ts + 1);
    if (tombs.contains(key)) ts = std::max(ts, tombs.value(key) + 1);
    Tombstones::record(tombStore(), key, ts);
    if (i >= 0) { recs.remove(i); saveRecords(recs); }
    if (g_changeHook) g_changeHook();
}

QVector<AddonRoster::Record> AddonRoster::records()
{
    QVector<Record> out;
    const QJsonArray arr = QJsonDocument::fromJson(store().value(itemsKey()).toString().toUtf8()).array();
    for (const QJsonValue& v : arr)
    {
        const Record r = fromJson(v.toObject());
        if (!r.key.isEmpty() && indexByKey(out, r.key) < 0) out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.key < b.key; });
    return out;
}

void AddonRoster::saveRecords(QVector<Record> recs)
{
    std::sort(recs.begin(), recs.end(), [](const Record& a, const Record& b) { return a.key < b.key; });
    QJsonArray arr;
    for (const Record& r : recs) arr.append(toJson(r));
    store().setValue(itemsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

bool AddonRoster::reconcile(qint64 now)
{
    if (now <= 0) now = nowSecs();
    QSettings& s = store();
    // No shadow at all yet: this is the first run of a build that has one. What is live is BACKFILLED at ts 0
    // (its real time is unknown) and nothing is tombstoned — an absence here predates the shadow and says
    // nothing about a removal.
    const bool backfill = !s.contains(itemsKey());

    QVector<Record> recs = records();
    QHash<QString, qint64> tombs = localTombs();
    bool changed = backfill;

    // The stamp a LOCAL EDIT gets: now, but always strictly newer than the version it edits and than any
    // tombstone on the key — so the edit wins every merge it meets, clock skew and same-second re-adds
    // included (a re-add in the second of its own removal would otherwise tie the tombstone and lose to `>=`).
    auto editStamp = [&](const Record* r, const QString& key) {
        qint64 ts = now;
        if (r) ts = std::max(ts, r->ts + 1);
        if (tombs.contains(key)) ts = std::max(ts, tombs.value(key) + 1);
        return ts;
    };
    auto clearTomb = [&](const QString& key) {
        if (!tombs.contains(key)) return;
        Tombstones::remove(tombStore(), key);
        tombs.remove(key);
    };

    const QStringList live = liveUrls();
    const QSet<QString> liveSet(live.begin(), live.end());
    const QHash<QString, bool> flags = liveFlags();
    const QVector<LocalAddon> locals = localAddons();
    const QSet<QString> sideloaded = sideloadedIds(locals);

    // 1. Every live URL has a record.
    for (const QString& u : live)
    {
        const int i = indexByUrl(recs, u);
        if (i >= 0)
        {
            // Live, yet suppressed by our own tombstone: a removal that was undone underneath us (a settings
            // Discard restores the live list; the tombstone is not in the transaction). Re-adding it now is what
            // the user's Discard meant, on every device.
            if (!backfill && suppressed(recs[i], tombs))
            {
                recs[i].ts = editStamp(&recs[i], recs[i].key);
                clearTomb(recs[i].key);
                changed = true;
            }
            continue;
        }
        const QString k = keyForUrl(u);
        const int j = indexByKey(recs, k);
        if (j >= 0)
        {
            // A second live URL for an add-on whose record's URL is also live: pre-#80 duplicates. The record
            // keeps its URL and the projection collapses the list onto it, exactly as #80's replace does.
            if (!recs[j].url.isEmpty() && liveSet.contains(recs[j].url)) continue;
            // Otherwise this key's URL changed (a re-configure), or an enabled-only record gained a URL.
            recs[j].url = u;
            if (!backfill) { recs[j].ts = editStamp(&recs[j], k); clearTomb(k); }
            changed = true;
            continue;
        }
        Record n;
        n.key = k;
        n.url = u;
        n.enabled = flags.value(k, true);
        n.ts = backfill ? 0 : editStamp(nullptr, k);
        if (!backfill) clearTomb(k);
        recs.push_back(n);
        changed = true;
    }

    // 1b. Every registry-installed folder has a kind-"registry" record (issue #77, increment 2). The record is
    //     the REFERENCE — registry, entry, version — and never the code. The opposite direction is deliberately
    //     absent: a registry record with no folder here is NOT a removal, because it is just as often a
    //     reference from another device that this one has not installed (yet, or ever — its registry may not be
    //     configured here). A real uninstall says so through recordRemoval().
    for (const LocalAddon& a : locals)
    {
        if (a.firstParty || !a.provenance.valid()) continue;
        const int j = indexByKey(recs, a.id);
        if (j >= 0)
        {
            if (!recs[j].url.isEmpty()) continue;       // a remote record already owns this key: one kind each
            if (recs[j].registry == a.provenance.registry && recs[j].entry == a.provenance.entry)
            {
                // Installed, yet suppressed by our own tombstone: re-installed after a removal. As for a URL.
                if (!backfill && suppressed(recs[j], tombs))
                {
                    recs[j].ts = editStamp(&recs[j], recs[j].key);
                    clearTomb(recs[j].key);
                    changed = true;
                }
                continue;
            }
            // A flag-only record gaining its reference, or a re-install from a different registry.
            recs[j].registry = a.provenance.registry;
            recs[j].entry = a.provenance.entry;
            recs[j].version = a.provenance.version;
            if (!backfill) { recs[j].ts = editStamp(&recs[j], a.id); clearTomb(a.id); }
            changed = true;
            continue;
        }
        Record n;
        n.key = a.id;
        n.enabled = flags.value(a.id, true);
        n.registry = a.provenance.registry;
        n.entry = a.provenance.entry;
        n.version = a.provenance.version;
        n.ts = backfill ? 0 : editStamp(nullptr, a.id);
        if (!backfill) clearTomb(a.id);
        recs.push_back(n);
        changed = true;
    }

    // 2. A record whose URL left the live list was removed here — outside AddonManager's API when it gets this
    //    far (a Discard, an older build) — so it is a removal, dated now. Not on a backfill: see above.
    if (!backfill)
    {
        for (int i = recs.size() - 1; i >= 0; --i)
        {
            const Record& r = recs[i];
            if (r.url.isEmpty() || liveSet.contains(r.url)) continue;
            if (!suppressed(r, tombs))
            {
                const qint64 ts = editStamp(&r, r.key);
                Tombstones::record(tombStore(), r.key, ts);
                // ...and into the local view too, or pass 3 below would find the removed add-on's leftover
                // flag with no record and no tombstone and re-create it as a NEWER enabled-only record — one
                // that out-dates this very tombstone on every peer, so the removal never propagates.
                tombs.insert(r.key, ts);
            }
            recs.remove(i);
            changed = true;
        }
    }

    // 3. Every live flag has a record, and a flipped flag is an edit.
    for (auto it = flags.begin(); it != flags.end(); ++it)
    {
        if (sideloaded.contains(it.key())) continue;    // device-only: its flag is not the roster's business
        const int j = indexByKey(recs, it.key());
        if (j < 0)
        {
            // A flag left behind by an add-on this device removed (removeRemoteSource keeps it, so a re-add
            // keeps the user's choice) is not a new statement: the tombstone already says what happened.
            if (tombs.contains(it.key())) continue;
            Record n;
            n.key = it.key();
            n.enabled = it.value();
            n.ts = backfill ? 0 : editStamp(nullptr, it.key());
            recs.push_back(n);
            changed = true;
        }
        else if (!backfill && recs[j].enabled != it.value())
        {
            recs[j].enabled = it.value();
            recs[j].ts = editStamp(&recs[j], it.key());
            changed = true;
        }
    }

    if (changed) saveRecords(recs);
    // 4. This device's own pre-roster registry lists (increment 4), adopted once, after the backfill above.
    const bool adoptedLists = adoptLocalLegacyLists();
    return (changed && !(backfill && recs.isEmpty() && live.isEmpty() && flags.isEmpty())) || adoptedLists;
}

void AddonRoster::touched()
{
    if (reconcile() && g_changeHook) g_changeHook();
}

bool AddonRoster::project(bool dryRun)
{
    QSettings& s = store();
    const QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const QStringList live = liveUrls();

    QStringList out;
    QSet<QString> used;
    auto take = [&](const QString& u) { if (!used.contains(u)) { used.insert(u); out << u; } };

    for (const QString& u : live)
    {
        const int i = indexByUrl(recs, u);
        if (i >= 0)
        {
            if (!suppressed(recs[i], tombs)) take(u);    // suppressed: removed on another device -> dropped
            continue;
        }
        const QString k = keyForUrl(u);
        const int j = indexByKey(recs, k);
        if (j >= 0 && !suppressed(recs[j], tombs))
        {
            // Re-configured on another device: the newer URL takes this entry's place (#80's in-place rule).
            // An enabled-only record states nothing about a URL, so the live one stays.
            take(recs[j].url.isEmpty() ? u : recs[j].url);
            continue;
        }
        if (tombs.contains(k)) continue;                 // its record lost to a tombstone: removed elsewhere
        take(u);                                         // no statement either way: never drop on absence
    }
    for (const Record& r : recs)                         // key order: the same on every device
        if (!r.url.isEmpty() && !suppressed(r, tombs)) take(r.url);

    bool changed = (out != live);

    const QVector<LocalAddon> locals = localAddons();
    const QSet<QString> sideloaded = sideloadedIds(locals);

    struct FlagWrite { QString key; bool value; };
    QVector<FlagWrite> flagWrites;
    for (const Record& r : recs)
    {
        if (r.isRegistrySource() || keyIsUrl(r) || suppressed(r, tombs) || sideloaded.contains(r.key)) continue;
        const QString k = QString(kEnabledPrefix) + r.key;
        const bool cur = s.contains(k) ? s.value(k).toBool() : true;   // absent = enabled (AddonManager's default)
        if (cur != r.enabled) flagWrites.push_back({ k, r.enabled });
    }
    if (!flagWrites.isEmpty()) changed = true;

    // A registry-installed folder whose record a tombstone has removed — uninstalled on another device, or here
    // through a merge of our own older state — is uninstalled here too. Only a folder carrying provenance: a
    // sideloaded folder that happens to share the id is this device's own, and nothing synced may delete it.
    QStringList uninstall;
    for (const LocalAddon& a : locals)
    {
        if (a.firstParty || !a.provenance.valid() || !tombs.contains(a.id)) continue;
        const int i = indexByKey(recs, a.id);
        if (i < 0 || suppressed(recs[i], tombs)) uninstall << a.dir;
    }
    if (!uninstall.isEmpty()) changed = true;

    if (dryRun || !changed) return changed;

    for (const QString& dir : uninstall)
    {
        // Provenance first: if the folder then refuses to delete (a file held open), what is left is a plain
        // device-only folder — not a registry install the next reconcile would read as a re-install and re-add.
        QFile::remove(dir + QLatin1Char('/') + provenanceFileName());
        QDir(dir).removeRecursively();
    }

    if (out != live)
    {
        const QSet<QString> kept(out.begin(), out.end());
        for (const QString& u : live)
            if (!kept.contains(u)) s.remove(manifestCacheKey(u));   // as removeRemoteSource does
        writeLiveUrls(out);
    }
    for (const FlagWrite& w : flagWrites) s.setValue(w.key, w.value);
    s.sync();
    return true;
}

void AddonRoster::adoptLegacySnapshot(const QString& urlsJson, const QHash<QString, bool>& enabled)
{
    reconcile();                                        // this device's own edits are stamped first
    QVector<Record> recs = records();
    const QHash<QString, qint64> tombs = localTombs();
    const QSet<QString> sideloaded = sideloadedIds(localAddons());   // #77 inc 2: device-only, never adopted
    bool changed = false;

    const QJsonArray arr = QJsonDocument::fromJson(urlsJson.toUtf8()).array();
    for (const QJsonValue& v : arr)
    {
        const QString base = normalizeBase(v.toString());
        if (base.isEmpty() || indexByUrl(recs, base) >= 0) continue;   // already held, by URL
        const QString k = keyForUrl(base);
        if (tombs.contains(k)) continue;                // removed here: an undated snapshot cannot undo that
        const int j = indexByKey(recs, k);
        if (j >= 0)
        {
            // Already held under its id. A URL we hold wins over the snapshot's; an enabled-only record gains
            // the URL, keeping its own stamp.
            if (recs[j].url.isEmpty()) { recs[j].url = base; changed = true; }
            continue;
        }
        Record n;
        n.key = k;
        n.url = base;
        n.enabled = enabled.value(k, true);
        n.ts = 0;
        recs.push_back(n);
        changed = true;
    }
    for (auto it = enabled.begin(); it != enabled.end(); ++it)
    {
        if (it.key().isEmpty() || indexByKey(recs, it.key()) >= 0 || tombs.contains(it.key())
            || sideloaded.contains(it.key()))
            continue;
        Record n;
        n.key = it.key();
        n.enabled = it.value();
        n.ts = 0;
        recs.push_back(n);
        changed = true;
    }
    if (!changed) return;
    saveRecords(recs);
    project();
    if (g_changeHook) g_changeHook();
}

void AddonRoster::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

void AddonRoster::markMigrationsRun() { g_migrationsRun = true; }
bool AddonRoster::migrationsRun() { return g_migrationsRun; }
