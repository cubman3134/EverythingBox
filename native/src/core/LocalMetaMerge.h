// The metadata layer persisted to MetaCache for a resolved local library file (issue #73).
//
// CatalogResolver turns a scanned file into a catalog id; this composes the id's scraped getMeta with the
// file's own .nfo sidecar so the local tile renders poster/plot/year like any addon tile. The precedence a
// surface ultimately sees is
//
//     manual override (#24)  >  .nfo sidecar  >  scraped-fills-blank
//
// and this function produces the lower TWO layers. The top layer is deliberately NOT baked in here: MetaCache
// composites the user's override at READ time (cachedDetail / loadArt / displayImage), which is the only thing
// that keeps "reset to scraped" honest — a baked-in override would make the reset appear to restore the edit.
// So production stores baseDetail() and the read path adds the override; the probe pins the full three-way
// precedence by composing baseDetail() with MetaOverrides::applyTo(), the same two production functions.
//
// WHICH FIELDS ARE AUTHORITATIVE. A Kodi .nfo (LocalLibrary::readNfo) fills only plot and thumb (and the imdb
// id, which is the tile key itself). Those are authoritative: a present .nfo value wins, and the scrape fills
// it only when blank. title/year are NOT .nfo data — they are parsed from the filename — so they are a
// FALLBACK the scrape outranks: the scraped title/year win, and the filename-parsed value fills a blank.
#pragma once
#include "LocalLibrary.h"
#include "../addons/AddonModels.h"

namespace LocalMeta
{
    MediaDetail baseDetail(const LocalLibrary::VideoEntry& local, const MediaDetail& scraped);
}
