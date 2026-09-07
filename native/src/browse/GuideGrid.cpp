#include "GuideGrid.h"

#include <QObject>
#include <algorithm>

namespace browse
{

static QLatin1String cellIdPrefix() { return QLatin1String("_guideprog:"); }

QString guideCellId(const QString& channelKey, const QDateTime& startUtc)
{
    return cellIdPrefix() + channelKey + QLatin1Char('@') + startUtc.toUTC().toString(Qt::ISODate);
}

bool parseGuideCellId(const QString& rowId, QString& channelKey, QDateTime& startUtc)
{
    // Accepts either the row ID ("_guideprog:<key>@<iso>") or the tune MIME ("guidetune:<key>@<iso>") — the
    // two differ only in a prefix, and a caller holding one should not have to know which it holds.
    int from = 0;
    if (rowId.startsWith(cellIdPrefix()))                     from = cellIdPrefix().size();
    else if (rowId.startsWith(QLatin1String("guidetune:")))   from = QLatin1String("guidetune:").size();
    else return false;
    const int at = rowId.lastIndexOf(QLatin1Char('@'));       // from the RIGHT: the key may hold an '@'
    if (at <= from) return false;
    const QDateTime t = QDateTime::fromString(rowId.mid(at + 1), Qt::ISODate);
    if (!t.isValid()) return false;
    channelKey = rowId.mid(from, at - from);
    startUtc   = t.toUTC();
    return !channelKey.isEmpty();
}

MediaCatalog guideGridCatalog(const QString& title, const QVector<GuideChannel>& channels,
                              const QDateTime& nowUtc,
                              const QDateTime& dayStartUtc, const QDateTime& dayEndUtc,
                              const QString& cellType, const QString& cellMimePrefix)
{
    MediaCatalog cat;
    cat.title = title;

    for (const GuideChannel& ch : channels)
    {
        MediaItem hdr;
        hdr.id           = QStringLiteral("_guidehdr:") + ch.key;   // unique per channel; keeps focus stable
        hdr.type         = QStringLiteral("_livetvheader");
        hdr.title        = ch.name;
        hdr.subtitle     = ch.note;                                 // empty for Live TV: the #75 row exactly
        hdr.thumbnailUrl = ch.logo;                                 // absent art is unremarkable
        cat.items.push_back(hdr);

        // Today's programmes for this channel: those whose window overlaps the day, in start order.
        QVector<xmltv::Programme> today;
        for (const xmltv::Programme& p : ch.programmes)
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
            it.id       = guideCellId(ch.key, p.startUtc);
            it.type     = cellType;
            it.title    = (onAir ? QStringLiteral("●  ") : QString()) + hhmm + QStringLiteral("  ") + p.title;
            it.subtitle = p.desc;
            if (!cellMimePrefix.isEmpty())
                it.mime = cellMimePrefix + ch.key + QLatin1Char('@') + p.startUtc.toUTC().toString(Qt::ISODate);
            cat.items.push_back(it);
        }
    }
    cat.hasMore = false;
    return cat;
}

}
