#include "FilterPresetStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"      // issue #184: a delete leaves a dated tombstone so a peer cannot resurrect it

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The active profile leaf, matching FavoritesStore::favKey()'s per-profile shape. Factored out so the data key
// and the tombstone-store name below cannot drift on the profile spelling (both fall back to "default").
static QString presetProfile()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

// Per-profile, so each user has their own presets (mirrors FavoritesStore::favKey()).
static QString presetsKey() { return QStringLiteral("filterpresets/") + presetProfile() + QStringLiteral("/items"); }

// Tombstone store for THIS profile's presets, keyed by preset `id` — the namespace CloudMerge's serializer
// reads (see CloudMerge.cpp presetTombStore()). Its per-profile shape mirrors presetsKey()'s.
static QString presetTombStore() { return QStringLiteral("filterpresets/") + presetProfile(); }

// The deterministic legacy id (see header). A UUIDv5 over the preset name under a fixed namespace: same name ->
// same id on every device, so two peers that both hold a pre-#184 copy of the preset converge rather than
// duplicating. New presets never reach here — save() mints a random id for them, precisely so a rename (which
// changes the name) does NOT change the id.
QString FilterPresetStore::syncIdForName(const QString& name)
{
    if (name.isEmpty()) return QString();
    static const QUuid ns(QStringLiteral("{6f1b2e7a-5c34-4d81-9a2f-1e0c7b3d4a55}")); // fixed FilterPreset namespace
    return QUuid::createUuidV5(ns, name).toString(QUuid::WithoutBraces);
}

static std::function<void()> g_changeHook;
void FilterPresetStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QVector<FilterPreset> FilterPresetStore::list()
{
    QVector<FilterPreset> out;
    const QByteArray json = store().value(presetsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        FilterPreset p;
        p.name   = o.value(QStringLiteral("name")).toString();
        p.filter = gamefilter::Filter::fromJson(o.value(QStringLiteral("filter")).toObject());
        p.ts     = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        p.id     = o.value(QStringLiteral("id")).toString();
        // Back-fill a legacy row (written by #63, before `id` existed) in-memory and deterministically, so
        // every reader below — save()'s upsert, remove()'s tombstone key — sees a stable non-empty id, and the
        // next saveAll() persists it. Deterministic (not random) so two devices agree on the id (see header).
        if (p.id.isEmpty()) p.id = syncIdForName(p.name);
        if (!p.name.isEmpty()) out.push_back(p);
    }
    return out;
}

static void saveAll(const QVector<FilterPreset>& presets)
{
    QJsonArray arr;
    for (const FilterPreset& p : presets)
    {
        QJsonObject o;
        // Persist the id (back-filled deterministically if a legacy row somehow reached here without one), so
        // the merge identity is durable and this row stops being a legacy row on the next read.
        o.insert(QStringLiteral("id"), p.id.isEmpty() ? FilterPresetStore::syncIdForName(p.name) : p.id);
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("filter"), p.filter.toJson());
        o.insert(QStringLiteral("ts"), static_cast<double>(p.ts));
        arr.append(o);
    }
    store().setValue(presetsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

void FilterPresetStore::save(const FilterPreset& preset)
{
    if (preset.name.isEmpty()) return;
    QVector<FilterPreset> presets = list();
    QString keepId;                                            // carry the existing id across an upsert
    for (int i = presets.size() - 1; i >= 0; --i)
        if (presets[i].name == preset.name) { keepId = presets[i].id; presets.remove(i); } // upsert by name
    FilterPreset stamped = preset;
    stamped.ts = QDateTime::currentSecsSinceEpoch();
    // Identity: an upsert of an existing name keeps that preset's id (a re-save is an EDIT, not a new preset);
    // a brand-new preset gets a random id — random, not name-derived, so a later rename keeps the id and the
    // merge sees a mutable-name edit rather than a delete+add (issue #184, see header). A caller-supplied id
    // (there are none today) is honoured only when there is no existing row to inherit from.
    if (!keepId.isEmpty())        stamped.id = keepId;
    else if (stamped.id.isEmpty()) stamped.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    presets.prepend(stamped); // newest first
    saveAll(presets);
    fireChanged();
}

void FilterPresetStore::remove(const QString& name)
{
    if (name.isEmpty()) return;
    QVector<FilterPreset> presets = list();
    QString removedId;
    for (int i = presets.size() - 1; i >= 0; --i)
        if (presets[i].name == name) { removedId = presets[i].id; presets.remove(i); }
    if (removedId.isEmpty()) return; // nothing removed: don't churn the store, tombstone nothing, or fire the hook
    // Leave a DATED tombstone keyed by the removed id BEFORE the row goes, exactly as FavoritesStore::remove
    // does (issue #184, the #132/#166 discipline): the bare removal below is indistinguishable from "never
    // known", so a peer that still holds this preset would re-add it on the next merge without a tombstone that
    // out-dates its copy. list() guarantees removedId is non-empty (a legacy row is back-filled in-memory), so
    // this never records against an empty key. A re-created preset gets a fresh random id, never this one, so
    // the tombstone is not something a legitimate re-add has to clear.
    Tombstones::record(presetTombStore(), removedId);
    saveAll(presets);
    fireChanged();
}

bool FilterPresetStore::rename(const QString& oldName, const QString& newName)
{
    if (oldName.isEmpty() || newName.isEmpty()) return false;
    if (oldName == newName) return true;
    QVector<FilterPreset> presets = list();
    int idx = -1;
    for (int i = 0; i < presets.size(); ++i)
    {
        if (presets[i].name == newName) return false; // target name already taken
        if (presets[i].name == oldName) idx = i;
    }
    if (idx < 0) return false; // source missing
    // A rename is a mutable-name edit on a STABLE id (issue #184): only the name and ts change, the id is kept,
    // so it needs NO tombstone and the merge folds it onto the same row as a concurrent edit (newest ts wins)
    // instead of the delete+add a name-keyed store would spell — which is the whole reason identity is the id.
    presets[idx].name = newName;
    presets[idx].ts   = QDateTime::currentSecsSinceEpoch();
    saveAll(presets);
    fireChanged();
    return true;
}

bool FilterPresetStore::exists(const QString& name)
{
    for (const FilterPreset& p : list())
        if (p.name == name) return true;
    return false;
}

FilterPreset FilterPresetStore::get(const QString& name)
{
    for (const FilterPreset& p : list())
        if (p.name == name) return p;
    return FilterPreset{};
}
