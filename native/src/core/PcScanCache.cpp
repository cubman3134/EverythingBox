#include "PcScanCache.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with the per-item stores (same AppPaths::dataDir() posture). One
// process gets one QSettings on it; coherence with any other writer comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QVector<pcscan::ScanEntry> pcscan::merge(const QVector<ScanEntry>& cached, const ScanResult& fresh)
{
    if (fresh.status == ScanStatus::Ok)
    {
        // The scan READ the source, so it is authoritative — even when it found nothing. A genuinely empty
        // library returns empty here and DOES clear the cache; only an unreadable scan (below) falls back.
        QVector<ScanEntry> out = fresh.entries;
        for (ScanEntry& e : out) e.available = true;
        return out;
    }
    // Unreadable: keep showing the last good scan rather than dropping every game, but mark each entry
    // unavailable so the surface can badge it instead of implying it is launchable right now. `fresh.entries`
    // is deliberately ignored — an unreadable scan has nothing trustworthy to say about what is installed.
    QVector<ScanEntry> out = cached;
    for (ScanEntry& e : out) e.available = false;
    return out;
}

QJsonArray pcscan::toJson(const QVector<ScanEntry>& entries)
{
    QJsonArray arr;
    for (const ScanEntry& e : entries)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), e.id);
        o.insert(QStringLiteral("name"), e.name);
        o.insert(QStringLiteral("available"), e.available);
        arr.push_back(o);
    }
    return arr;
}

QVector<pcscan::ScanEntry> pcscan::fromJson(const QJsonArray& arr)
{
    QVector<ScanEntry> out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        ScanEntry e;
        e.id        = o.value(QStringLiteral("id")).toString();
        e.name      = o.value(QStringLiteral("name")).toString();
        // Absent -> true: a legacy blob without the flag is a last-good scan, i.e. all available.
        e.available = o.value(QStringLiteral("available")).toBool(true);
        out.push_back(e);
    }
    return out;
}

QString pcscan::iniKey(const QString& source)
{
    return QStringLiteral("pcscan/") + source;
}

QVector<pcscan::ScanEntry> pcscan::loadCached(const QString& source)
{
    const QByteArray raw = store().value(iniKey(source)).toString().toUtf8();
    if (raw.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) return {};
    return fromJson(doc.array());
}

void pcscan::storeCached(const QString& source, const QVector<ScanEntry>& entries)
{
    const QByteArray raw = QJsonDocument(toJson(entries)).toJson(QJsonDocument::Compact);
    store().setValue(iniKey(source), QString::fromUtf8(raw));
    store().sync();
}

QVector<pcscan::ScanEntry> pcscan::reconcile(const QString& source, const ScanResult& fresh)
{
    const QVector<ScanEntry> cached = loadCached(source);
    const QVector<ScanEntry> out = merge(cached, fresh);
    // Persist ONLY a successful scan. An unreadable one must leave the good cache intact — writing the
    // fallback back would overwrite the last good scan with itself and, worse, bake the unavailable flag in.
    if (fresh.status == ScanStatus::Ok) storeCached(source, out);
    return out;
}
