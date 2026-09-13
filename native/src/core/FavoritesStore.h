// Per-profile favourites: catalog items a user has starred. They appear in a "Favorites" section on the
// Home page and re-open their detail page when clicked. Stored as a JSON list in everythingbox.ini, keyed by the
// active profile (so each user has their own). Enough of the MediaItem is kept to display + re-open it.
#pragma once
#include "SystemCatalog.h"
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

struct FavoriteItem
{
    QString addonId;       // source addon (to resolve the LoadedAddon when re-opening)
    QString itemId;        // the addon's item id (getDetail/getMeta key) - also the favourite's identity
    QString title;
    QString subtitle;
    QString type;          // media type (movie/series/album/...) - drives the icon
    QString thumbnailUrl;  // poster/cover to show
    bool expandable = false;
    // Local-file favourites (a Recent/Downloaded game starred from its item menu): these re-open by path
    // through openRecent (which recovers the console from the Recent/Downloads store) instead of via an addon.
    // Empty for ordinary addon-catalog favourites.
    QString path;          // absolute file path to re-open
    QString kind;          // "game" | "pcgame" | … (openRecent routing kind)
    QString system;        // games: the SystemCatalog id (or "pc"), so favourites can be shown per-console
    // Music tracks (#368): the album the starred row was ON — the music key its row's mime names. Read only
    // where the id cannot name its own album (an EverythingBox server's shelf: its track id carries the shelf
    // and the track, and its play url lives for one session, filled when that album is fetched). A key, never
    // a url. Empty for everything else, and for a track starred before #368.
    QString albumKey;
    qint64  ts = 0;        // epoch seconds this favourite was added/last written (multi-device merge: newest-ts wins)
};

namespace FavoritesStore
{
    QVector<FavoriteItem> list();               // for the active profile
    void add(const FavoriteItem& item);         // de-duped by itemId, newest first
    // Add a favourite that CAME FROM the place a love would be sent to (issue #193, increment 6): a track a
    // music server already holds a star for. Identical to add() except that the love hook is not fired, and
    // the reason is in FavoritesStore.cpp - telling the server about a star it gave us is a request per track
    // that changes nothing. It is not a general quiet add; see the note there.
    void addFromSource(const FavoriteItem& item);
    void remove(const QString& itemId);
    bool isFavorite(const QString& itemId);
    // A star the user PRESSED, as one call: add() when the item is not a favourite, remove() when it is — so
    // the love hook below fires either way and the server star (#193 increment 6) goes wherever the press
    // came from (issue #297, the classic track menus). Returns whether the item is a favourite afterwards.
    // Never the path for a star that came FROM a server; that is addFromSource, and it must stay quiet.
    bool toggle(const FavoriteItem& item);

    // Identity keys (itemId + path) of EVERY profile's favourites, for image-cache pinning: a starred
    // item's art must never be evicted, whichever profile starred it.
    QSet<QString> allKeys();

    // Multi-device sync trigger (mdsync T2): a change-callback fired after add/remove, set once by MainWindow
    // to (re)arm the debounced Drive push. QtCore-clean (a std::function, not a Qt signal). Unset in probes.
    void setChangeHook(std::function<void()> hook);

    // MUSIC SCROBBLING (issue #192): "love" and "unlove", mapped onto the favourite action the app already
    // has. Fired with the item that was starred or un-starred and which of the two it was.
    //
    // WHY HERE AND NOT AT THE BUTTON. There are five places that star something — the themed leaf action, the
    // classic grid, the bulk-select verb, the Live TV channel row, the detail panel — and a hook at any one of
    // them would be a love that works from one surface and silently does not from the others, which is exactly
    // the class of bug the scrobbling issue is about. add() and remove() are the two functions all five go
    // through, so this is the only seam that cannot be half-wired.
    //
    // remove() looks the item up BEFORE deleting it and passes what it found, so an un-love carries the type
    // and title the listener actually un-starred rather than a bare id the caller would have to re-resolve
    // from a store that no longer holds it.
    void setLoveHook(std::function<void(const FavoriteItem&, bool loved)> hook);

    // The SystemCatalog id for a local-game favourite, derived from what it re-opens as: PC games are
    // always "pc" (checked first — .exe is also a psx disc extension), emulated ROMs map by extension.
    // Empty when the path isn't a known game file. Inline+pure so headless probes can cover it.
    inline QString deriveSystem(const QString& path, const QString& kind)
    {
        if (kind == QStringLiteral("pcgame")) return QStringLiteral("pc");
        if (path.isEmpty()) return QString();
        const GameSystem* sys = SystemCatalog::forExtension(QFileInfo(path).suffix().toLower());
        return sys ? sys->id : QString();
    }

    // One-time migration: favourites saved before `system` was stamped (so the per-console ★ Favorites
    // folder never matched them) get it derived. `hint` resolves a favourite through the Recent/Downloads
    // stores, which know the real console for ambiguous extensions (an Atari ST ".st" reads as snes by
    // extension); the extension is the fallback. Returns true if anything changed (the caller re-saves).
    // Streamed favourites (no path) are left alone.
    inline bool backfillSystems(QVector<FavoriteItem>& items,
                                const std::function<QString(const FavoriteItem&)>& hint = {})
    {
        bool changed = false;
        for (FavoriteItem& it : items)
        {
            if (it.path.isEmpty() || !it.system.isEmpty()) continue;
            QString sys = hint ? hint(it) : QString();
            if (sys.isEmpty()) sys = deriveSystem(it.path, it.kind);
            if (!sys.isEmpty()) { it.system = sys; changed = true; }
        }
        return changed;
    }
}
