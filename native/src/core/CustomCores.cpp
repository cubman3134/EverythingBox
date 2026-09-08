#include "CustomCores.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace {
const QLatin1String kPrefix("custom:");
}

// ---- pure vocabulary ------------------------------------------------------------------------------------

QString CustomCores::refFor(const QString& id)
{
    return id.isEmpty() ? QString() : (QString(kPrefix) + id);
}

bool CustomCores::isCustomRef(const QString& ref)
{
    return ref.startsWith(kPrefix) && ref.size() > kPrefix.size();
}

QString CustomCores::idFromRef(const QString& ref)
{
    return isCustomRef(ref) ? ref.mid(kPrefix.size()) : QString();
}

QString CustomCores::sanitizeId(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    bool pendingSep = false;
    for (const QChar ch : raw)
    {
        const QChar lower = ch.toLower();
        if ((lower >= QLatin1Char('a') && lower <= QLatin1Char('z'))
            || (lower >= QLatin1Char('0') && lower <= QLatin1Char('9')))
        {
            if (pendingSep && !out.isEmpty()) out.append(QLatin1Char('_'));
            pendingSep = false;
            out.append(lower);
        }
        else
            pendingSep = true;   // collapse any run of separators to a single '_' (and never a trailing one)
    }
    return out;
}

// ---- pure JSON ------------------------------------------------------------------------------------------

QJsonObject CustomCores::toJson(const CustomCore& c)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), c.id);
    o.insert(QStringLiteral("path"), c.path);
    o.insert(QStringLiteral("name"), c.name);
    if (!c.version.isEmpty()) o.insert(QStringLiteral("version"), c.version);
    if (!c.extensions.isEmpty())
    {
        QJsonArray a;
        for (const QString& e : c.extensions) a.push_back(e);
        o.insert(QStringLiteral("extensions"), a);
    }
    if (c.supportsNoGame) o.insert(QStringLiteral("supportsNoGame"), true);
    if (c.needFullpath)   o.insert(QStringLiteral("needFullpath"), true);
    if (!c.needs.isEmpty()) o.insert(QStringLiteral("needs"), c.needs);
    if (c.addedAt != 0)   o.insert(QStringLiteral("addedAt"), c.addedAt);
    return o;
}

CustomCore CustomCores::fromJson(const QJsonObject& o)
{
    CustomCore c;
    c.id      = o.value(QStringLiteral("id")).toString().trimmed();
    c.path    = o.value(QStringLiteral("path")).toString().trimmed();
    c.name    = o.value(QStringLiteral("name")).toString().trimmed();
    c.version = o.value(QStringLiteral("version")).toString().trimmed();
    for (const QJsonValue& v : o.value(QStringLiteral("extensions")).toArray())
    {
        const QString e = v.toString().trimmed().toLower();
        if (!e.isEmpty()) c.extensions.push_back(e);
    }
    c.supportsNoGame = o.value(QStringLiteral("supportsNoGame")).toBool(false);
    c.needFullpath   = o.value(QStringLiteral("needFullpath")).toBool(false);
    c.needs          = o.value(QStringLiteral("needs")).toString();
    c.addedAt        = static_cast<qint64>(o.value(QStringLiteral("addedAt")).toDouble(0));
    return c;
}

QByteArray CustomCores::serialize(const CustomCoreRegistry& r)
{
    QJsonArray cores;
    for (const CustomCore& c : r.cores) cores.push_back(toJson(c));
    QJsonObject root;
    root.insert(QStringLiteral("noticeAcknowledged"), r.noticeAcknowledged);
    root.insert(QStringLiteral("cores"), cores);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

CustomCoreRegistry CustomCores::parse(const QByteArray& bytes, QString* err)
{
    CustomCoreRegistry r;
    if (bytes.trimmed().isEmpty()) return r;      // absent/empty file == an empty registry, not an error

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError)
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "not valid JSON (%1 at offset %2)")
                            .arg(pe.errorString()).arg(pe.offset);
        return r;
    }
    if (!doc.isObject())
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "the registry must be a JSON object");
        return r;
    }
    const QJsonObject root = doc.object();
    r.noticeAcknowledged = root.value(QStringLiteral("noticeAcknowledged")).toBool(false);
    const QJsonValue coresValue = root.value(QStringLiteral("cores"));
    if (!coresValue.isArray())
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "\"cores\" must be an array");
        r.noticeAcknowledged = false;             // an unusable document is an EMPTY registry, not a half one
        return r;
    }
    int idx = 0;
    for (const QJsonValue& v : coresValue.toArray())
    {
        const int here = idx++;
        if (!v.isObject())
        {
            if (err) *err = QCoreApplication::translate("CustomCores", "entry %1 is not an object — skipped").arg(here);
            continue;
        }
        const CustomCore c = fromJson(v.toObject());
        if (c.id.isEmpty() || c.path.isEmpty())
        {
            if (err) *err = QCoreApplication::translate("CustomCores", "entry %1 has no id/path — skipped").arg(here);
            continue;
        }
        r.cores.push_back(c);
    }
    return r;
}

