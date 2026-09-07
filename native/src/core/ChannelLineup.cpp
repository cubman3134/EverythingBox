#include "ChannelLineup.h"
#include "LocalLibrary.h"
#include "MediaDurations.h"
#include "PlaylistStore.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QHash>

#include <algorithm>

using channels::Candidate;
using channels::Channel;
using channels::LineupItem;
using channels::SourceKind;

namespace
{
QHash<int, std::function<QVector<Candidate>(const Channel&)>>& resolvers()
{
    static QHash<int, std::function<QVector<Candidate>(const Channel&)>> r;
    return r;
}

// A playlist entry -> a candidate. WHAT THE PLAYER IS HANDED is the entry's local path when it has one, and
// its addon item id otherwise; the same string is what the duration index is keyed by (it is the identity
// PlaybackSession resumes under), so the two cannot disagree about which item a length belongs to.
Candidate fromPlaylistEntry(const PlaylistEntry& e)
{
    Candidate c;
    c.itemId  = e.itemId.isEmpty() ? e.path : e.itemId;
    c.title   = e.title;
    c.playKey = e.path.isEmpty() ? e.itemId : e.path;
    return c;
}

QVector<Candidate> fromPlaylist(const Channel& ch)
{
    QVector<Candidate> out;
    Playlist pl;
    if (!PlaylistStore::get(ch.sourceId, pl)) return out;
    out.reserve(pl.items.size());
    for (const PlaylistEntry& e : pl.items)
    {
        const Candidate c = fromPlaylistEntry(e);
        if (c.playKey.isEmpty()) continue;   // an entry that names nothing cannot be aired
        out.push_back(c);
    }
    return out;
}

// A LocalFolder channel's sourceId is EITHER a series key (LocalLibrary::showKeyFor) or an absolute folder
// path. Both are accepted, and tested in that order, because the two things a viewer means by "a channel of
// this" are a show and a directory, and one string cannot be mistaken for the other: a show key is
// "name:<lowercased title>" or an imdb id, neither of which is a path.
QVector<Candidate> fromLocalFolder(const Channel& ch)
{
    QVector<Candidate> out;
    if (ch.sourceId.isEmpty()) return out;
    const QString folder = QDir::fromNativeSeparators(ch.sourceId);
    for (const LocalLibrary::VideoEntry& e : LocalLibrary::index().all())
    {
        const bool bySeries = LocalLibrary::showKeyFor(e) == ch.sourceId;
        const QString path  = QDir::fromNativeSeparators(e.path);
        const bool byFolder = !folder.isEmpty()
                              && (path.startsWith(folder + QLatin1Char('/'), Qt::CaseInsensitive)
                                  || QFileInfo(path).absolutePath().compare(folder, Qt::CaseInsensitive) == 0);
        if (!bySeries && !byFolder) continue;
        Candidate c;
        c.itemId  = LocalLibrary::tileId(e);
        c.title   = LocalLibrary::displayTitle(e);
        c.playKey = e.path;
        if (c.itemId.isEmpty()) c.itemId = e.path;
        out.push_back(c);
    }
    // Deterministic order regardless of the scan's, so two devices that walked the folder in a different
    // sequence lay out the same day and an InOrder channel plays a series in the order the files are named.
    // By PATH, case-insensitively: it is the one key every entry has, it is total (no two entries share a
    // path), and for a Kodi-style tree it sorts S01E01 before S01E02 without this file having to know that.
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        return a.playKey.compare(b.playKey, Qt::CaseInsensitive) < 0;
    });
    return out;
}
} // namespace

QVector<Candidate> ChannelLineup::candidatesFor(const Channel& ch)
{
    // An installed resolver wins for its kind — that is the seam increment 2's sources arrive through, and it
    // is also how a probe drives a kind this build cannot otherwise enumerate.
    auto it = resolvers().constFind(channels::toInt(ch.sourceKind));
    if (it != resolvers().constEnd() && *it) return (*it)(ch);

    switch (ch.sourceKind)
    {
        case SourceKind::Playlist:    return fromPlaylist(ch);
        case SourceKind::LocalFolder: return fromLocalFolder(ch);
        // FilterPreset, AddonCatalog and ServerItems have no built-in enumeration in increment 1 (see the
        // header). EMPTY, not "everything" and not a guess: an empty lineup makes the channel say it has
        // nothing to air, which is recoverable, where a guessed lineup would air the wrong programme against
        // a guide that promised another.
        case SourceKind::FilterPreset:
        case SourceKind::AddonCatalog:
        case SourceKind::ServerItems:
        default:
            return {};
    }
}

