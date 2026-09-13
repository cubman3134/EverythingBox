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
// TWO ADD-ONS WITH ONE CATALOGUE ID (issue #394). The rule above keys a catalogue by its id alone, so two add-ons
// that both declare `top` produced one key between them, with every effect listed at the top: the second tab
// opened the first add-on's catalogue, both lit up, and an arranged home dropped one tile. When two catalogue
// tabs on the strip share an id, BOTH are written in the add-on-qualified form
//
//     catalog:@<the add-on's manifest id, percent-encoded>/<the catalogue id>
//
// and every catalogue whose id is unique on the strip keeps exactly the key above (keyCatalogueTabs()).
//   * THE SAME ON EVERY DEVICE. The home-row list syncs, so the qualified form is built ONLY from what is the
//     same wherever the add-on is installed: the manifest id the add-on declares and the catalogue id it
//     declares. Never the load order, an index or which add-on "came first" — that differs between devices.
//     That is also why BOTH colliding catalogues are qualified: escaping only the second would give the row
//     `source:top` to add-on A on one device and to add-on B on another.
//   * STILL ONE-TO-ONE. An unescaped key never starts with `catalog:`. An escaped key continues with a built-in
//     key, `item:` or `catalog:` — never `@`. A qualified key always continues with `@`. So the three never meet,
//     none is a built-in key or starts with `item:`, and the manifest id is percent-encoded (no `/` survives) so
//     the first `/` after the `@` is always the separator: two different (add-on, catalogue) pairs never share a
//     qualified key.
//   * WHETHER a catalogue is qualified does depend on this device (a second add-on declaring `top` has to be
//     installed for anything to collide); WHAT it is qualified to does not. The row resolution below is what
//     makes the list survive that: a catalogue answers to every spelling any device could have stored for it.
//
// THE SPELLINGS A CATALOGUE'S ROW ANSWERS TO (catalogueRowId), in order: its own key's row; its qualified
// spelling (stored by a device where its id collided); its unqualified key (stored by a device where it did
// not); its raw id (stored before #392). Its own row wins whenever it is stored. Otherwise it takes the first
// OTHER spelling that is stored, is not the own row of another tab on this device, and that no other catalogue
// on this device also answers to. So:
//   * a plain `source:top` stored by a one-add-on device is the catalogue's own row on any one-add-on device;
//   * on a device where two add-ons declare `top`, that row is AMBIGUOUS — both catalogues answer to it — and
//     neither takes it. The row is KEPT in the list (never pruned: HomeRows.h, a device must never drop a row it
//     cannot place), both qualified tiles appear as rows the list has not heard of, and the plain row is placed
//     again the moment this device is back to one add-on declaring `top`. Handing it to either one would be a
//     guess that differs between devices, which is exactly what the synced list cannot survive;
//   * a qualified row stored by that two-add-on device is taken back by the one catalogue it names on a device
//     where the id is unique — so an arrangement made where the ids collided keeps its place everywhere.
//
// Header-only and QtCore-only: probe_homerows drives it with no HomeView.
#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

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

    // The add-on-qualified key of a catalogue whose id another add-on's tab also uses (#394). Built only from the
    // manifest id and the catalogue id, so it is the same on every device (see the header).
    inline QString qualifiedForCatalogue(const QString& addonId, const QString& catalogueId)
    {
        return QString(cataloguePrefix()) + QLatin1Char('@') + QString::fromLatin1(QUrl::toPercentEncoding(addonId))
               + QLatin1Char('/') + catalogueId;
    }

    // One catalogue tab on the strip: the add-on's manifest id, the catalogue id, and the key keyCatalogueTabs()
    // gives it.
    struct CatalogueTab
    {
        QString addonId;
        QString catalogueId;
        QString key;
    };

    // Keys EVERY catalogue tab on the strip at once, because a key depends on the other tabs: a catalogue whose id
    // a tab of a DIFFERENT add-on also uses gets qualifiedForCatalogue(), and every other one forCatalogue().
    // Nothing here reads the order of `tabs`, so the same add-ons give the same keys however they loaded.
    inline void keyCatalogueTabs(QVector<CatalogueTab>& tabs)
    {
        QHash<QString, QSet<QString>> addonsOfId;   // catalogue id -> the add-ons with a tab for it
        for (const CatalogueTab& t : tabs) addonsOfId[t.catalogueId].insert(t.addonId);
        for (CatalogueTab& t : tabs)
            t.key = addonsOfId.value(t.catalogueId).size() > 1 ? qualifiedForCatalogue(t.addonId, t.catalogueId)
                                                                : forCatalogue(t.catalogueId);
    }

    // The home-row id (`source:<key>`) of any tab.
    inline QString rowIdForKey(const QString& navKey) { return QStringLiteral("source:") + navKey; }

    // Every row id a catalogue's tile answers to, its own first (the order catalogueRowId prefers them in).
    inline QStringList catalogueRowSpellings(const CatalogueTab& tab)
    {
        QStringList ids{ rowIdForKey(tab.key) };
        for (const QString& key : { qualifiedForCatalogue(tab.addonId, tab.catalogueId),   // a colliding device (#394)
                                    forCatalogue(tab.catalogueId),                         // a device where it didn't
                                    tab.catalogueId })                                     // before #392
        {
            const QString id = rowIdForKey(key);
            if (!ids.contains(id)) ids << id;
        }
        return ids;
    }

    // The home-row id a CATALOGUE's tile answers to. `strip` is every catalogue tab on this device, keyed by
    // keyCatalogueTabs() (it includes `tab`); `rowKeys` is every nav key that has a `source:` row on this device
    // right now, built-in and catalogue alike — every tab but Home, which has no row; `storedRowIds` is every id in
    // the profile's stored row list. The rule is in the header: own row if stored, else the first other spelling
    // that is stored and that nothing else on this device owns or also answers to, else the own row.
    inline QString catalogueRowId(const CatalogueTab& tab, const QVector<CatalogueTab>& strip,
                                  const QSet<QString>& rowKeys, const QSet<QString>& storedRowIds)
    {
        const QStringList spellings = catalogueRowSpellings(tab);
        const QString& own = spellings.first();
        if (storedRowIds.contains(own)) return own;
        for (int i = 1; i < spellings.size(); ++i)
        {
            const QString& id = spellings[i];
            if (!storedRowIds.contains(id)) continue;
            if (rowKeys.contains(id.mid(int(qstrlen("source:"))))) continue;   // another tab's own row
            bool shared = false;                                                // ...or another catalogue's too
            for (const CatalogueTab& other : strip)
                if ((other.addonId != tab.addonId || other.catalogueId != tab.catalogueId)
                    && catalogueRowSpellings(other).contains(id))
                { shared = true; break; }
            if (!shared) return id;
        }
        return own;
    }
}
