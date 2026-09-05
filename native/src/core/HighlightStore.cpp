#include "HighlightStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <algorithm>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture as
// BookmarkStore). Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each viewer keeps their own highlights. The profile leaf mirrors BookmarkStore's.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

// ---- the fixed palette ------------------------------------------------------------------------------------
// Four, and the index is what is stored. Tints a reader can tell apart at a glance on paper, sepia, dark and
// true black — which is why none of them is a saturated primary.
namespace {
struct Swatch { const char* name; const char* hex; };
const Swatch kSwatches[] = {
    { "Yellow", "#F2D06B" },
    { "Green",  "#8CC98C" },
    { "Blue",   "#7FA9DF" },
    { "Pink",   "#DF9FC0" },
};
const int kSwatchCount = int(sizeof(kSwatches) / sizeof(kSwatches[0]));
}

int HighlightStore::colorCount() { return kSwatchCount; }

int HighlightStore::normalizeColor(int index)
{
    return (index >= 0 && index < kSwatchCount) ? index : 0;
}

QString HighlightStore::colorName(int index)
{
    return QString::fromLatin1(kSwatches[normalizeColor(index)].name);
}

QString HighlightStore::colorHex(int index)
{
    return QString::fromLatin1(kSwatches[normalizeColor(index)].hex);
}

// Change-callback: fired after a mutation to (re)arm the debounced push; null in probes.
static std::function<void()> g_changeHook;
void HighlightStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QString HighlightStore::itemsKey()
{
    return QStringLiteral("highlights/") + profileId() + QStringLiteral("/items");
}

QString HighlightStore::tombstoneStore()
{
    return QStringLiteral("highlights/") + profileId();
}

QString HighlightStore::idFor(const QString& bookKey, const ReaderAnchor& anchor)
{
    if (bookKey.isEmpty()) return QString();
    // md5(bookKey | canonical-anchor), byte for byte the bookmark rule — the anchor canonicalised through its
    // own toJson (QJsonObject keys are sorted, so Compact bytes are stable and device-independent). The colour
    // is deliberately NOT in the seed: recolouring a passage must keep the row it already had.
    const QByteArray canon = QJsonDocument(anchor.toJson()).toJson(QJsonDocument::Compact);
    QByteArray seed = bookKey.toUtf8();
    seed.append('|');
    seed.append(canon);
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

bool HighlightStore::touches(const ReaderAnchor& a, const ReaderAnchor& b)
{
    if (!a.isRange() || !b.isRange()) return false;   // a point anchor is a bookmark, and never merges
    if (a.spine != b.spine) return false;
    // Half-open ranges [offset, endOffset). "<=" on both sides is what makes EXACTLY TOUCHING count: an end
    // that equals the next start has nothing between the two, so they are one passage on the page.
    return a.offset <= b.endOffset && b.offset <= a.endOffset;
}

HighlightStore::MergePlan HighlightStore::planMergeIn(const QVector<Highlight>& existing,
                                                     const ReaderAnchor& range)
{
    MergePlan plan;
    plan.merged = range;
    // A POINT anchor (endOffset -1) is a bookmark and is refused here — checked BEFORE the inverted-range
    // normalisation below, because -1 < offset is true for every point anchor and swapping would turn one into
    // a bogus range starting at -1 that isRange() then happily accepts.
    if (plan.merged.kind != ReaderAnchor::Book || plan.merged.endOffset < 0) return plan;
    // A selection made BACKWARDS is still a selection: the caret can legitimately end up before the anchor.
    if (plan.merged.endOffset < plan.merged.offset) qSwap(plan.merged.offset, plan.merged.endOffset);
    if (!plan.merged.isRange()) return plan;

    // Repeat until a full pass absorbs nothing: swallowing one neighbour can bring the union up against the
    // next, and a single pass would leave two adjacent rows where the page shows one band.
    QVector<bool> taken(existing.size(), false);
    bool grew = true;
    while (grew)
    {
        grew = false;
        for (int i = 0; i < existing.size(); ++i)
        {
            if (taken.at(i)) continue;
            const Highlight& h = existing.at(i);
            if (h.id.isEmpty() || !touches(plan.merged, h.anchor)) continue;
            plan.merged.offset    = qMin(plan.merged.offset, h.anchor.offset);
            plan.merged.endOffset = qMax(plan.merged.endOffset, h.anchor.endOffset);
            taken[i] = true;
            plan.absorbed << h.id;
            grew = true;
        }
    }
    return plan;
}

static ReaderAnchor anchorFromValue(const QJsonValue& v)
{
    return ReaderAnchor::fromJson(v.toObject());
}

static HighlightStore::Highlight highlightFromObject(const QJsonObject& o)
{
    HighlightStore::Highlight h;
    h.id      = o.value(QStringLiteral("id")).toString();
    h.bookKey = o.value(QStringLiteral("bookKey")).toString();
    h.anchor  = anchorFromValue(o.value(QStringLiteral("anchor")));
    h.color   = HighlightStore::normalizeColor(o.value(QStringLiteral("color")).toInt(0));
    h.text    = o.value(QStringLiteral("text")).toString();
    h.ts      = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    return h;
}

static QJsonObject highlightToObject(const HighlightStore::Highlight& h)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), h.id);
    o.insert(QStringLiteral("bookKey"), h.bookKey);
    o.insert(QStringLiteral("anchor"), h.anchor.toJson());
    o.insert(QStringLiteral("color"), h.color);
    o.insert(QStringLiteral("text"), h.text);
    o.insert(QStringLiteral("ts"), static_cast<double>(h.ts));
    return o;
}

