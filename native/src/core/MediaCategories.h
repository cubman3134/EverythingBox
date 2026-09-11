// The type -> top-level category oracle, extracted to core so it can be linked without pulling in HomeView
// (which drags Quick/Widgets/the whole browse UI). A catalog/media type maps to exactly one of the four
// inherent buckets the home surface groups by. HomeView::mediaCategory delegates here so the two can never
// drift; PlaylistStore's category migration and probe_playlists pin this same function.
//
// Tokens are VERBATIM the strings the rest of the app keys off ("video" | "audio" | "game" | "reading" —
// `game` SINGULAR). Anything unrecognised (movie, series, tv, livetv, channel, film, video, …) falls to
// "video", the catch-all default.
#pragma once
#include <QString>

namespace core
{
    inline QString mediaCategory(const QString& type)
    {
        const QString t = type.toLower();
        if (t == QLatin1String("album") || t == QLatin1String("track") || t == QLatin1String("music")
            || t == QLatin1String("song") || t == QLatin1String("audiobook") || t == QLatin1String("podcast")
            || t == QLatin1String("podcast_episode"))                    return QStringLiteral("audio");
        if (t == QLatin1String("game") || t == QLatin1String("platform")
            || t == QLatin1String("rom") || t == QLatin1String("console")) return QStringLiteral("game");
        if (t == QLatin1String("book") || t == QLatin1String("ebook") || t == QLatin1String("novel")
            || t == QLatin1String("comic") || t == QLatin1String("comic_issue") || t == QLatin1String("manga")
            || t == QLatin1String("manga_chapter"))                      return QStringLiteral("reading");
        // Photos (issue #102): the local image library's own bucket. Additive — no existing type maps here, so
        // no playlist migration changes, and the unknown->video fallback below is untouched.
        if (t == QLatin1String("photo") || t == QLatin1String("photos")
            || t == QLatin1String("image") || t == QLatin1String("images")) return QStringLiteral("photos");
        // movie, series, tv, livetv, livesport(s), channel, film, video, ... and anything unrecognised:
        return QStringLiteral("video");
    }

    // True for a key mediaCategory can answer — i.e. a real bucket a playlist can be filed under.
    inline bool isMediaCategory(const QString& key)
    {
        return key == QLatin1String("video") || key == QLatin1String("audio") || key == QLatin1String("game")
            || key == QLatin1String("reading") || key == QLatin1String("photos");
    }

    // Which playlist bucket an "Add to playlist" files into (issue #373). Every entry point — the P key, both
    // classic menus, the themed chooser, the games menu — asks this one question.
    //   catalogKey        — HomeView::currentCatalogKey(): "addonId|catalogId|catalogType" of the browse root.
    //   activeCategoryKey — the bucket of the category the user is standing in, when the root is one of the
    //                       SYNTHETIC categories (Music, Audiobooks, My Books, Photos, a bucket's Playlists).
    //   itemType          — the row being added.
    //
    // The synthetic roots are pushed with no catalogue at all, so their key is "native||" and the old answer,
    // mediaCategory(""), was the catch-all "video": a track from the local music library, or from a Subsonic
    // server merged into it, was filed among the Video playlists. In order:
    //   1. a real catalogue at the root (it names a catalogue id or a type) — its type decides, exactly as it
    //      always has, whatever the row or the category say;
    //   2. otherwise the category the user is in, when that is a real bucket;
    //   3. otherwise the row's own type (a search root, Home) — which is mediaCategory's catch-all only when the
    //      row itself says nothing better.
    inline QString playlistCategory(const QString& catalogKey, const QString& activeCategoryKey,
                                    const QString& itemType)
    {
        const QString catalogId   = catalogKey.section(QLatin1Char('|'), 1, 1);
        const QString catalogType = catalogKey.section(QLatin1Char('|'), 2, 2);
        if (!catalogId.isEmpty() || !catalogType.isEmpty()) return mediaCategory(catalogType);
        if (isMediaCategory(activeCategoryKey))              return activeCategoryKey;
        return mediaCategory(itemType);
    }
}