// ---- THE PURE MODEL -------------------------------------------------------------------------------------

QStringList CustomCores::augmentCandidates(const QStringList& catalogue, const QStringList& systemExtensions,
                                           const QList<CustomCore>& customs)
{
    // The identity rail, spelled as an early return so it is impossible to break by accident: nothing
    // registered means the catalogue list is handed straight back, same object value, same order.
    if (customs.isEmpty()) return catalogue;

    QStringList out = catalogue;
    for (const CustomCore& c : customs)
    {
        if (c.id.isEmpty()) continue;
        bool claims = false;
        for (const QString& e : c.extensions)
            if (systemExtensions.contains(e.toLower())) { claims = true; break; }
        if (!claims) continue;
        const QString ref = refFor(c.id);
        if (!out.contains(ref)) out.push_back(ref);   // APPEND: cores[0] stays the catalogue default
    }
    return out;
}

bool CustomCores::mayDownload(const QString& coreName) { return !isCustomRef(coreName); }

QList<CustomCore> CustomCores::runEntries(const QList<CustomCore>& customs)
{
    QList<CustomCore> out;
    for (const CustomCore& c : customs)
        if (c.supportsNoGame && !c.id.isEmpty()) out.push_back(c);
    return out;
}

bool CustomCores::noticeDue(const CustomCoreRegistry& r) { return !r.noticeAcknowledged; }

QString CustomCores::noticeText()
{
    return QCoreApplication::translate(
        "CustomCores",
        "Custom cores are not curated by EverythingBox. This one runs as native code inside the app: if it "
        "crashes, misbehaves or corrupts a save, that is between you and the core. Shown once — you will not "
        "be asked again.");
}

// ---- the registry ---------------------------------------------------------------------------------------

QString CustomCores::customDir()
{
    const QString d = AppPaths::dataDir() + QStringLiteral("/cores/custom");
    QDir().mkpath(d);
    return d;
}

QString CustomCores::registryPath()
{
    return customDir() + QStringLiteral("/registry.json");
}

namespace {

CustomCoreRegistry& cache()
{
    static CustomCoreRegistry r;
    return r;
}
bool& cacheLoaded()
{
    static bool loaded = false;
    return loaded;
}

void readFromDisk()
{
    QByteArray bytes;
    QFile f(CustomCores::registryPath());
    if (f.open(QIODevice::ReadOnly)) { bytes = f.readAll(); f.close(); }
    QString err;
    cache() = CustomCores::parse(bytes, &err);
    if (!err.isEmpty())
        qWarning("CustomCores: %s", qUtf8Printable(err));
    cacheLoaded() = true;
}

bool writeToDisk(QString* err)
{
    QSaveFile f(CustomCores::registryPath());
    if (!f.open(QIODevice::WriteOnly))
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "couldn't write %1").arg(CustomCores::registryPath());
        return false;
    }
    f.write(CustomCores::serialize(cache()));
    if (!f.commit())
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "couldn't write %1").arg(CustomCores::registryPath());
        return false;
    }
    return true;
}

} // namespace

const CustomCoreRegistry& CustomCores::registry()
{
    if (!cacheLoaded()) readFromDisk();
    return cache();
}

void CustomCores::reload() { readFromDisk(); }

const QList<CustomCore>& CustomCores::all() { return registry().cores; }

const CustomCore* CustomCores::byRef(const QString& ref)
{
    const QString id = idFromRef(ref);
    if (id.isEmpty()) return nullptr;
    for (const CustomCore& c : all())
        if (c.id == id) return &c;
    return nullptr;
}

QString CustomCores::pathForRef(const QString& ref)
{
    const CustomCore* c = byRef(ref);
    return c ? c->path : QString();
}

QString CustomCores::displayNameFor(const QString& ref)
{
    const CustomCore* c = byRef(ref);
    if (!c) return QString();
    const QString shown = c->name.trimmed().isEmpty() ? c->id : c->name.trimmed();
    return QCoreApplication::translate("CustomCores", "%1 (custom core)").arg(shown);
}

bool CustomCores::add(const CustomCore& c, QString* err)
{
    if (c.id.isEmpty() || c.path.isEmpty())
    {
        if (err) *err = QCoreApplication::translate("CustomCores", "a custom core needs an id and a path");
        return false;
    }
    registry();  // ensure the cache is warm before mutating it
    bool replaced = false;
    for (int i = 0; i < cache().cores.size(); ++i)
        if (cache().cores[i].id == c.id) { cache().cores[i] = c; replaced = true; break; }
    if (!replaced) cache().cores.push_back(c);
    return writeToDisk(err);
}

bool CustomCores::remove(const QString& id)
{
    if (id.isEmpty()) return false;
    registry();
    for (int i = 0; i < cache().cores.size(); ++i)
        if (cache().cores[i].id == id)
        {
            cache().cores.removeAt(i);
            writeToDisk(nullptr);
            return true;
        }
    return false;
}

void CustomCores::acknowledgeNotice()
{
    registry();
    if (cache().noticeAcknowledged) return;
    cache().noticeAcknowledged = true;
    writeToDisk(nullptr);
}
