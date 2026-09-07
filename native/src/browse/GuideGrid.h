// THE EPG GRID — one builder, two suppliers (issue #75 increment 3 + issue #179 increment 2).
//
// #75 shipped a channels x time grid for XMLTV, written against the SOURCE-AGNOSTIC programme model
// (xmltv::Programme) precisely so that #179's computed schedules could be poured into the same grid. This
// file is where that promise is kept: the grid builder was lifted out of LiveTvGuide.cpp unchanged and given
// its data as a plain list of channels-with-programmes, so neither supplier is the special one.
//
//   * Live TV passes its M3U entries matched against a parsed XMLTV guide (browse/LiveTvGuide.cpp).
//   * A personal channel passes its computed day (browse/ChannelGuide.cpp).
//
// The row shapes are BYTE-IDENTICAL to what #75 produced — probe_xmltv still asserts them against the
// LiveTvGuide entry point — because a grid that renders differently depending on who filled it is two grids
// wearing one name.
//
// NO XMLTV DEPENDENCY. Only the Programme STRUCT is used; nothing here parses, gunzips or looks a channel up,
// so this translation unit links without XmltvGuide.cpp or miniz and a probe that only wants the grid does
// not inherit a decompressor. That is the same discipline LiveTvGuide.h documents for itself.
#pragma once
#include "../addons/AddonModels.h"   // MediaCatalog / MediaItem
#include "../core/XmltvGuide.h"      // xmltv::Programme  (the model only — no parser is called)
#include <QDateTime>
#include <QString>
#include <QVector>

namespace browse
{
    // One row of the grid, already resolved: what the channel is called, what art it has (empty is
    // unremarkable), and the programmes it is offering. `key` is the channel's DURABLE identity and is what
    // the row ids are built from — a stream url for Live TV, "channel:<id>" for a personal channel — so
    // focus survives a rebuild and an activated cell can name the channel it came from.
    struct GuideChannel
    {
        QString key;
        QString name;
        QString logo;                          // tile art for the header row; empty = none, which is fine
        QString note;                          // header subtitle: why a channel has no programmes today, if so
        QVector<xmltv::Programme> programmes;  // any order; the builder sorts and filters to the day
    };

    // The id a programme cell carries: "_guideprog:<channel key>@<ISO-8601 UTC start>". The '@' is found from
    // the RIGHT, because a channel key may contain one (an M3U url with credentials in it does) and an
    // ISO-8601 stamp never can.
    QString  guideCellId(const QString& channelKey, const QDateTime& startUtc);
    bool     parseGuideCellId(const QString& rowId, QString& channelKey, QDateTime& startUtc);

    // THE GRID. One section per channel — a non-activatable "_livetvheader" row carrying its name and logo —
    // followed by that channel's programmes whose window overlaps [dayStartUtc, dayEndUtc), each titled
    // "HH:mm  Title" in LOCAL time with the description as its subtitle and a leading "●  " on whichever one
    // is on air at `nowUtc`. A channel with no programmes for the day still gets its header, so the grid
    // never silently drops a channel.
    //
    // `cellType` is the MediaItem::type the programme rows carry and `cellMimePrefix` their mime:
    //   * Live TV passes ("_guideprog", "") — inert rows, exactly what #75 shipped.
    //   * A personal channel passes ("_guidetune", "guidetune:") — the cell is pressable and its mime is
    //     "guidetune:<channel key>@<ISO start>", which is the id minus the "_guideprog:" prefix, so the same
    //     parse reads either one.
    // Both are REQUIRED rather than defaulted: which kind of grid this is is the caller's decision to state.
    // Pure — no store, no network, no clock of its own.
    MediaCatalog guideGridCatalog(const QString& title, const QVector<GuideChannel>& channels,
                                  const QDateTime& nowUtc,
                                  const QDateTime& dayStartUtc, const QDateTime& dayEndUtc,
                                  const QString& cellType, const QString& cellMimePrefix);
}
