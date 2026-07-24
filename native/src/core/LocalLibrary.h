// A local VIDEO library (movies + TV) laid out Kodi/Plex-style. The root is Settings::libraryFolder
// (default <data>/library). We walk it, classify each video by filename convention, read Kodi .nfo
// sidecars for the imdb id + plot + thumb, and surface each file as a first-class MediaItem. The pure
// functions (parseFile/scanFolder/buildIndex) take an explicit root and are probe-tested; the cached
// index convenience layer (Task 3) reads Settings and is main-thread only.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>

namespace LocalLibrary
{
    enum class Kind { Movie, Episode };

    struct VideoEntry
    {
        QString path;
        Kind    kind = Kind::Movie;
        QString title;
        int     year = 0;
        QString show;
        int     season = 0;
        int     episode = 0;
        QString imdbId;
        QString seriesImdbId;
        QString plot;
        QString thumbPath;
    };

    struct OwnedIndex
    {
        QHash<QString, QString> pathById;
        QHash<QString, int>     seriesCount;
        QVector<VideoEntry>     entries;

        bool    ownsId(const QString& id) const { return pathById.contains(id) || seriesCount.contains(id); }
        QString localPathFor(const QString& id) const { return pathById.value(id); }
        int     ownedEpisodes(const QString& seriesId) const { return seriesCount.value(seriesId); }
        const QVector<VideoEntry>& all() const { return entries; }
    };

    // Pure (probe-tested), root explicit.
    VideoEntry          parseFile(const QString& path);
    QVector<VideoEntry> scanFolder(const QString& root);
    OwnedIndex          buildIndex(const QVector<VideoEntry>& entries,
                                   const QHash<QString, QStringList>& extraMovieIdsByPath = {});
    QString             displayTitle(const VideoEntry& e);

    // Cached process-wide index (main-thread only). root() reads Settings::libraryFolder().
    QString            root();
    void               installIndex(OwnedIndex idx);
    const OwnedIndex&  index();
}
