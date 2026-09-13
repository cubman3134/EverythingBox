// THE KEY A HOME TAB IS ADDRESSED BY (issue #392).
//
// Every entry on the home's tab strip (and, through HomeView::navTargets_, every category on the carousel, the
// XMB and the themed home) is addressed by one string, its nav key. activateNav() opens the FIRST target with a
// key, styleTypeButtons() lights EVERY button with it, the carousel treats a key spelled `item:<n>` as a grid
// item rather than a tab, and the themed home names a catalogue tile `source:<key>` in the synced home-row list.
//
// The built-in tabs have fixed keys (home, photos, music, audiobooks, books). A catalogue's tab used its raw
// catalogue id, which is chosen by whoever wrote the add-on — so a server whose music catalogue was called
// `music` produced a second tab keyed `music`: pressing one could open the other, both lit up, and the row list
// kept whichever it met first and dropped the other. Nothing about `music` was special; any built-in key did it.
//
// THE RULE: a catalogue id keeps its own spelling as its key, EXCEPT an id that a built-in key, the carousel's
// item prefix or this file's own prefix could be confused with — that one is written `catalog:<id>`. Escaping
// the prefix itself is what makes the map one-to-one: an unescaped key never starts with `catalog:`, an escaped
// one always does, and no built-in key starts with it. So a catalogue can no longer spell a built-in key, and
// two different catalogue ids can never be given one key by the escape.
//
// WHY ESCAPE ONLY THOSE, rather than prefixing every catalogue or renaming the built-ins: the key is PERSISTED.
// The home-row list (HomeRows.h, issue #161) stores `source:<key>` and syncs it across a profile's devices.
// Prefixing every catalogue would orphan every arranged catalogue row on every install; renaming the built-ins
// would orphan every arranged Music/Photos/Audiobooks/My Books row. Escaping only a colliding id changes the key
// of nothing that did not already collide.
//
// ...AND A STORED ROW FROM BEFORE STILL RESOLVES. A list written before this change names such a catalogue
// `source:<id>`. catalogueRowId() hands that old spelling back to the catalogue whenever nothing else on this
// device now answers to it (no built-in tab of that key is present — always so for `home`, which never has a
// row), so its position and its hidden flag survive. Where a built-in of that key IS present the old spelling
// was always ambiguous — both rows produced it — and it stays with the built-in, whose key never changed; the
// catalogue then appears under its new id, at the end, like any row the list has not heard of.
//
// Header-only and QtCore-only: probe_homerows drives it with no HomeView.
#pragma once
#include <QSet>
#include <QString>
#include <QStringList>

namespace navkeys
{
    // The keys the built-in tabs are addressed by. EVERY literal HomeView passes to makeTab(), to a built-in
    // navTargets_ entry or to styleTypeButtons() must be listed here — the headless runner checks it — or a
    // catalogue could spell the new one.
    inline const QStringList& builtInKeys()
    {
        static const QStringList keys = { QStringLiteral("home"), QStringLiteral("photos"), QStringLiteral("music"),
                                          QStringLiteral("audiobooks"), QStringLiteral("books") };
        return keys;
    }

    // The carousel reads a key with this prefix as a grid item (HomeView's CarouselView::activated handler).
    inline QLatin1String itemPrefix()      { return QLatin1String("item:"); }
    // The escape a colliding catalogue id is written under.
    inline QLatin1String cataloguePrefix() { return QLatin1String("catalog:"); }

    inline bool mustEscape(const QString& catalogueId)
    {
        return builtInKeys().contains(catalogueId) || catalogueId.startsWith(itemPrefix())
               || catalogueId.startsWith(cataloguePrefix());
    }

    // The nav key of a catalogue's tab.
    inline QString forCatalogue(const QString& catalogueId)
    {
        return mustEscape(catalogueId) ? QString(cataloguePrefix()) + catalogueId : catalogueId;
    }

    // The home-row id (`source:<key>`) of any tab.
    inline QString rowIdForKey(const QString& navKey) { return QStringLiteral("source:") + navKey; }

    // The home-row id a CATALOGUE's tile answers to. `rowKeys` is every nav key that has a `source:` row on this
    // device right now, built-in and catalogue alike — every tab but Home, which has no row; `storedRowIds` is
    // every id in the profile's stored row list.
    inline QString catalogueRowId(const QString& catalogueId, const QSet<QString>& rowKeys,
                                  const QSet<QString>& storedRowIds)
    {
        const QString key = forCatalogue(catalogueId);
        const QString id  = rowIdForKey(key);
        if (key == catalogueId || storedRowIds.contains(id)) return id;   // never renamed, or already re-stored
        const QString legacy = rowIdForKey(catalogueId);
        if (storedRowIds.contains(legacy) && !rowKeys.contains(catalogueId)) return legacy;
        return id;
    }
}
