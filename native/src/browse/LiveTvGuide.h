// Live TV EPG display builders (#75, increment 3) — the bridge from the source-agnostic programme model
// (xmltv::Programme, shared with #179) to the browse MediaCatalog the UI renders.
//
// Kept in its OWN translation unit rather than in SyntheticCatalogs.cpp on purpose: only the app and this
// feature's probe link the XMLTV parser, so the existing SyntheticCatalogs probes (probe_browse / probe_iptv /
// probe_locallib …) do NOT pick up an XmltvGuide.cpp/miniz dependency they have no use for. The now/next
// strings the channel LIST shows are passed into liveTvChannelsCatalog as a plain QHash<tvg-id, text>, so that
// builder stays free of any EPG type — the two files never have to be linked together.
#pragma once
#include "../addons/AddonModels.h"      // MediaCatalog / MediaItem
#include "../core/XmltvGuide.h"          // xmltv::Guide / Programme  (the source-agnostic model)
#include "../media/StreamResolver.h"     // M3uEntry (the channel, carrying its tvg-id)
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

namespace browse
{
    // For each channel with a tvg-id that matches the guide, its now/next one-liner ("Now: X · Next: Y"),
    // keyed by tvg-id. Channels with no match are simply absent from the map (the list then shows the group,
    // today's behaviour). Pure: guide + channels + now in, map out — no network, no store.
    QHash<QString, QString> liveTvNowNextByTvgId(const QVector<M3uEntry>& channels,
                                                 const xmltv::Guide& guide, const QDateTime& nowUtc);

    // A SIMPLE guide grid for today: one section per channel (a "_livetvheader" row carrying the channel name),
    // followed by that channel's programmes whose window overlaps [dayStartUtc, dayEndUtc) — each a
    // non-activatable "_guideprog" row titled "HH:mm  Title" (local time) with the description as its subtitle,
    // and the programme on air at `nowUtc` marked with a leading ● . Channels are listed in the ORDER given
    // (playlist order); a channel with no matching programmes for the day is still shown (header only), so the
    // grid never silently drops a channel. Built against the source-agnostic Programme model, so #179 supplies
    // the same shape from a computed schedule. Pure.
    MediaCatalog liveTvGuideCatalog(const QString& sourceName, const QVector<M3uEntry>& channels,
                                    const xmltv::Guide& guide, const QDateTime& nowUtc,
                                    const QDateTime& dayStartUtc, const QDateTime& dayEndUtc);
}
