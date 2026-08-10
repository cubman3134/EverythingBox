#include "StremioTranslate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

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

QString StremioTranslate::catalogPath(const Catalog& c, const QMap<QString, QString>& extras)
{
    auto seg = [](const QString& s) {
        return QString::fromUtf8(QUrl::toPercentEncoding(s));
    };
    QString path = QStringLiteral("/catalog/") + seg(c.type) + QLatin1Char('/') + seg(c.id);

    // The caller's values win; presets only fill keys the caller left alone.
    QMap<QString, QString> merged = c.presets;
    for (auto it = extras.constBegin(); it != extras.constEnd(); ++it) merged.insert(it.key(), it.value());

    QStringList parts;
    // QMap iterates in key order, which is what makes this string stable for the result cache.
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it)
    {
        if (it.value().isEmpty()) continue;
        parts << seg(it.key()) + QLatin1Char('=') + seg(it.value());
    }
    if (!parts.isEmpty()) path += QLatin1Char('/') + parts.join(QLatin1Char('&'));
    return path + QStringLiteral(".json");
}

QString StremioTranslate::subtitlesPath(const QString& type, const QString& id,
                                        const QMap<QString, QString>& extras)
{
    // Built EXACTLY like catalogPath: type/id and every extra key AND value percent-encoded, the extras joined
    // in sorted key order (QMap iterates by key) so a given request always hashes to the same string. An empty
    // value is dropped rather than emitted as a bare "key=" — the same rule catalogPath applies, and the same
    // reason (some addons 400 on it).
    auto seg = [](const QString& s) {
        return QString::fromUtf8(QUrl::toPercentEncoding(s));
    };
    QString path = QStringLiteral("/subtitles/") + seg(type) + QLatin1Char('/') + seg(id);

    QStringList parts;
    for (auto it = extras.constBegin(); it != extras.constEnd(); ++it)
    {
        if (it.value().isEmpty()) continue;
        parts << seg(it.key()) + QLatin1Char('=') + seg(it.value());
    }
    if (!parts.isEmpty()) path += QLatin1Char('/') + parts.join(QLatin1Char('&'));
    return path + QStringLiteral(".json");
}

QVector<StremioTranslate::SubtitleAddonResult>
StremioTranslate::parseSubtitlesResponse(const QByteArray& body)
{
    QVector<SubtitleAddonResult> out;
    for (const QJsonValue& sv : QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("subtitles")).toArray())
    {
        const QJsonObject s = sv.toObject();
        SubtitleAddonResult r;
        r.url  = s.value(QStringLiteral("url")).toString();
        r.lang = s.value(QStringLiteral("lang")).toString();
        r.id   = s.value(QStringLiteral("id")).toString();
        if (r.url.isEmpty()) continue;   // a row with no file to load is not a choice — drop it
        out.push_back(r);
    }
    return out;
}

bool StremioTranslate::handlesId(const Manifest& m, const QString& resource, const QString& id)
{
    // A per-resource list REPLACES the manifest-level one for that resource — the object form exists
    // precisely so a stream resource can narrow what the addon as a whole claims.
    const QStringList prefixes = m.resourceIdPrefixes.contains(resource)
                                     ? m.resourceIdPrefixes.value(resource)
                                     : m.idPrefixes;
    if (prefixes.isEmpty()) return true;      // claims nothing in particular -> eligible for everything
    for (const QString& p : prefixes) if (id.startsWith(p)) return true;
    return false;
}