// The whole active-profile list (all books), as stored.
static QVector<HighlightStore::Highlight> readAll()
{
    QVector<HighlightStore::Highlight> out;
    const QByteArray raw = store().value(HighlightStore::itemsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(raw).array())
        if (v.isObject())
        {
            const HighlightStore::Highlight h = highlightFromObject(v.toObject());
            if (!h.id.isEmpty()) out.push_back(h);
        }
    return out;
}

static void writeAll(const QVector<HighlightStore::Highlight>& items)
{
    QJsonArray arr;
    for (const HighlightStore::Highlight& h : items) arr.append(highlightToObject(h));
    store().setValue(HighlightStore::itemsKey(),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

static QVector<HighlightStore::Highlight> forBook(const QString& bookKey)
{
    QVector<HighlightStore::Highlight> out;
    for (const HighlightStore::Highlight& h : readAll())
        if (h.bookKey == bookKey) out.push_back(h);
    return out;
}

HighlightStore::MergePlan HighlightStore::planMerge(const QString& bookKey, const ReaderAnchor& range)
{
    if (bookKey.isEmpty())
    {
        MergePlan plan;
        plan.merged = range;
        return plan;
    }
    return planMergeIn(forBook(bookKey), range);
}

HighlightStore::Highlight HighlightStore::add(const QString& bookKey, const ReaderAnchor& anchor,
                                              int color, const QString& text)
{
    if (bookKey.isEmpty()) return Highlight();

    QVector<Highlight> items = readAll();
    QVector<Highlight> mine;
    for (const Highlight& h : items)
        if (h.bookKey == bookKey) mine.push_back(h);

    const MergePlan plan = planMergeIn(mine, anchor);
    if (!plan.merged.isRange()) return Highlight();   // a point anchor is a bookmark; this store refuses it

    Highlight hl;
    hl.id      = idFor(bookKey, plan.merged);
    hl.bookKey = bookKey;
    hl.anchor  = plan.merged;
    hl.color   = normalizeColor(color);
    hl.text    = text;
    hl.ts      = QDateTime::currentSecsSinceEpoch();

    // Everything the union swallowed goes — tombstoned, so a peer that still holds the narrower row cannot
    // bring it back and leave the reader with two overlapping bands. The union's own id survives that sweep
    // (re-adding the exact same passage is idempotent, and its tombstone is cleared below).
    QStringList gone = plan.absorbed;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == hl.id || gone.contains(items[i].id)) items.remove(i);
    items.push_back(hl);
    writeAll(items);

    for (const QString& id : gone)
        if (id != hl.id) Tombstones::record(tombstoneStore(), id);
    // A re-add of a previously-removed passage resurrects it: clear the stale tombstone so it does not
    // self-suppress on the next merge (the same "deletion undone" path bookmarks take).
    Tombstones::remove(tombstoneStore(), hl.id);

    fireChanged();
    return hl;
}

void HighlightStore::setColor(const QString& id, int color)
{
    if (id.isEmpty()) return;
    QVector<Highlight> items = readAll();
    bool changed = false;
    for (Highlight& h : items)
        if (h.id == id)
        {
            h.color = normalizeColor(color);
            h.ts    = QDateTime::currentSecsSinceEpoch();   // the newest colour is the one that wins on merge
            changed = true;
        }
    if (!changed) return;
    writeAll(items);
    fireChanged();
}

void HighlightStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<Highlight> items = readAll();
    bool removed = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == id) { items.remove(i); removed = true; }
    if (!removed) return;
    writeAll(items);
    Tombstones::record(tombstoneStore(), id);
    fireChanged();
}

QVector<HighlightStore::Highlight> HighlightStore::list(const QString& bookKey)
{
    QVector<Highlight> out;
    if (bookKey.isEmpty()) return out;
    out = forBook(bookKey);
    std::sort(out.begin(), out.end(), [](const Highlight& a, const Highlight& b) {
        return ReaderAnchor::inReadingOrder(a.anchor, b.anchor);
    });
    return out;
}

HighlightStore::Highlight HighlightStore::at(const QString& bookKey, int spine, int offset)
{
    if (bookKey.isEmpty()) return Highlight();
    for (const Highlight& h : list(bookKey))
        if (h.anchor.isRange() && h.anchor.spine == spine
            && offset >= h.anchor.offset && offset < h.anchor.endOffset)
            return h;
    return Highlight();
}

QVector<HighlightStore::Highlight> HighlightStore::all() { return readAll(); }
