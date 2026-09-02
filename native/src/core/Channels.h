// PERSONAL TV CHANNELS — the schedule model (issue #179, increment 1).
//
// A channel is a SOURCE + an ORDERING + a START EPOCH. Given the source's items and their durations, the
// day's lineup is a pure function of those three things and the day — never of the wall clock, never of a
// store read, never of the order the app happened to load something in. That is the whole point: the guide
// on this device and the guide on the TV in the next room must agree, and they only can if the timeline is
// computed rather than remembered.
//
// EVERYTHING IN THIS HEADER IS PURE. The clock is an argument (`nowUtc`), the timezone is an argument
// (`tzOffsetSec`), the lineup is an argument. There is no QDateTime::currentDateTime() below this line, and
// probe_channels holds that: it drives a whole day of one channel by handing it seconds.
//
// ---- The four rules the schedule is built to ------------------------------------------------------------
//
// 1. DETERMINISM. The shuffle is a seeded Fisher–Yates over splitmix64, seeded by hash(channelId, dayStart).
//    Same channel + same day + same lineup -> byte-identical timeline, on every device, for ever. Nothing
//    here consults QRandomGenerator (whose global instance is seeded per process, which would make two
//    devices — and two runs on ONE device — disagree about what is on at 20:00).
//
// 2. THE DAY IS THE UNIT. A schedule covers ONE local day, [local midnight, +24h), expressed in UTC seconds.
//    A new day re-seeds, so an item added to the source shows up from the next day boundary and not before.
//
// 3. THE CURRENT DAY IS FROZEN. ScheduleCache keeps the FIRST schedule computed for a (channel, day) and
//    returns it again even when the lineup has since changed. Without that, adding an episode at 20:15 would
//    silently re-cut the evening under a viewer who is halfway through something — the guide would have lied,
//    which is the one thing a guide may not do. The cache stores the inputs' hash alongside, so a caller can
//    SEE that the live lineup has drifted from the airing one (`Schedule::inputsHash` vs `hashOfLineup`).
//
// 4. A DURATION IS A PREREQUISITE, NOT A LOOKUP. Only items whose length is already known qualify; an item
//    with no known duration is dropped from the lineup by `withDurations` and named in its `skipped` list, so
//    the caller can log it ONCE at build time. Nothing here probes a file, and nothing here may: the issue's
//    "channels are built from items we can enumerate with known durations" is a hard boundary — a channel
//    that stalls at every programme boundary is worse than no channel.
//
// The timeline is published in the SOURCE-AGNOSTIC programme model #75 already shipped (xmltv::Programme),
// so the guide grid increment 2 builds renders a computed channel and an XMLTV channel without knowing which
// is which. That is why `toProgrammes` lives here and not in the UI.
//
// QtCore-only (QString/QVector/QDateTime), no Quick, no Widgets, no disk, no network.
#pragma once
#include "XmltvGuide.h"   // the source-agnostic programme model shared with #75

#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <functional>

namespace channels
{
    // WHERE A CHANNEL'S ITEMS COME FROM. The three implemented this increment are Playlist, FilterPreset and
    // LocalFolder; AddonCatalog and ServerItems are RESERVED — their numbers are spoken for so an increment-2
    // channel written by a newer build round-trips through an older one's store instead of being rewritten as
    // a playlist. `isImplemented` is the gate the editor and the lineup builder ask.
    enum class SourceKind
    {
        Playlist     = 0,   // a saved playlist (PlaylistStore)
        FilterPreset = 1,   // a #63 saved filter preset
        LocalFolder  = 2,   // a local-library folder / series (LocalLibrary)
        AddonCatalog = 3,   // RESERVED — increment 2
        ServerItems  = 4,   // RESERVED — increment 2 (#83)
    };

    // HOW THE DAY IS ORDERED. TimeBlocked ("this block from 20:00") is RESERVED for increment 2 and is
    // deliberately given its number now, for the reason above; `isImplemented` rejects it today and
    // `orderFor` degrades it to InOrder rather than inventing a timeline nobody specified.
    enum class Ordering
    {
        InOrder    = 0,
        Shuffle    = 1,
        TimeBlocked = 2,    // RESERVED — increment 2
    };

