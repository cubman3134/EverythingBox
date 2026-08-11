#include "LiveTvGuide.h"

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
    MediaCatalog cat;
    cat.title = sourceName.isEmpty() ? QObject::tr("Guide") : QObject::tr("%1 — Guide").arg(sourceName);

    for (const M3uEntry& e : channels)
    {
        // The channel's section header. Prefer the EPG display-name when we matched one, else the playlist
        // title — a channel is never dropped for lacking EPG data.
        QString chanName = e.title;
        if (!e.tvgId.isEmpty())
        {
            const QString dn = guide.channelNames.value(e.tvgId);
            if (!dn.isEmpty()) chanName = dn;
        }
        MediaItem hdr;
        hdr.id    = QStringLiteral("_guidehdr:") + e.url;   // url is unique per channel; keeps focus stable
        hdr.type  = QStringLiteral("_livetvheader");
        hdr.title = chanName;
        cat.items.push_back(hdr);

        // Today's programmes for this channel: those whose window overlaps the day, in start order.
        QVector<xmltv::Programme> progs = xmltv::programmesForChannel(guide, e.tvgId);
        QVector<xmltv::Programme> today;
        for (const xmltv::Programme& p : progs)
            if (p.startUtc.isValid() && p.stopUtc.isValid()
                && p.startUtc < dayEndUtc && p.stopUtc > dayStartUtc)
                today.push_back(p);
        std::sort(today.begin(), today.end(), [](const xmltv::Programme& a, const xmltv::Programme& b) {
            return a.startUtc < b.startUtc;
        });

        for (const xmltv::Programme& p : today)
        {
            const bool onAir = p.startUtc <= nowUtc && nowUtc < p.stopUtc;
            // Local wall-clock for the row label (the stored times are UTC). "HH:mm  Title", ● when on air now.
            const QString hhmm = p.startUtc.toLocalTime().toString(QStringLiteral("HH:mm"));
            MediaItem it;
            it.id       = QStringLiteral("_guideprog:") + e.url + QLatin1Char('@') + p.startUtc.toString(Qt::ISODate);
            it.type     = QStringLiteral("_guideprog");   // non-activatable, like the header (no url)
            it.title    = (onAir ? QStringLiteral("●  ") : QString()) + hhmm + QStringLiteral("  ") + p.title;
            it.subtitle = p.desc;
            cat.items.push_back(it);
        }
    }
    cat.hasMore = false;
    return cat;
}

}
