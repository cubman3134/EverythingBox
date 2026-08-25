// A small persistent list of recently opened content (videos, audio, books/PDFs, games). Stored as a
// JSON array in everythingbox.ini so it survives restarts; the Home screen's "Recent" tab lists it and the
// main window re-opens an entry by its kind. Newest first, de-duplicated by path, capped.
//
// AN EXPLICIT REMOVE IS TOMBSTONED; A CAP EVICTION IS NOT (issue #150). CloudMerge unions this profile's list
// with a peer's and cannot read a reason out of an absence, so a removal that left no record was handed
// straight back by any device that still had the entry — the #132 defect in its list-membership form. remove()
// and clear() therefore date every entry they drop, in a per-profile tombstone namespace, keyed by the same
// key-else-path identity the union pass de-duplicates on.
//
// The cap is deliberately on the other side of that line. Dropping the 41st entry is the list running out of
// room rather than the user forgetting something, and tombstoning it would make the cap permanent: an item that
// scrolled off could never re-enter on a later re-watch. It needs no record — the merge unions and re-caps, so
// an evicted entry comes back only while it is still among the newest 40 overall. add()'s de-dup removal is a
// move-to-front and records nothing either; add() also LIFTS any tombstone, because re-opening an item is the
// user undoing their own removal of it.
//
// NO ENTRY HERE EVER CARRIES A CREDENTIAL (issue #200). "recent/" is matched by CloudSync::isPerItemStoreKey
// and by no device-local carve-out, so every row rides the CloudMerge document to every device on the
// account — and an addon-resolved stream's path is a SIGNED url whose query is a debrid/provider token. So
// add() scrubs what it is given (see StoredUrl.h for the rule and RecentStore.cpp for which field gets
// which), CredentialScrub::run() cleans what earlier builds already wrote, and CloudMerge scrubs what a peer
// sends. The row survives all three: a signed url is a one-shot artefact, the IDENTITY is `key`, and what is
// kept — scheme, host and path — is enough for the Home tile, the resume lookup, the de-dup and the kind
// dispatch. probe_cloudmerge §34-35 pins it.
#pragma once
#include <QString>
#include <QVector>

struct RecentItem
{
    QString path;   // absolute file path / URL to re-open
    QString title;  // display label
    QString kind;   // "video" | "audio" | "document" | "game" | "pcgame" | "steamgame" | "epicgame" | "goggame"
                    // | "battlenetgame"
                    // A "steamgame" is a native Steam launch: path is the steam://rungameid/<appid> URL, key is
                    // "steam:<appid>", thumb is the vertical capsule; re-opening hands the URL back to Steam.
                    // An "epicgame" is the same fire-and-forget shape for the Epic launcher: path is the
                    // com.epicgames.launcher://apps/<AppName> URI, key is "epic:<AppName>".
                    // A "goggame" is a DRM-free GOG exe: path IS the resolved exe, key is "gog:<id>". It re-opens
                    // through the MONITORED launchPcExe path (the pcgame LAUNCH MECHANICS) but keeps its own
                    // "goggame" KIND so it groups under the GOG console and never double-records as a pcgame.
                    // A "battlenetgame" is EITHER shape, decided per title: a game with a known product code
                    // records path = battlenet://<code> and re-opens fire-and-forget (the epicgame shape); a
                    // code-less one records path = its exe and re-opens through launchPcExe (the goggame shape).
                    // Key is "bnet:<code>" (coded) or "bnet:<DisplayName>" (code-less).
    QString thumb;  // optional poster image (path or http url); empty -> a type placeholder is drawn
    QString key;    // stable identity for resume + de-dup (e.g. an addon item id); empty -> use path. A
                    // streamed item's URL changes each session, so resume/de-dup key on this instead.
    QString system; // games only: the resolved SystemCatalog id (e.g. "psx", "gc") the game launched with,
                    // so re-opening picks the right console instead of guessing from a shared extension
                    // (.iso/.cue/.chd/.bin). Empty for non-games / legacy entries.
    qint64  ts = 0; // last-opened time (unix seconds); set on add(). Lets cross-device sync merge by recency.
};

namespace RecentStore
{
    QVector<RecentItem> list();          // newest first
    void add(const RecentItem& item);    // move-to-front + de-dup by path + cap
    void remove(const QString& pathOrKey); // drop the entry whose path or key matches
    void clear();

    // How a Recent of a given kind is re-launched (the pure dispatch table). "steamgame"/"pcgame" relaunch
    // through their native launchers; the media kinds re-open their recorded file/URL. MainWindow::openRecent
    // switches on this so the app and the headless probe share one definition of the dispatch.
    enum class Relaunch { SteamGame, EpicGame, GogGame, BattleNetGame, PcGame, Video, Audio, Document, Game, Unknown };
    Relaunch relaunchFor(const QString& kind);
}