    // One channel, as stored. `startEpoch` is the UTC second the channel went on air: before it the channel
    // has no programmes at all (a schedule for an earlier day is empty, and the day it starts begins at the
    // epoch rather than at midnight), which is what makes "created at 15:00" honest instead of retroactive.
    // `startFromBeginning` is the per-channel override for the people who hate joining mid-programme.
    // `ts` is the epoch second this row was last written — the field the multi-device merge orders by.
    struct Channel
    {
        QString    id;
        QString    name;
        SourceKind sourceKind = SourceKind::Playlist;
        QString    sourceId;                  // playlist id / preset id / series-or-folder key
        Ordering   ordering   = Ordering::Shuffle;
        qint64     startEpoch = 0;            // UTC seconds; 0 == "has always been on air"
        bool       startFromBeginning = false;
        qint64     ts = 0;
    };

    // A candidate item, BEFORE the duration gate: what the source enumerates. `playKey` is what the player is
    // handed (a local file path today); `itemId` is the stable identity used for skip reporting and equality.
    struct Candidate
    {
        QString itemId;
        QString title;
        QString playKey;
    };

    // A candidate that PASSED the duration gate. Nothing downstream can carry a zero/negative length, which
    // is what makes the timeline arithmetic total.
    struct LineupItem
    {
        QString itemId;
        QString title;
        QString playKey;
        int     durationSec = 0;   // > 0, always (withDurations is the only constructor in practice)
    };

    // One programme in the computed timeline.
    struct Slot
    {
        QString itemId;
        QString title;
        QString playKey;
        qint64  startUtc    = 0;
        int     durationSec = 0;
        qint64  endUtc() const { return startUtc + durationSec; }
    };

    // One local day of one channel.
    struct Schedule
    {
        QString        channelId;
        qint64         dayStartUtc = 0;   // local midnight, as a UTC epoch second
        quint64        inputsHash  = 0;   // hash of (channel fields, day, lineup) this timeline was cut from
        QVector<Slot>  programmes;
        bool     isEmpty() const { return programmes.isEmpty(); }
        qint64   dayEndUtc() const { return dayStartUtc + 86400; }
    };

    // "What's on now", and where in it the clock is. `offsetSec` is the JOIN POINT before the per-channel
    // override is applied; `remainingSec` is what the surfing banner counts down. `hasNext` is false at the
    // last slot of the day — the caller re-cuts tomorrow rather than this type pretending to know it.
    struct Airing
    {
        bool   valid        = false;
        Slot   current;
        int    offsetSec    = 0;
        int    remainingSec = 0;
        bool   hasNext      = false;
        Slot   next;
    };

    // A day never holds more than this many programmes. A guard, not a policy: a source whose items are one
    // second long would otherwise lay 86 400 programmes, and the arithmetic below has no other bound. Reaching it
    // truncates the day (the schedule simply ends early) rather than looping.
    inline constexpr int kMaxSlotsPerDay = 2048;

    // ---- enum <-> int, for the store and the wire ---------------------------------------------------------
    // An UNKNOWN number is NOT clamped to a neighbour, it falls back to the default — a channel written by a
    // build that has a source kind this one has never heard of must not silently become a playlist channel
    // pointing at a preset id. Round-tripping the RESERVED values is the point of them existing here.
    inline int toInt(SourceKind k) { return static_cast<int>(k); }
    inline int toInt(Ordering o)   { return static_cast<int>(o); }
    inline SourceKind sourceKindFromInt(int v)
    {
        switch (v)
        {
            case 0: return SourceKind::Playlist;
            case 1: return SourceKind::FilterPreset;
            case 2: return SourceKind::LocalFolder;
            case 3: return SourceKind::AddonCatalog;
            case 4: return SourceKind::ServerItems;
            default: return SourceKind::Playlist;   // unknown -> the default, never a neighbour (see above)
        }
    }
    inline Ordering orderingFromInt(int v)
    {
        switch (v)
        {
            case 0: return Ordering::InOrder;
            case 1: return Ordering::Shuffle;
            case 2: return Ordering::TimeBlocked;
            default: return Ordering::Shuffle;
        }
    }
    inline bool isImplemented(SourceKind k)
    { return k == SourceKind::Playlist || k == SourceKind::FilterPreset || k == SourceKind::LocalFolder; }
    inline bool isImplemented(Ordering o)
    { return o == Ordering::InOrder || o == Ordering::Shuffle; }

