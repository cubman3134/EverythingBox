#include "LocalMetaMerge.h"

MediaDetail LocalMeta::baseDetail(const LocalLibrary::VideoEntry& local, const MediaDetail& scraped)
{
    MediaDetail d = scraped;   // the scrape is the base; .nfo overrides where it spoke, filename fills blanks

    // ---- .nfo authoritative fields (a present sidecar value wins; blank ⇒ the scrape stands) ----
    if (!local.plot.trimmed().isEmpty())
        d.overview = local.plot;

    if (!local.thumbPath.trimmed().isEmpty())
    {
        // The .nfo poster wins on EVERY surface, not just the detail card. Point imageUrl at it AND drop the
        // scraped poster/thumb roles: otherwise MetaCache would prefetch the scraped poster and both
        // cachedDetail's imagePath("poster"/"thumb") and the grid's displayImage()/scrapedImage() would serve
        // it over the sidecar file. Other scraped roles (logo/fanart/background/screenshots) are kept — a .nfo
        // never carries those, so there is nothing authoritative to defend them against.
        d.imageUrl = local.thumbPath;
        d.art.images.remove(QStringLiteral("poster"));
        d.art.images.remove(QStringLiteral("thumb"));
    }

    // ---- filename-parsed fallbacks (the scrape outranks these; fill only a blank) ----
    if (d.title.trimmed().isEmpty())
        d.title = local.title;
    if (d.subtitle.trimmed().isEmpty() && local.year > 0)
        d.subtitle = QString::number(local.year);

    d.valid = !d.title.isEmpty() || !d.overview.isEmpty()
              || !d.imageUrl.isEmpty() || !d.facts.isEmpty() || !d.art.isEmpty();
    return d;
}
