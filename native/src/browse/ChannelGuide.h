// PERSONAL TV CHANNELS IN THE SHARED GUIDE GRID (issue #179, increment 2) — the second supplier.
//
// Everything here is a PROJECTION. The timeline is computed by core/Channels (pure, deterministic, clock as
// an argument); this file turns one already-cut day per channel into the browse::GuideChannel rows the one
// grid builder renders, and turns an activated cell back into "which channel, and which second". It computes
// no schedule of its own, and it must not: if this file could decide what is on at 20:00 there would be two
// answers to that question, and the guide would be free to disagree with the tuner.
//
// THE AGREEMENT THIS EXISTS TO KEEP: what the grid shows for a channel at time T is exactly what tuning that
// channel at time T plays, including the offset. The cell carries the programme's START SECOND, so the claim
// is checkable rather than merely intended — probe_channels drives several channels across several times and
// asserts the cell and channels::whatsOn agree item for item and second for second.
//
// Its own translation unit for LiveTvGuide.h's reason, one supplier along: only the app and probe_channels
// need a schedule AND a grid, so the Live TV probes do not inherit core/Channels and the grid builder does
// not inherit the schedule model.
#pragma once
#include "GuideGrid.h"
#include "../core/Channels.h"
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

namespace browse
{
    // The grid for the personal channels. `days` is parallel to `channels` — each entry is that channel's
    // schedule for the day being drawn, exactly as the tuner would cut it (ScheduleCache::dayFor), bumpers
    // included or not: interstitial slots are dropped by channels::toProgrammes and never reach the grid.
    // `logoByChannelId` is art keyed by CHANNEL ID (not by row key); a channel with no art is unremarkable.
    //
    // A channel with an empty schedule keeps its header row and is given a plain-English note instead of
    // programmes — "nothing with a known length yet" for a lineup the duration gate emptied, "off air today"
    // for one whose day holds no programmes. It is never silently dropped and nothing is opened to find out.
    MediaCatalog channelGuideCatalog(const QVector<channels::Channel>& channels,
                                     const QVector<channels::Schedule>& days,
                                     const QHash<QString, QString>& logoByChannelId,
                                     const QDateTime& nowUtc,
                                     const QDateTime& dayStartUtc, const QDateTime& dayEndUtc);

    // An activated grid cell -> the channel it belongs to and the second its programme starts. Accepts the
    // row's mime ("guidetune:channel:<id>@<iso>") or its id ("_guideprog:channel:<id>@<iso>"). False for
    // anything else, including a Live TV cell — those are inert and must not route here.
    bool parseChannelGuideCell(const QString& mimeOrId, QString& channelId, qint64& cellStartUtc);
}
