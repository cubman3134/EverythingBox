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

    // The mark a cell that is ON AIR carries at the front of its title. Exposed because the builder writes it
    // and guideNowIndex reads it, and two spellings of one marker is how a grid starts disagreeing with
    // itself about what is on.
    QString  guideOnAirMarker();

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

    // WHERE A GUIDE SHOULD OPEN (issue #308). The grid is a flat list of the whole day per channel, so
    // opening it at 21:00 used to mean scrolling past twenty-one hours to reach what is actually on. This
    // answers "which row is now", as an index into `cat.items`, and the surface lands the cursor there.
    //
    // Read strictly in this order, and the FIRST CHANNEL SECTION that answers at all wins — so the cursor
    // stays at the top of the guide unless the first channel has nothing to say about this moment:
    //
    //   1. The cell marked ON AIR. That is the answer the issue asks for, and it is the same mark the grid
    //      prints, so the cursor cannot land somewhere the grid does not say is on.
    //   2. Failing that, the next cell DUE TO START. This is the deliberate answer to the bumper case: while
    //      an interstitial is airing no cell carries the mark, correctly — the guide is a contract about
    //      programmes and a bumper is not one — so there is no "now" row to land on. Landing on what is
    //      about to start is what a bumper actually means, it is a real row rather than an invented one, and
    //      it never claims something is on when it is not. The same rule covers a channel between programmes
    //      and one whose day has not begun.
    //   3. -1 when nothing in the grid answers either question (every channel off air, or a day entirely in
    //      the past). The caller leaves the cursor where it would have been — the top — rather than guessing.
    //
    // Rule 1 reads THE MARK THE GRID PRINTED rather than recomputing what is on, which is what makes "the
    // cursor cannot land on something the guide does not say is on" true by construction rather than by two
    // calculations happening to agree. `nowUtc` therefore only decides rule 2, and callers pass the same
    // clock the grid was cut with (HomeView::populateChannelGuide reads it once and uses it for both).
    //
    // Pure, and clock-as-an-argument like everything else in this feature.
    int guideNowIndex(const MediaCatalog& cat, const QDateTime& nowUtc);
}
