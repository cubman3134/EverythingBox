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
}
