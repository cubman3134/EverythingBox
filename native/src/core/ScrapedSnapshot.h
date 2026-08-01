// The providers' own answer for ONE item, stamped with the item it belongs to (issue #24).
//
// WHY THIS IS A TYPE AND NOT A BARE MEMBER. The metadata editor corrects an item against "what the scraper
// found", and the richest copy of that is the live /meta reply the open card was drawn from — richer than the
// scrape cache (full facts, full synopsis), so the card must not be re-rendered from the cache alone after an
// edit. Holding that reply in a plain MediaDetail member is what went wrong: it is written only when a reply
// ARRIVES, so an item whose addon returns nothing (offline, or gone upstream) never writes one — and the
// PREVIOUS item's reply was still sitting in the member. The editor, opened on this item's key, then showed
// the previous item's title, subtitle, synopsis and poster as "what the scraper found": the OSK seeded from
// it, the "typed back what the scraper found -> store nothing" comparison ran against it, and committing a
// field wrote one item's content into another item's override — which CloudMerge then carried to every
// device. That corrupts the stored record, not just pixels.
//
// The fix is to make the shape unspellable rather than to remember to clear the member on every exit path
// (navigating back, hiding the card, a request that never lands, a pop): the value cannot be read without
// naming the item it is wanted for, and forKey() answers for that item only. Nothing here is a cache — it is
// one slot, deliberately, so the snapshot of the card you are looking at is the only one that exists.
//
// Pure and header-only (no Qt beyond the models), so the surfaces that use it are Qt Widgets/QML classes no
// headless probe can link while the RULE they depend on is still asserted directly — see probe_meta's
// "keyed scrape snapshot" section, and the source gate in run-headless-probes.sh that pins the UI to it.
#pragma once
#include "../addons/AddonModels.h"

#include <QString>

namespace MetaEdit
{
struct ScrapedSnapshot
{
    // Stamp the providers' answer for `itemKey`. An item with NO identity (no id and no url — the key space
    // is MetaCache::keyFor) cannot own a snapshot: storing one under the empty key would let the next
    // identity-less card read the previous item's answer back out, the same defect one level down.
    void remember(const QString& itemKey, const MediaDetail& scraped)
    {
        if (itemKey.isEmpty()) { key_.clear(); detail_ = MediaDetail{}; return; }
        key_ = itemKey;
        detail_ = scraped;
    }

    // The answer for `itemKey`, or an invalid MediaDetail when the snapshot belongs to a different item (or
    // to no item yet). The caller then falls back to a per-item source — the scrape cache — rather than to
    // whatever happened to be shown last.
    //
    // No empty-key guard here: remember() refuses to store one, so key_ is empty exactly when nothing is
    // held, and detail_ is default-invalid then. A second guard saying that again would be a line no
    // mutation could kill.
    MediaDetail forKey(const QString& itemKey) const
    {
        return (itemKey == key_) ? detail_ : MediaDetail{};
    }

private:
    QString key_;
    MediaDetail detail_;
};
} // namespace MetaEdit
