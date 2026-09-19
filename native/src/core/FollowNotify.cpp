#include "FollowNotify.h"

#include <QCoreApplication>
#include <QHash>
#include <algorithm>

namespace follownotify
{

static QString tr(const char* s) { return QCoreApplication::translate("FollowNotify", s); }

qint64 SeriesNews::newestAt() const
{
    qint64 t = 0;
    for (const NewChild& c : children) t = std::max(t, c.foundAt);
    return t;
}

QString SeriesNews::newestTitle() const
{
    // The newest by foundAt; among equals the LAST listed, which is the order the source listed them in and
    // so, for a source that lists oldest-first (nearly all of them), the most recent episode.
    const NewChild* best = nullptr;
    for (const NewChild& c : children)
        if (!best || c.foundAt >= best->foundAt) best = &c;
    return best ? best->title : QString();
}

SeriesNews newsFor(const QString& seriesId, const QString& seriesTitle,
                   const QVector<FollowSnapshot::Pending>& pending, const QStringList& foundIds,
                   const std::function<bool(const QString&)>& dealtWith)
{
    SeriesNews n;
    n.seriesId = seriesId;
    n.seriesTitle = seriesTitle.isEmpty() ? seriesId : seriesTitle;
    const QSet<QString> found(foundIds.cbegin(), foundIds.cend());
    QSet<QString> taken;
    for (const FollowSnapshot::Pending& p : pending)
    {
        if (!found.contains(p.id) || taken.contains(p.id)) continue;   // announced by an EARLIER cycle
        if (dealtWith && dealtWith(p.id)) continue;                     // seen before the cycle ended
        taken.insert(p.id);
        n.children.push_back({ p.id, p.title, p.foundAt });
    }
    return n;
}

QVector<SeriesNews> dropMuted(const QVector<SeriesNews>& in, const std::function<bool(const QString&)>& isMuted)
{
    QVector<SeriesNews> out;
    for (const SeriesNews& s : in)
    {
        if (s.count() <= 0) continue;
        if (isMuted && isMuted(s.seriesId)) continue;
        out.push_back(s);
    }
    return out;
}

QVector<SeriesNews> mergeNews(const QVector<SeriesNews>& earlier, const QVector<SeriesNews>& later)
{
    QVector<SeriesNews> out = earlier;
    QHash<QString, int> at;
    for (int i = 0; i < out.size(); ++i) at.insert(out[i].seriesId, i);
    for (const SeriesNews& s : later)
    {
        const auto it = at.constFind(s.seriesId);
        if (it == at.constEnd())
        {
            at.insert(s.seriesId, int(out.size()));
            out.push_back(s);
            continue;
        }
        SeriesNews& dst = out[it.value()];
        if (!s.seriesTitle.isEmpty()) dst.seriesTitle = s.seriesTitle;   // the fresher title, if it was renamed
        QSet<QString> have;
        for (const NewChild& c : dst.children) have.insert(c.id);
        for (const NewChild& c : s.children)
            if (!have.contains(c.id)) { have.insert(c.id); dst.children.push_back(c); }
    }
    return out;
}

QString nameList(const QStringList& names, int maxNamed)
{
    const int n = int(names.size());
    if (n == 0) return QString();
    if (n == 1) return names.first();
    if (n <= maxNamed)
    {
        const QStringList head = names.mid(0, n - 1);
        return tr("%1 and %2").arg(head.join(QStringLiteral(", ")), names.last());
    }
    return tr("%1 and %2 more").arg(names.mid(0, maxNamed).join(QStringLiteral(", ")), QString::number(n - maxNamed));
}

Notice summarize(const QVector<SeriesNews>& newsIn)
{
    QVector<SeriesNews> news = newsIn;
    std::stable_sort(news.begin(), news.end(), [](const SeriesNews& a, const SeriesNews& b) {
        if (a.newestAt() != b.newestAt()) return a.newestAt() > b.newestAt();
        if (a.count() != b.count()) return a.count() > b.count();
        return a.seriesTitle.localeAwareCompare(b.seriesTitle) < 0;
    });
    Notice n;
    n.seriesCount = int(news.size());
    for (const SeriesNews& s : news) n.itemCount += s.count();
    if (news.isEmpty()) return n;
    if (news.size() == 1)
    {
        const SeriesNews& s = news.first();
        n.title = s.count() == 1 ? tr("%1: 1 new episode").arg(s.seriesTitle)
                                 : tr("%1: %2 new episodes").arg(s.seriesTitle, QString::number(s.count()));
        n.body = s.newestTitle();
        return n;
    }
    n.title = tr("New in %1 series you follow").arg(news.size());
    QStringList names;
    for (const SeriesNews& s : news) names << s.seriesTitle;
    n.body = nameList(names);
    return n;
}

Decision Outbox::onCycle(const QVector<SeriesNews>& news, Consent consent, bool holding,
                         const std::function<bool(const QString&)>& isMuted)
{
    Decision d;
    const QVector<SeriesNews> live = dropMuted(news, isMuted);   // muted series never reach the grouping
    if (live.isEmpty()) return d;
    switch (consent)
    {
    case Consent::Off:
        pending_.clear();
        return d;
    case Consent::Unset:
        // Off by default: nothing is delivered. The first cycle that finds something offers to turn it on —
        // once, ever. The prompt is not a notification, so it is never queued alongside one.
        if (!prompted_)
        {
            prompted_ = true;
            if (holding) { promptPending_ = true; d.held = true; }
            else         d.prompt = true;
        }
        return d;
    case Consent::On:
        break;
    }
    const QVector<SeriesNews> merged = mergeNews(pending_, live);
    if (holding)
    {
        // ONE pending summary, not a queue of them: a later cycle folds into it through the same grouping.
        pending_ = merged;
        d.held = true;
        return d;
    }
    pending_.clear();
    // AT MOST ONE per cycle: everything this cycle (and anything held before it) becomes one Notice.
    d.notices.push_back(summarize(merged));
    return d;
}

Decision Outbox::release(Consent consent, const std::function<bool(const QString&)>& isMuted,
                         const std::function<bool(const QString&, const QString&)>& stillNew)
{
    Decision d;
    if (promptPending_)
    {
        promptPending_ = false;
        if (consent == Consent::Unset) d.prompt = true;   // answered in the meantime: nothing left to ask
    }
    if (pending_.isEmpty()) return d;
    QVector<SeriesNews> held = pending_;
    pending_.clear();
    if (consent != Consent::On) return d;                  // switched off while it was held: drop it
    for (SeriesNews& s : held)
    {
        QVector<NewChild> keep;
        for (const NewChild& c : s.children)
            if (!stillNew || stillNew(s.seriesId, c.id)) keep.push_back(c);
        s.children = keep;
    }
    held = dropMuted(held, isMuted);
    if (!held.isEmpty()) d.notices.push_back(summarize(held));
    return d;
}

} // namespace follownotify
