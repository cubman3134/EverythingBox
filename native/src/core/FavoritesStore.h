// Per-profile favourites: catalog items a user has starred. They appear in a "Favorites" section on the
// Home page and re-open their detail page when clicked. Stored as a JSON list in mymediavault.ini, keyed by the
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
    qint64  ts = 0;        // epoch seconds this favourite was added/last written (multi-device merge: newest-ts wins)
};

namespace FavoritesStore
{
    QVector<FavoriteItem> list();               // for the active profile
    void add(const FavoriteItem& item);         // de-duped by itemId, newest first
    void remove(const QString& itemId);
    bool isFavorite(const QString& itemId);

    // Identity keys (itemId + path) of EVERY profile's favourites, for image-cache pinning: a starred
    // item's art must never be evicted, whichever profile starred it.
    QSet<QString> allKeys();

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
