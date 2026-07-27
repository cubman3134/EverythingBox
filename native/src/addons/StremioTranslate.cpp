#include "StremioTranslate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

QStringList stringArray(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& e : v.toArray()) if (e.isString()) out << e.toString();
    return out;
}

// The modern `extra[]` objects and the legacy `extraRequired`/`extraSupported` string arrays describe the
// same thing. Normalize to one list so no caller has to branch on which form an addon happened to use.
QVector<StremioTranslate::Extra> parseExtras(const QJsonObject& c)
{
    QVector<StremioTranslate::Extra> out;
    auto find = [&out](const QString& name) -> StremioTranslate::Extra* {
        for (StremioTranslate::Extra& e : out) if (e.name == name) return &e;
        return nullptr;
    };

    for (const QJsonValue& ev : c.value(QStringLiteral("extra")).toArray())
    {
        const QJsonObject eo = ev.toObject();
        StremioTranslate::Extra e;
        e.name = eo.value(QStringLiteral("name")).toString();
        if (e.name.isEmpty()) continue;
        // Deduplicate by name, FIRST-WINS — matching what the legacy `extraSupported` path below already
        // does. Without this a manifest declaring `genre` twice yields two Extras: `presets` collapses
        // them, but a UI iterating `extras` would draw the control twice.
        if (find(e.name)) continue;
        e.isRequired  = eo.value(QStringLiteral("isRequired")).toBool();
        e.options     = stringArray(eo.value(QStringLiteral("options")));
        e.optionsLimit = eo.contains(QStringLiteral("optionsLimit"))
                             ? eo.value(QStringLiteral("optionsLimit")).toInt(1) : 1;
        out.push_back(e);
    }

    // Legacy: names only, no options. Merge rather than replace — a manifest may carry both.
    for (const QString& n : stringArray(c.value(QStringLiteral("extraSupported"))))
        if (!find(n)) { StremioTranslate::Extra e; e.name = n; out.push_back(e); }
    for (const QString& n : stringArray(c.value(QStringLiteral("extraRequired"))))
    {
        if (StremioTranslate::Extra* e = find(n)) e->isRequired = true;
        else { StremioTranslate::Extra e2; e2.name = n; e2.isRequired = true; out.push_back(e2); }
    }
    return out;
}

// Decide what we can do with a catalog, and — when it is browsable only because we can supply a default —
// record that default. A catalog is NEVER dropped here: an Unsatisfiable one keeps a reason so the UI can
// say why it is missing instead of leaving a hole the user cannot explain.
void classify(StremioTranslate::Catalog& c)
{
    using U = StremioTranslate::CatalogUse;
    bool needsSearch = false;
    for (const StremioTranslate::Extra& e : c.extras)
    {
        if (!e.isRequired) continue;
        if (e.name == QStringLiteral("search")) { needsSearch = true; continue; }
        if (e.options.isEmpty())
        {
            c.use = U::Unsatisfiable;
            c.skipReason = QStringLiteral("needs a \"%1\" value the add-on does not list").arg(e.name);
            // BOTH non-Browse verdicts clear `presets`, for one shared reason: a preset is a browse
            // default, and neither of these is a browse. Callers build catalog paths straight out of
            // `presets` without re-checking `use`, so anything left here leaks into a request.
            // Here it also makes the struct ORDER-INDEPENDENT. This loop returns on the first required
            // extra we cannot supply, so without the clear the same manifest would produce different
            // presets depending only on the order the extras happened to be declared in:
            // [genre(options), mystery(none)] would keep genre="Action", [mystery, genre] nothing.
            // The verdict is order-independent; the rest of the struct must be too.
            c.presets.clear();
            return;
        }
        c.presets.insert(e.name, e.options.first());
    }
    // Search dominates: a catalog that cannot answer without a query is a search source, not a shelf —
    // even when it also takes a genre we could have defaulted. Clearing presets is what makes that real:
    // a leftover genre default would silently filter EVERY query on this catalog. Same rule as the
    // Unsatisfiable path above — do not "simplify" either clear away.
    c.use = needsSearch ? U::SearchOnly : U::Browse;
    if (needsSearch) c.presets.clear();
}

} // namespace

StremioTranslate::Manifest StremioTranslate::parseManifest(const QByteArray& body)
{
    Manifest m;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    // The same detection AddonManager used: a Stremio manifest declares what it serves and for what.
    if (!o.contains(QStringLiteral("resources")) || !o.contains(QStringLiteral("types"))) return m;

    m.id          = o.value(QStringLiteral("id")).toString();
    m.name        = o.value(QStringLiteral("name")).toString(m.id);
    m.version     = o.value(QStringLiteral("version")).toString();
    m.description = o.value(QStringLiteral("description")).toString();
    m.logo        = o.value(QStringLiteral("logo")).toString();
    m.types       = stringArray(o.value(QStringLiteral("types")));
    m.idPrefixes  = stringArray(o.value(QStringLiteral("idPrefixes")));

    // `resources` mixes plain names and objects that scope a single resource. Take the name from both, and
    // keep the object's own types/idPrefixes — those are the per-resource overrides routing needs.
    for (const QJsonValue& r : o.value(QStringLiteral("resources")).toArray())
    {
        if (r.isString()) { m.resources << r.toString(); continue; }
        const QJsonObject ro = r.toObject();
        const QString name = ro.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        m.resources << name;
        const QStringList pf = stringArray(ro.value(QStringLiteral("idPrefixes")));
        const QStringList ty = stringArray(ro.value(QStringLiteral("types")));
        if (!pf.isEmpty()) m.resourceIdPrefixes.insert(name, pf);
        if (!ty.isEmpty()) m.resourceTypes.insert(name, ty);
    }

    const QJsonObject bh = o.value(QStringLiteral("behaviorHints")).toObject();
    m.configurable          = bh.value(QStringLiteral("configurable")).toBool();
    m.configurationRequired = bh.value(QStringLiteral("configurationRequired")).toBool();

    for (const QJsonValue& cv : o.value(QStringLiteral("catalogs")).toArray())
    {
        const QJsonObject co = cv.toObject();
        Catalog c;
        c.type   = co.value(QStringLiteral("type")).toString();
        c.id     = co.value(QStringLiteral("id")).toString();
        c.name   = co.value(QStringLiteral("name")).toString(c.type);
        c.extras = parseExtras(co);
        classify(c);
        m.catalogs.push_back(c);
    }
    return m;
}
