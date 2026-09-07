#include "ChannelGuide.h"

#include <QObject>

namespace browse
{

MediaCatalog channelGuideCatalog(const QVector<channels::Channel>& chans,
                                 const QVector<channels::Schedule>& days,
                                 const QHash<QString, QString>& logoByChannelId,
                                 const QDateTime& nowUtc,
                                 const QDateTime& dayStartUtc, const QDateTime& dayEndUtc)
{
    QVector<GuideChannel> rows;
    rows.reserve(chans.size());
    for (int i = 0; i < chans.size(); ++i)
    {
        const channels::Channel& c = chans.at(i);
        GuideChannel gc;
        // The ROW-PRODUCER KEY is the channel's durable identity everywhere else in this feature (the
        // favourite, the Recents row, a #161 home row), so it is what the cells are addressed by too. One
        // spelling, so a cell cannot route to a channel the star files under a different name.
        gc.key  = channels::rowProducerKey(c.id);
        gc.name = c.name;
        gc.logo = logoByChannelId.value(c.id);
        if (i < days.size())
            gc.programmes = channels::toProgrammes(days.at(i));   // bumpers are dropped in there
        if (gc.programmes.isEmpty())
        {
            // WHY, in a sentence, and without opening anything. An empty schedule means one of two things and
            // the difference matters to the viewer: a lineup the duration gate emptied (play something from it
            // once and it appears), or a channel that simply is not broadcasting in this day's window.
            const bool nothingCut = (i >= days.size()) || days.at(i).programmes.isEmpty();
            gc.note = nothingCut ? QObject::tr("Nothing with a known length yet")
                                 : QObject::tr("Off air today");
        }
        rows.push_back(gc);
    }
    return guideGridCatalog(QObject::tr("Guide"), rows, nowUtc, dayStartUtc, dayEndUtc,
                            QStringLiteral("_guidetune"), QStringLiteral("guidetune:"));
}

bool parseChannelGuideCell(const QString& mimeOrId, QString& channelId, qint64& cellStartUtc)
{
    QString key; QDateTime start;
    if (!parseGuideCellId(mimeOrId, key, start)) return false;
    const QString id = channels::channelIdFromKey(key);
    if (id.isEmpty()) return false;              // a Live TV cell: its key is a url, not "channel:<id>"
    channelId    = id;
    cellStartUtc = start.toUTC().toSecsSinceEpoch();
    return true;
}

}