    // Human labels for the editor's Choice rows (translated by the caller's tr() where it matters; these are
    // the canonical spellings the probe pins so the store and the UI cannot drift).
    inline QString label(SourceKind k)
    {
        switch (k)
        {
            case SourceKind::Playlist:     return QStringLiteral("Playlist");
            case SourceKind::FilterPreset: return QStringLiteral("Saved filter");
            case SourceKind::LocalFolder:  return QStringLiteral("Local series or folder");
            case SourceKind::AddonCatalog: return QStringLiteral("Catalogue");
            case SourceKind::ServerItems:  return QStringLiteral("Server");
        }
        return QStringLiteral("Playlist");
    }
    inline QString label(Ordering o)
    {
        switch (o)
        {
            case Ordering::InOrder:     return QStringLiteral("In order");
            case Ordering::Shuffle:     return QStringLiteral("Shuffle");
            case Ordering::TimeBlocked: return QStringLiteral("Time blocks");
        }
        return QStringLiteral("Shuffle");
    }

    // ---- the row-producer NAME (#161 coordination) --------------------------------------------------------
    // A channel is addressable as a home-row producer under "channel:<id>". #161 names this key; nothing here
    // depends on that feature's code. Also the identity a channel is favourited and re-opened under, so the
    // Recents route and the home row cannot drift apart on the spelling.
    // INLINE, deliberately: every browse builder that renders a channel row spells this key, and making them
    // link the whole schedule (and, through it, the XMLTV programme model and its gunzip) to say "channel:" +
    // an id would put miniz into half a dozen probe targets that have nothing to do with a timeline.
    inline QLatin1String channelKeyPrefix() { return QLatin1String("channel:"); }
    inline QString rowProducerKey(const QString& channelId)
    { return channelId.isEmpty() ? QString() : channelKeyPrefix() + channelId; }
    inline bool isRowProducerKey(const QString& key)
    { return key.startsWith(channelKeyPrefix()) && key.size() > channelKeyPrefix().size(); }
    inline QString channelIdFromKey(const QString& key)   // "" when `key` is not a channel key
    { return isRowProducerKey(key) ? key.mid(channelKeyPrefix().size()) : QString(); }

    // ---- the pure schedule --------------------------------------------------------------------------------

    // Local midnight for the day `nowUtc` falls in, as a UTC epoch second. `tzOffsetSec` is the local zone's
    // offset from UTC in seconds (QDateTime::offsetFromUtc()). Floors correctly for negative epochs and for
    // west-of-UTC offsets, which a plain integer division does not.
    qint64 dayStartUtc(qint64 nowUtc, int tzOffsetSec);

    // The shuffle seed: hash(channelId, dayStart). Stable across processes, devices and Qt versions — it is a
    // fixed FNV-1a/splitmix64 mix over the id's UTF-8 bytes, not qHash (which is per-process salted).
    quint64 seedFor(const QString& channelId, qint64 dayStart);

    // The duration gate. Keeps every candidate whose `durationFor(candidate)` is > 0, in input order; every
    // other candidate's itemId is appended to `skipped` (when non-null) so the caller logs it ONCE, at build
    // time, and never asks again while drawing a guide.
    //
    // THE LOOKUP TAKES THE WHOLE CANDIDATE, not its play key, and that is not tidiness. A live drive found the
    // gate skipping every item of a channel whose files the app had already played: the duration index is
    // keyed by the item's DURABLE IDENTITY (what PlaybackSession resumes under — "local:<path>" for a local
    // video, an addon item id for a catalogue row), and the play key is the file path. Asking with one string
    // meant asking with the wrong one, and the failure looked exactly like "nothing here has a known length".
    // A candidate carries both; which of them the index answers to is the index's business, not the gate's.
    QVector<LineupItem> withDurations(const QVector<Candidate>& candidates,
                                      const std::function<int(const Candidate&)>& durationFor,
                                      QStringList* skipped = nullptr);

