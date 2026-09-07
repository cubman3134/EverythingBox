#include "LiveTvGuide.h"
#include "GuideGrid.h"      // the ONE grid builder both suppliers feed (#75 inc 3 / #179 inc 2)

#include <QObject>
#include <algorithm>

namespace browse
{

QHash<QString, QString> liveTvNowNextByTvgId(const QVector<M3uEntry>& channels,
                                             const xmltv::Guide& guide, const QDateTime& nowUtc)
{
    QHash<QString, QString> out;
    for (const M3uEntry& e : channels)
    {
        if (e.tvgId.isEmpty() || out.contains(e.tvgId)) continue;   // one lookup per distinct tvg-id
        const QVector<xmltv::Programme> progs = xmltv::programmesForChannel(guide, e.tvgId);
        if (progs.isEmpty()) continue;
        const xmltv::NowNext nn = xmltv::nowNext(progs, nowUtc);
        if (!nn.hasCurrent && !nn.hasNext) continue;                // matched a channel but nothing airing/ahead

        QString text;
        if (nn.hasCurrent) text = QObject::tr("Now: %1").arg(nn.current.title);
        if (nn.hasNext)
        {
            const QString nextPart = QObject::tr("Next: %1").arg(nn.next.title);
            text = text.isEmpty() ? nextPart : text + QStringLiteral(" · ") + nextPart;   // "Now … · Next …"
        }
        out.insert(e.tvgId, text);
    }
    return out;
}

MediaCatalog liveTvGuideCatalog(const QString& sourceName, const QVector<M3uEntry>& channels,
                                const xmltv::Guide& guide, const QDateTime& nowUtc,
                                const QDateTime& dayStartUtc, const QDateTime& dayEndUtc)
{
    // THE ADAPTER, not a second grid (#179 inc 2). All this does is answer the shared builder's question —
    // "which channels, called what, offering what?" — from an M3U playlist matched against a parsed EPG. The
    // rows it produces are byte-identical to the ones #75 shipped; probe_xmltv still asserts them here.
    QVector<GuideChannel> rows;
    rows.reserve(channels.size());
    for (const M3uEntry& e : channels)
    {
        GuideChannel gc;
        gc.key = e.url;                     // unique per channel; keeps focus stable across a rebuild
        // Prefer the EPG display-name when we matched one, else the playlist title — a channel is never
        // dropped, nor renamed to nothing, for lacking EPG data.
        gc.name = e.title;
        if (!e.tvgId.isEmpty())
        {
            const QString dn = guide.channelNames.value(e.tvgId);
            if (!dn.isEmpty()) gc.name = dn;
        }
        gc.programmes = xmltv::programmesForChannel(guide, e.tvgId);
        rows.push_back(gc);
    }
    const QString title = sourceName.isEmpty() ? QObject::tr("Guide")
                                               : QObject::tr("%1 — Guide").arg(sourceName);
    // Inert cells: a Live TV programme is not something you can tune to on its own (the CHANNEL is), so it
    // carries no mime and the type stays the non-activatable one.
    return guideGridCatalog(title, rows, nowUtc, dayStartUtc, dayEndUtc,
                            QStringLiteral("_guideprog"), QString());
}

}
