// Offline metadata for downloaded media. When something is downloaded for keeps, its catalog metadata
// (title/type/synopsis/facts) and artwork are saved locally so the Downloaded/Recent shelves — posters,
// info panels, detail pages — keep working with no network.
//
// Layout: one folder per item under <dataDir>/metadata/<sha1(key)>/
//   meta.json   - versioned JSON: { "v", "key", "savedAt", "item": {...}, "detail": {...},
//                 "images": { "<role>": "<file>" }, ... }. All writes MERGE into the existing object,
//                 so unknown/future top-level keys survive round-trips — new metadata kinds can be added
//                 later by merging another key, with no migration and no code changes here.
//   <role>.<ext> - downloaded artwork ("thumb" = grid poster, "poster" = detail cover; roles are open-ended).
//
// The key is the item's stable addon id when it has one, else its url/path (same identity the stores use).
#pragma once
#include "../addons/AddonModels.h"
#include <QJsonObject>
#include <QString>

namespace MetaCache
{
    QString keyFor(const MediaItem& item);      // stable id, else url; empty when the item has neither
    QString dirFor(const QString& key);         // the item's metadata folder (not created by this call)

    // Read the whole bundle ({} when none) / merge a patch into it (top-level keys replace; everything
    // else already present is preserved). merge() stamps "v", "key" and "savedAt".
    QJsonObject load(const QString& key);
    void merge(const QString& key, const QJsonObject& patch);

    // Convenience writers for what the app knows today (each is one merge() under a dedicated key).
    void saveItem(const MediaItem& item);                        // -> "item"
    void saveDetail(const QString& key, const MediaDetail& d);   // -> "detail"

    // The extensible artwork/videos/audio/metadata bundle (logo, box, fanart, screenshots, trailers, theme
    // music, provider facts). saveArt records every role's URLs under "art" AND kicks off background
    // downloads of each image role so it renders offline; loadArt reconstructs the bundle, resolving each
    // image role to its locally cached file when present (best-first), else the stored URL. Roles/keys are
    // open-ended, so a new metadata provider needs no change here.
    void saveArt(const QString& key, const MediaArt& art);
    MediaArt loadArt(const QString& key);

    // Reconstruct a detail card from the cache (valid=false when nothing usable is stored). The image
    // resolves to the locally cached artwork when present, so it renders fully offline.
    MediaDetail cachedDetail(const QString& key);

    // Artwork. cacheImage downloads url into the item's folder as <role>.<ext> (async; no-op for empty /
    // non-http urls or when already cached) and records it under "images". imagePath returns the local
    // file for a role ("" when absent); displayImage picks the cached grid image if present, else `url`.
    void cacheImage(const QString& key, const QString& role, const QString& url);
    QString imagePath(const QString& key, const QString& role);
    QString displayImage(const QString& key, const QString& url);

    void remove(const QString& key);            // delete the item's whole metadata folder (uninstall)
}