    // A hash of the lineup as it stands — the value stored in Schedule::inputsHash. Two lineups that differ in
    // any item, title, length or order hash differently; the same lineup hashes the same on every device.
    quint64 hashOfLineup(const Channel& ch, qint64 dayStart, const QVector<LineupItem>& lineup);

    // The day's play order as a permutation of [0, n). InOrder is the identity; Shuffle is a seeded
    // Fisher–Yates; TimeBlocked (reserved) currently degrades to the identity.
    QVector<int> orderFor(const Channel& ch, qint64 dayStart, int n);

    // Cut one local day. `dayStart` must be a value dayStartUtc() produced. The day begins at
    // max(dayStart, ch.startEpoch) — a channel that went on air at 15:00 is off-air before it, on its first
    // day and on every earlier one — and programmes are laid end to end, repeating the permutation, until the day
    // is covered or kMaxSlotsPerDay is reached.
    Schedule buildDay(const Channel& ch, qint64 dayStart, const QVector<LineupItem>& lineup);

    // What is on at `nowUtc`, and where in it. The window is HALF-OPEN, [start, start+duration): at the exact
    // second a programme starts, that programme is on with offset 0 and its predecessor is over. Outside the
    // schedule's day, or in a gap before the channel goes on air, `valid` is false.
    Airing whatsOn(const Schedule& s, qint64 nowUtc);

    // Where playback actually starts: the airing's offset, or 0 when the channel is set to start programmes
    // from the beginning. The ONE place the override is applied, so the tuner and the banner agree.
    int joinOffsetSec(const Airing& a, bool startFromBeginning);

    // The timeline in #75's source-agnostic programme model, ready for the guide grid increment 2 builds.
    QVector<xmltv::Programme> toProgrammes(const Schedule& s);

    // ---- surfing ------------------------------------------------------------------------------------------

    // Up/Down over a wrapped channel list. `delta` is -1 (up) or +1 (down); any other value is clamped to its
    // sign. Returns -1 for an empty list, so a caller cannot index into nothing.
    int surfIndex(int count, int index, int delta);

    // The channels whose CURRENT item should be prefetched while `tunedId` is on: its immediate neighbours and
    // nothing else. BOUNDED TO ±1 BY CONSTRUCTION — the return is at most two ids, deduplicated, and never
    // contains `tunedId`. A one- or two-channel list therefore yields fewer than two, which is the point: a
    // wrapped neighbour that IS the tuned channel is not a neighbour.
    QStringList prefetchNeighbours(const QStringList& channelIds, const QString& tunedId);

    // ---- the frozen-day cache -----------------------------------------------------------------------------
    // Rule 3 above, as an object. Per channel it holds the day it cut and the schedule it cut; a call for the
    // SAME day returns that schedule again and ignores the lineup it was handed, so today's guide cannot move
    // under a viewer. A call for a different day re-cuts and replaces.
    class ScheduleCache
    {
    public:
        Schedule dayFor(const Channel& ch, qint64 nowUtc, int tzOffsetSec,
                        const QVector<LineupItem>& lineup);

        // Was the last dayFor() for this channel served from the frozen copy (rather than freshly cut)?
        // Exposed for the probe and for a caller that wants to say "today's lineup is already airing".
        bool servedFrozen() const { return servedFrozen_; }

        // Has the live lineup drifted from the one today's airing timeline was cut from? A caller may show
        // "new episodes air from tomorrow"; nothing here acts on it.
        bool driftedFrom(const Channel& ch, qint64 nowUtc, int tzOffsetSec,
                         const QVector<LineupItem>& lineup) const;

        void forget(const QString& channelId);
        void clear();
        int  size() const { return byChannel_.size(); }

    private:
        struct Entry { qint64 dayStart = 0; Schedule sched; };
        QHash<QString, Entry> byChannel_;
        bool servedFrozen_ = false;
    };
}