QVector<int> StremioTranslate::routeProviders(const QVector<Manifest>& manifests, const QString& resource,
                                              const QString& type, const QString& id, bool* fellBackToAll)
{
    if (fellBackToAll) *fellBackToAll = false;

    // Stage 1: who OFFERS this resource for this type at all. The type test uses the MANIFEST-level list
    // deliberately, not resourceTypes: a per-resource narrowing has no fallback beneath it, so honouring it
    // here could silently leave a type with no provider — the exact failure the stage-2 fallback exists to
    // prevent. Widening this is a separate decision, not a side effect of adding id routing.
    QVector<int> offering;
    for (int i = 0; i < manifests.size(); ++i)
    {
        const Manifest& m = manifests[i];
        if (!m.resources.contains(resource)) continue;
        if (!m.types.isEmpty() && !m.types.contains(type)) continue;
        offering.push_back(i);
    }

    // Stage 2: of those, who claims THIS id space — and the fallback that makes the narrowing safe.
    QVector<int> routed;
    for (int i : offering)
        if (handlesId(manifests[i], resource, id)) routed.push_back(i);
    if (!routed.isEmpty()) return routed;
    if (fellBackToAll) *fellBackToAll = !offering.isEmpty();
    return offering;
}

namespace {

// Addons put the seeder count in the title, conventionally after a 👤 (or "Seeders:"/"S:"). Best-effort:
// an unparsed count is -1, which sorts last rather than pretending to be zero.
//
// The markers are scanned as SEPARATE patterns in priority order across the whole string, deliberately
// NOT as one alternation. Regex alternation is leftmost-wins, so a single combined pattern lets the
// weakest marker pre-empt the strongest whenever it happens to sit further left: "Show S:2 E:5 👤 500"
// read 2, not 500 — and a release that reads as 2 seeders sorts below every unknown (-1) row, so
// auto-play silently lands on a different, possibly dead torrent. Priority order fixes that while the
// bare `s:` form still serves the addons that only ever emit "S: 42 | L: 3".
//
// The `(?<![a-z])` lookbehind and the REQUIRED colon on that last form are what keep sizes ("2.1 GB"),
// resolutions ("1080p") and scene episode tokens ("S01E05", "Sens8.S01") reading as unknown. Do not relax
// either one.
int scrapeSeeders(const QString& title)
{
    static const QRegularExpression markers[] = {
        QRegularExpression(QStringLiteral("\\x{1F464}\\s*(\\d+)")),
        QRegularExpression(QStringLiteral("seeders?\\s*:?\\s*(\\d+)"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral("(?<![a-z])s\\s*:\\s*(\\d+)"),
                           QRegularExpression::CaseInsensitiveOption),
    };
    for (const QRegularExpression& re : markers)
    {
        const QRegularExpressionMatch mm = re.match(title);
        if (mm.hasMatch()) return mm.captured(1).toInt();
    }
    return -1;
}

bool validInfoHash(const QString& h)
{
    if (h.size() == 40)
    {
        for (const QChar c : h)
            if (!((c >= '0' && c <= '9') || (c.toLower() >= 'a' && c.toLower() <= 'f'))) return false;
        return true;
    }
    if (h.size() == 32)
    {
        for (const QChar c : h)
        { const QChar u = c.toUpper(); if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7'))) return false; }
        return true;
    }
    return false;
}

} // namespace

QVector<StremioTranslate::StreamCandidate> StremioTranslate::parseStreams(const QByteArray& body, int maxRows)
{
    QVector<StreamCandidate> out;
    for (const QJsonValue& sv : QJsonDocument::fromJson(body).object()
                                    .value(QStringLiteral("streams")).toArray())
    {
        const QJsonObject s = sv.toObject();
        StreamCandidate c;
        c.url      = s.value(QStringLiteral("url")).toString();
        c.mime     = s.value(QStringLiteral("mime")).toString();
        c.infoHash = s.value(QStringLiteral("infoHash")).toString();
        c.fileIdx  = s.contains(QStringLiteral("fileIdx")) ? s.value(QStringLiteral("fileIdx")).toInt() : -1;
        c.name     = s.value(QStringLiteral("name")).toString();
        c.title    = s.value(QStringLiteral("title")).toString();
        if (c.title.isEmpty()) c.title = s.value(QStringLiteral("description")).toString();

        const QJsonObject bh = s.value(QStringLiteral("behaviorHints")).toObject();
        c.bingeGroup  = bh.value(QStringLiteral("bingeGroup")).toString();
        c.notWebReady = bh.value(QStringLiteral("notWebReady")).toBool();
        c.videoSize   = qint64(bh.value(QStringLiteral("videoSize")).toDouble());
        c.requestHeaders = StreamHeaders::parseProxyHeaders(bh);
        c.seeders     = scrapeSeeders(c.title);

        if (!c.isDirect() && !validInfoHash(c.infoHash)) continue;  // nothing playable here
        out.push_back(c);
    }

    // Sort THEN cap: capping first would keep the 30 rows the addon happened to list, not the 30 best.
    sortCandidates(out);
    if (maxRows >= 0 && out.size() > maxRows) out.resize(maxRows);
    return out;
}

QVector<StremioTranslate::StreamCandidate>
StremioTranslate::mergeCandidates(const QVector<QVector<StreamCandidate>>& perAddon)
{
    QVector<StreamCandidate> out;
    int total = 0;
    for (const QVector<StreamCandidate>& block : perAddon) total += block.size();
    out.reserve(total);
    for (const QVector<StreamCandidate>& block : perAddon) out += block;
    sortCandidates(out);   // NOT optional: a concatenation of sorted blocks is not itself sorted
    return out;
}

void StremioTranslate::sortCandidates(QVector<StreamCandidate>& v)
{
    std::stable_sort(v.begin(), v.end(), [](const StreamCandidate& a, const StreamCandidate& b) {
        if (a.isDirect() != b.isDirect()) return a.isDirect();   // instant beats needing a debrid round-trip
        if (a.seeders != b.seeders)       return a.seeders > b.seeders;
        return a.videoSize > b.videoSize;
    });
}

QString StremioTranslate::describe(const StreamCandidate& c)
{
    // Addons pack several lines into name/title; NavMenu word-wraps, so a row with embedded newlines reads
    // as junk. Collapse to one line and join the parts the user actually chooses on.
    // simplified() already turns every whitespace run — newlines included — into a single space.
    auto flat = [](const QString& s) { return s.simplified(); };
    QStringList parts;
    if (!c.name.isEmpty()) parts << flat(c.name);
    const QString title = flat(c.title);
    if (!title.isEmpty()) parts << title;

    // The seeder count is NEVER appended. It is scraped out of `title` — the very string this row is
    // already displaying — so appending it can only ever duplicate what the user is reading.
    // StreamCandidate::seeders still exists and still drives the sort; this is only about the rendering.
    //
    // videoSize is different: it comes from behaviorHints, a source the title need not agree with, so it
    // IS additive for the addons that do not write a size into their release line. Append it only when
    // the title carries no size token of its own — a digit followed by GB/MB, case-insensitively.
    static const QRegularExpression sizeInTitle(QStringLiteral("\\b\\d[\\d.]*\\s*[MG]B\\b"),
                                                QRegularExpression::CaseInsensitiveOption);
    if (c.videoSize > 0 && !title.contains(sizeInTitle))
        parts << QStringLiteral("%1 GB").arg(double(c.videoSize) / 1073741824.0, 0, 'f', 1);

    if (c.notWebReady) parts << QStringLiteral("may need an external player");
    QString row = parts.join(QStringLiteral(" · "));

    // One candidate must stay ONE row. Flattening the newlines is not enough on its own: a raw-torrent addon
    // routinely emits 150-200 character release lines, and NavMenu word-wraps, so a single candidate then ate
    // two rows and the list stopped being scannable. Elide to kMaxDescribeChars (see the header for why 96).
    if (row.size() > kMaxDescribeChars)
        row = row.left(kMaxDescribeChars - 1).trimmed() + QChar(0x2026); // …
    return row;
}

int StremioTranslate::pickAuto(const QVector<StreamCandidate>& all, const QString& preferGroup)
{
    if (all.isEmpty()) return -1;
    if (!preferGroup.isEmpty())
        for (int i = 0; i < all.size(); ++i)
            if (all[i].bingeGroup == preferGroup) return i;   // the release the user already chose
    return 0;   // already sorted best-first
}
