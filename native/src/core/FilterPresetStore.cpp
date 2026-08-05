#include "FilterPresetStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each user has their own presets (mirrors FavoritesStore::favKey()).
static QString presetsKey()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("filterpresets/") + (id.isEmpty() ? QStringLiteral("default") : id)
           + QStringLiteral("/items");
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
    for (int i = presets.size() - 1; i >= 0; --i)
        if (presets[i].name == preset.name) presets.remove(i); // upsert by name
    FilterPreset stamped = preset;
    stamped.ts = QDateTime::currentSecsSinceEpoch();
    presets.prepend(stamped); // newest first
    saveAll(presets);
    fireChanged();
}

void FilterPresetStore::remove(const QString& name)
{
    if (name.isEmpty()) return;
    QVector<FilterPreset> presets = list();
    const int before = presets.size();
    for (int i = presets.size() - 1; i >= 0; --i)
        if (presets[i].name == name) presets.remove(i);
    if (presets.size() == before) return; // nothing removed: don't churn the store or fire the hook
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
