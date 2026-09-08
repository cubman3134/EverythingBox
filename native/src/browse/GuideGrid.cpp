#include "GuideGrid.h"

#include <QObject>
#include <algorithm>

namespace browse
{

static QLatin1String cellIdPrefix() { return QLatin1String("_guideprog:"); }

QString guideOnAirMarker() { return QStringLiteral("●  "); }

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
            it.title    = (onAir ? guideOnAirMarker() : QString()) + hhmm + QStringLiteral("  ") + p.title;
            it.subtitle = p.desc;
            if (!cellMimePrefix.isEmpty())
                it.mime = cellMimePrefix + ch.key + QLatin1Char('@') + p.startUtc.toUTC().toString(Qt::ISODate);
            cat.items.push_back(it);
        }
    }
    cat.hasMore = false;
    return cat;
}

int guideNowIndex(const MediaCatalog& cat, const QDateTime& nowUtc)
{
    const QDateTime now = nowUtc.toUTC();
    const QString   mark = guideOnAirMarker();
    int  answer = -1;      // this channel section's answer so far
    bool onAir  = false;   // ...and whether it is the strong kind (rule 1) rather than the next-up (rule 2)

    // One walk, section by section. `i == size` closes the last section, which is why the loop runs one past
    // the end rather than repeating the flush after it.
    for (int i = 0; i <= cat.items.size(); ++i)
    {
        const bool end    = (i == cat.items.size());
        const bool header = !end && cat.items.at(i).type == QLatin1String("_livetvheader");
        if (end || header)
        {
            if (answer >= 0) return answer;   // the first channel that answers at all wins
            onAir = false;                     // ...otherwise start the next section clean
            continue;
        }
        const MediaItem& it = cat.items.at(i);
        QString key; QDateTime start;
        if (!parseGuideCellId(it.id, key, start)) continue;   // a note row / anything that is not a cell
        if (it.title.startsWith(mark))
        {
            if (!onAir) { answer = i; onAir = true; }   // rule 1, and the first marked cell is the answer
            continue;
        }
        if (!onAir && answer < 0 && start >= now) answer = i;  // rule 2: the next one due to start
    }
    return -1;
}

}
