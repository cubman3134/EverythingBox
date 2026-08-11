// XMLTV EPG parsing + a SOURCE-AGNOSTIC programme model (#75, increment 3).
//
// The programme model below is deliberately free of anything XMLTV- or IPTV-specific: a channel key, a UTC
// start/stop window, a title and a description. That is the shared infrastructure #179 (personal TV channels
// built from your own library) reuses — it populates the SAME `Programme`/`Guide` from a COMPUTED schedule
// instead of an XMLTV feed, and the guide grid renders either without knowing where the data came from.
//
// QtCore-only (QXmlStreamReader + QDateTime), no disk and no network: fetching, gunzip and caching live in the
// caller (HomeView). `gunzip` is the one exception — it inflates a gzip buffer via the vendored miniz — but it
// is a pure bytes->bytes transform with no I/O, so it stays here beside the parser it feeds.
//
// Discipline (parseManifest style): malformed or partial input yields whatever parsed and NEVER throws.
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

namespace xmltv
{
    // One programme, in the source-agnostic model shared with #179. `channelId` is the XMLTV channel id here
    // (matched against an M3U entry's tvg-id); for #179 it is whatever key that feature's schedule uses.
    // Times are UTC — parseXmltvTime() has already folded the feed's zone offset out.
    struct Programme
    {
        QString   channelId;
        QDateTime startUtc;
        QDateTime stopUtc;
        QString   title;
        QString   desc;
    };

    // A parsed guide: every programme (all channels, in document order) plus the channel-id -> display-name map
    // read from the <channel> elements. Keeping the two separate lets the display name be shown even for a
    // channel that currently has no programmes, and lets a programme lookup ignore names entirely.
    struct Guide
    {
        QVector<Programme>      programmes;
        QHash<QString, QString> channelNames;   // channel id -> first <display-name>
    };

    // The now/next selection for ONE channel. `hasCurrent`/`hasNext` disambiguate "no programme" from a
    // default-constructed Programme (a channel off-air right now still has a valid `next`).
    struct NowNext
    {
        Programme current;
        Programme next;
        bool      hasCurrent = false;
        bool      hasNext    = false;
    };

    // gzip magic (0x1f 0x8b, deflate method) -> inflate the body; anything else (plain XML, truncated, empty)
    // is returned UNCHANGED so a .xml and a .xml.gz feed take the same path. Never throws; a corrupt gzip
    // stream degrades to the input bytes rather than an empty result.
    QByteArray gunzip(const QByteArray& data);

    // Parse an XMLTV timestamp "YYYYMMDDHHMMSS [+-]ZZZZ" -> UTC QDateTime. The zone offset is OPTIONAL: with it,
    // the wall-clock is converted to UTC (a "+0100" stamp reads one hour EARLIER in UTC); without it the stamp
    // is taken as already-UTC. A malformed stamp yields a null (invalid) QDateTime.
    QDateTime parseXmltvTime(const QString& s);

    // Parse a whole XMLTV document. Pulls <channel id><display-name> and <programme channel start stop><title>/
    // <desc>. Partial/malformed input keeps whatever was read before the error (never throws).
    Guide parseXmltv(const QByteArray& xml);

    // The programmes for one channel id, in document order. The match is exact but ASCII-case-insensitive: some
    // playlists carry "CNN.us" against an EPG channel id of "cnn.us", and folding case rescues that common case
    // without the risk a looser (substring/normalised) match would bring. Empty tvgId -> no programmes.
    QVector<Programme> programmesForChannel(const Guide& g, const QString& tvgId);

    // now/next over ONE channel's programmes (typically programmesForChannel's result): `current` is the
    // programme whose [start, stop) window contains `nowUtc`; `next` is the earliest programme starting strictly
    // after `nowUtc`. Pure and order-independent — the input need not be sorted. Empty input, or a `nowUtc`
    // outside every window with nothing ahead, yields hasCurrent/hasNext = false.
    NowNext nowNext(const QVector<Programme>& forOneChannel, const QDateTime& nowUtc);
}