int ChannelLineup::knownDurationSec(const Candidate& c)
{
    // The item id first (see the header): it is the identity the app resumes — and therefore MEASURES — the
    // item under whenever it has one. The play key is the fallback for a candidate that carries no id.
    const int byId = c.itemId.isEmpty() ? 0 : MediaDurations::seconds(c.itemId);
    if (byId > 0) return byId;
    return c.playKey.isEmpty() ? 0 : MediaDurations::seconds(c.playKey);
}

QVector<LineupItem> ChannelLineup::build(const Channel& ch, QStringList* skipped)
{
    return channels::withDurations(candidatesFor(ch),
                                   [](const Candidate& c) { return ChannelLineup::knownDurationSec(c); },
                                   skipped);
}

// ---- interstitials (issue #179, increment 2) ---------------------------------------------------------------

// The extensions a bumper may have. The same family the local video library indexes; a folder of stills or
// text files simply yields nothing, which is the "empty folder" case and not an error.
static QStringList interstitialNameFilters()
{
    return QStringList{ QStringLiteral("*.mp4"),  QStringLiteral("*.mkv"),  QStringLiteral("*.mov"),
                        QStringLiteral("*.m4v"),  QStringLiteral("*.avi"),  QStringLiteral("*.webm"),
                        QStringLiteral("*.mpg"),  QStringLiteral("*.mpeg"), QStringLiteral("*.ts"),
                        QStringLiteral("*.wmv") };
}

QVector<Candidate> ChannelLineup::interstitialCandidates(const QString& dir, QString* reason)
{
    if (reason) reason->clear();
    QVector<Candidate> out;
    const QString path = dir.trimmed();
    if (path.isEmpty()) return out;          // no folder set is the DEFAULT, never an error

    const QFileInfo fi(path);
    if (!fi.exists())
    {
        if (reason) *reason = QObject::tr("There is no folder at %1.").arg(QDir::toNativeSeparators(path));
        return out;
    }
    if (!fi.isDir())
    {
        if (reason) *reason = QObject::tr("%1 is a file, not a folder of bumpers.")
                                  .arg(QDir::toNativeSeparators(path));
        return out;
    }
    QDir d(path);
    if (!d.isReadable())
    {
        if (reason) *reason = QObject::tr("%1 cannot be read.").arg(QDir::toNativeSeparators(path));
        return out;
    }
    // NOT RECURSIVE, and named as a decision rather than an omission: a bumper folder is a short flat list a
    // person curates, and walking a whole tree from a mistyped path is how a channel ends up airing somebody's
    // holiday videos between episodes.
    QFileInfoList files = d.entryInfoList(interstitialNameFilters(), QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& f : files)
    {
        Candidate c;
        c.playKey = f.absoluteFilePath();
        c.itemId  = c.playKey;               // a bumper is not a library item: its path IS its identity
        c.title   = f.completeBaseName();
        out.push_back(c);
    }
    // The scan order QDir::Name gives is already stable, but say so explicitly: the break at 20:25 has to hold
    // the same ident on the phone and on the TV, and that rests on both walking the folder the same way.
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        return a.playKey.compare(b.playKey, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QVector<LineupItem> ChannelLineup::interstitials(const Channel& ch, const QString& globalDir,
                                                 QStringList* skipped, QString* reason)
{
    const QString dir = channels::interstitialDirFor(ch, globalDir);
    return channels::withDurations(interstitialCandidates(dir, reason),
                                   [](const Candidate& c) { return ChannelLineup::knownDurationSec(c); },
                                   skipped);
}

void ChannelLineup::setSourceResolver(SourceKind kind,
                                      std::function<QVector<Candidate>(const Channel&)> fn)
{
    if (fn) resolvers().insert(channels::toInt(kind), std::move(fn));
    else    resolvers().remove(channels::toInt(kind));
}

void ChannelLineup::clearSourceResolvers() { resolvers().clear(); }
