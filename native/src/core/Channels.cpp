#include "Channels.h"

#include <QDateTime>

namespace
{
// splitmix64 — a fixed, standard, endian-independent 64-bit mixer. Chosen over qHash deliberately: qHash is
// SALTED PER PROCESS (QHashSeed), so a shuffle seeded from it would differ between two runs of the same build
// on the same machine, let alone between devices. Everything about the lineup has to be reproducible, so the
// mixing function is spelled out here and never changed.
quint64 splitmix64(quint64& x)
{
    x += 0x9E3779B97F4A7C15ULL;
    quint64 z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// FNV-1a over bytes. Same reasoning: a fixed function, not a Qt one.
quint64 fnv1a(quint64 h, const QByteArray& b)
{
    for (char c : b)
    {
        h ^= static_cast<quint64>(static_cast<unsigned char>(c));
        h *= 0x100000001B3ULL;
    }
    return h;
}

quint64 fnv1a(quint64 h, const QString& s) { return fnv1a(h, s.toUtf8()); }

quint64 fnv1aNum(quint64 h, qint64 v)
{
    for (int i = 0; i < 8; ++i)
    {
        h ^= static_cast<quint64>((static_cast<quint64>(v) >> (i * 8)) & 0xFF);
        h *= 0x100000001B3ULL;
    }
    return h;
}

constexpr quint64 kFnvOffset = 0xCBF29CE484222325ULL;

// Floor division that is correct for negative numerators (a/b in C++ truncates towards zero, which would put
// 1969-12-31 23:00 UTC in the same "day" as 1970-01-01 00:00).
qint64 floorDiv(qint64 a, qint64 b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }
} // namespace

namespace channels
{

// ---- the pure schedule -----------------------------------------------------------------------------------

qint64 dayStartUtc(qint64 nowUtc, int tzOffsetSec)
{
    const qint64 local = nowUtc + tzOffsetSec;
    return floorDiv(local, 86400) * 86400 - tzOffsetSec;
}

quint64 seedFor(const QString& channelId, qint64 dayStart)
{
    quint64 h = fnv1a(kFnvOffset, channelId);
    h = fnv1aNum(h, dayStart);
    // One splitmix pass so two ids that differ in one byte do not produce correlated first draws.
    return splitmix64(h);
}

QVector<LineupItem> withDurations(const QVector<Candidate>& candidates,
                                  const std::function<int(const Candidate&)>& durationFor,
                                  QStringList* skipped)
{
    QVector<LineupItem> out;
    out.reserve(candidates.size());
    for (const Candidate& c : candidates)
    {
        // No duration lookup at all is not "everything qualifies" — it is "nothing does". A caller that
        // forgot to supply the index must get an empty channel and a skip list, not a lineup of zero-length
        // programmes that would divide the day into nothing.
        const int d = durationFor ? durationFor(c) : 0;
        if (d <= 0)
        {
            if (skipped) skipped->append(c.itemId.isEmpty() ? c.playKey : c.itemId);
            continue;
        }
        LineupItem li;
        li.itemId      = c.itemId;
        li.title       = c.title;
        li.playKey     = c.playKey;
        li.durationSec = d;
        out.push_back(li);
    }
    return out;
}

quint64 hashOfLineup(const Channel& ch, qint64 dayStart, const QVector<LineupItem>& lineup)
{
    quint64 h = fnv1a(kFnvOffset, ch.id);
    h = fnv1a(h, ch.sourceId);
    h = fnv1aNum(h, toInt(ch.sourceKind));
    h = fnv1aNum(h, toInt(ch.ordering));
    h = fnv1aNum(h, ch.startEpoch);
    h = fnv1aNum(h, dayStart);
    h = fnv1aNum(h, lineup.size());
    for (const LineupItem& li : lineup)
    {
        h = fnv1a(h, li.itemId);
        h = fnv1a(h, li.title);
        h = fnv1a(h, li.playKey);
        h = fnv1aNum(h, li.durationSec);
    }
    return h;
}

QVector<int> orderFor(const Channel& ch, qint64 dayStart, int n)
{
    QVector<int> idx;
    if (n <= 0) return idx;
    idx.reserve(n);
    for (int i = 0; i < n; ++i) idx.push_back(i);
    if (ch.ordering != Ordering::Shuffle) return idx;   // InOrder, and (reserved) TimeBlocked, keep source order

    // Fisher–Yates, from the back, over a seeded splitmix64. The modulo bias at n <= a few thousand is far
    // below anything a viewer could perceive, and an unbiased rejection loop would make the number of PRNG
    // draws depend on the values drawn — which is fine for a shuffle but makes the sequence harder to reason
    // about across future changes. Determinism is the property being protected here, not uniformity.
    quint64 state = seedFor(ch.id, dayStart);
    for (int i = n - 1; i > 0; --i)
    {
        const quint64 r = splitmix64(state);
        const int j = static_cast<int>(r % static_cast<quint64>(i + 1));
        idx.swapItemsAt(i, j);
    }
    return idx;
}

Schedule buildDay(const Channel& ch, qint64 dayStart, const QVector<LineupItem>& lineup)
{
    Schedule s;
    s.channelId   = ch.id;
    s.dayStartUtc = dayStart;
    s.inputsHash  = hashOfLineup(ch, dayStart, lineup);
    if (lineup.isEmpty()) return s;

    // A channel is OFF AIR before its start epoch — on the day it was created it begins at the epoch, and on
    // any earlier day it has no programmes at all. Nothing is retroactive: a channel made this afternoon does
    // not claim to have been broadcasting all morning.
    const qint64 dayEnd  = s.dayEndUtc();
    const qint64 airFrom = qMax(dayStart, ch.startEpoch);
    if (airFrom >= dayEnd) return s;

    const QVector<int> perm = orderFor(ch, dayStart, lineup.size());
    if (perm.isEmpty()) return s;

    qint64 t = airFrom;
    int step = 0;
    while (t < dayEnd && s.programmes.size() < kMaxSlotsPerDay)
    {
        const LineupItem& li = lineup.at(perm.at(step % perm.size()));
        Slot sl;
        sl.itemId      = li.itemId;
        sl.title       = li.title;
        sl.playKey     = li.playKey;
        sl.startUtc    = t;
        sl.durationSec = li.durationSec;
        s.programmes.push_back(sl);
        t += li.durationSec;   // > 0 by withDurations' gate, so this loop always advances
        ++step;
    }
    return s;
}

Airing whatsOn(const Schedule& s, qint64 nowUtc)
{
    Airing a;
    for (int i = 0; i < s.programmes.size(); ++i)
    {
        const Slot& sl = s.programmes.at(i);
        // HALF-OPEN. At sl.startUtc exactly, sl is on with offset 0; at sl.endUtc() exactly, the NEXT one is.
        // The boundary second belongs to the programme that is starting, which is what a clock says and what
        // the guide draws.
        if (nowUtc < sl.startUtc) break;             // programmes are laid in ascending order — nothing later can match
        if (nowUtc >= sl.endUtc()) continue;
        a.valid        = true;
        a.current      = sl;
        a.offsetSec    = static_cast<int>(nowUtc - sl.startUtc);
        a.remainingSec = sl.durationSec - a.offsetSec;
        if (i + 1 < s.programmes.size()) { a.hasNext = true; a.next = s.programmes.at(i + 1); }
        break;
    }
    return a;
}

int joinOffsetSec(const Airing& a, bool startFromBeginning)
{
    if (!a.valid) return 0;
    return startFromBeginning ? 0 : a.offsetSec;
}

QVector<xmltv::Programme> toProgrammes(const Schedule& s)
{
    QVector<xmltv::Programme> out;
    out.reserve(s.programmes.size());
    for (const Slot& sl : s.programmes)
    {
        xmltv::Programme p;
        p.channelId = rowProducerKey(s.channelId);
        p.startUtc  = QDateTime::fromSecsSinceEpoch(sl.startUtc, Qt::UTC);
        p.stopUtc   = QDateTime::fromSecsSinceEpoch(sl.endUtc(), Qt::UTC);
        p.title     = sl.title;
        out.push_back(p);
    }
    return out;
}

// ---- surfing ---------------------------------------------------------------------------------------------

int surfIndex(int count, int index, int delta)
{
    if (count <= 0) return -1;
    const int step = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    const int n = ((index + step) % count + count) % count;
    return n;
}

QStringList prefetchNeighbours(const QStringList& channelIds, const QString& tunedId)
{
    QStringList out;
    const int idx = channelIds.indexOf(tunedId);
    if (idx < 0 || channelIds.isEmpty()) return out;
    // ±1 AND NO MORE. The bound is structural — two candidate offsets, not a loop with a cap that a later
    // edit could raise by changing a constant.
    for (int d : { -1, 1 })
    {
        const int n = surfIndex(channelIds.size(), idx, d);
        if (n < 0) continue;
        const QString& id = channelIds.at(n);
        if (id == tunedId || out.contains(id)) continue;   // a wrapped neighbour that is US is not a neighbour
        out.append(id);
    }
    return out;
}

// ---- the frozen-day cache --------------------------------------------------------------------------------

Schedule ScheduleCache::dayFor(const Channel& ch, qint64 nowUtc, int tzOffsetSec,
                               const QVector<LineupItem>& lineup)
{
    const qint64 day = dayStartUtc(nowUtc, tzOffsetSec);
    auto it = byChannel_.find(ch.id);
    if (it != byChannel_.end() && it->dayStart == day)
    {
        // FROZEN. The lineup argument is deliberately NOT consulted: today's timeline is the one already
        // airing, and re-cutting it because an episode was added at 20:15 would move a programme under a
        // viewer who is halfway through it (see the header, rule 3).
        servedFrozen_ = true;
        return it->sched;
    }
    Entry e;
    e.dayStart = day;
    e.sched    = buildDay(ch, day, lineup);
    byChannel_.insert(ch.id, e);
    servedFrozen_ = false;
    return e.sched;
}

bool ScheduleCache::driftedFrom(const Channel& ch, qint64 nowUtc, int tzOffsetSec,
                                const QVector<LineupItem>& lineup) const
{
    const qint64 day = dayStartUtc(nowUtc, tzOffsetSec);
    auto it = byChannel_.constFind(ch.id);
    if (it == byChannel_.constEnd() || it->dayStart != day) return false;   // nothing frozen -> nothing to drift from
    return it->sched.inputsHash != hashOfLineup(ch, day, lineup);
}

void ScheduleCache::forget(const QString& channelId) { byChannel_.remove(channelId); }
void ScheduleCache::clear() { byChannel_.clear(); servedFrozen_ = false; }

} // namespace channels
