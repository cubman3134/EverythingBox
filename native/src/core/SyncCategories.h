#pragma once
#include <QCoreApplication>   // translate(): the category names are user-facing, and this header has no QObject
#include <QSettings>
#include <QString>

// WHAT CLOUD SYNC CARRIES, IN THE USER'S WORDS (issue #27, increments 1 and 2).
//
// Sync scope used to be decided entirely in code: the device-local carve-out (CloudSync::isDeviceLocalKey), the
// per-item-store table (PerItemStores.h) and the add-on roster's own exclusions. Nothing a user could read said
// what actually leaves the device. This header names every key the settings bundle or the merge document can
// carry as ONE of a small set of categories, so Cloud Sync can list them and the user can turn most of them off.
//
// THE RULES, decided on the issue and not reopened here:
//   * Every synced key maps to exactly one category. A key no rule claims is Unmapped: at run time it syncs as
//     "other settings" (behaviour unchanged), and probe_cloudmerge section 46 fails on it so a new settings
//     group cannot ship without being named here.
//   * OFF MEANS NOT PUSHED AND NOT APPLIED. IT NEVER MEANS DELETED. A disabled category's keys leave the
//     settings bundle and the sync fingerprint; incoming values for it are ignored; its merge-document sections
//     are neither serialised from this device nor merged into it. Nothing is tombstoned because it is absent,
//     here or on a peer: absence is how every merge in this app already reads "I have nothing to say".
//   * The switches are DEVICE-LOCAL. They live under cloud/sync/<id>, which the cloud/ carve-out keeps out of
//     the bundle and the fingerprint, and which SettingsTxn keeps out of Discard's reach.
//   * They default to ON, so nothing changes until someone turns one off.
//   * Accounts & sign-ins has NO switch. Whether credentials keep syncing is the owner's call (the issue's
//     comments); until it is made they sync exactly as before, and the page says so.
//   * Other settings has no switch either: it carries the profile list every per-profile category is keyed
//     under, each profile's passcode hash, and the catch-all the next feature's keys land in, so "off" would have
//     no stable meaning and could silently stop a parental-control change from reaching a child's device.
//   * Device-local keys never sync, as before. They are listed so the page can say so.
//
// PURE ON PURPOSE: QtCore only, no store of its own. CloudSync and CloudMerge each pass their own QSettings, and
// the probes link this without anything else.
namespace synccat
{
enum class Category
{
    Progress,      // watch progress & resume
    Collections,   // favourites & playlists
    Marks,         // tags, marks & hidden
    Stats,         // play statistics
    Music,         // music
    Addons,        // add-ons & their configuration
    Appearance,    // appearance & theme
    Accounts,      // accounts & sign-ins        (shown, never switchable here)
    Other,         // other settings             (shown, never switchable here)
    DeviceLocal,   // never syncs                (shown for honesty)
    Unmapped,      // no rule claims it: syncs as Other, and the probe fails
};

// The order the page lists them in.
inline constexpr Category kListed[] = {
    Category::Progress, Category::Collections, Category::Marks, Category::Stats, Category::Music,
    Category::Addons, Category::Appearance, Category::Accounts, Category::Other, Category::DeviceLocal,
};

// The stable id: the leaf of the switch key and the UI row id. Never translated, never renamed.
inline const char* id(Category c)
{
    switch (c)
    {
        case Category::Progress:    return "progress";
        case Category::Collections: return "favorites";
        case Category::Marks:       return "marks";
        case Category::Stats:       return "stats";
        case Category::Music:       return "music";
        case Category::Addons:      return "addons";
        case Category::Appearance:  return "appearance";
        case Category::Accounts:    return "accounts";
        case Category::Other:       return "other";
        case Category::DeviceLocal: return "devicelocal";
        case Category::Unmapped:    return "unmapped";
    }
    return "unmapped";
}

inline QString displayName(Category c)
{
    switch (c)
    {
        case Category::Progress:    return QCoreApplication::translate("SyncCategories", "Watch progress & resume");
        case Category::Collections: return QCoreApplication::translate("SyncCategories", "Favourites & playlists");
        case Category::Marks:       return QCoreApplication::translate("SyncCategories", "Tags, marks & hidden");
        case Category::Stats:       return QCoreApplication::translate("SyncCategories", "Play statistics");
        case Category::Music:       return QCoreApplication::translate("SyncCategories", "Music");
        case Category::Addons:      return QCoreApplication::translate("SyncCategories", "Add-ons & their configuration");
        case Category::Appearance:  return QCoreApplication::translate("SyncCategories", "Appearance & theme");
        case Category::Accounts:    return QCoreApplication::translate("SyncCategories", "Accounts & sign-ins");
        case Category::Other:       return QCoreApplication::translate("SyncCategories", "Other settings");
        case Category::DeviceLocal: return QCoreApplication::translate("SyncCategories", "Device-local settings");
        case Category::Unmapped:    return QCoreApplication::translate("SyncCategories", "Other settings");
    }
    return QString();
}

// The seven categories a user may switch off. Not Accounts (the owner's decision), not Other (see the header
// comment), not DeviceLocal (it never syncs to begin with).
inline bool hasToggle(Category c)
{
    switch (c)
    {
        case Category::Progress: case Category::Collections: case Category::Marks: case Category::Stats:
        case Category::Music: case Category::Addons: case Category::Appearance:
            return true;
        default:
            return false;
    }
}

// "cloud/sync/<id>" — under cloud/, so CloudSync::isDeviceLocalKey already keeps it off every other device.
inline QString toggleKey(Category c) { return QStringLiteral("cloud/sync/") + QLatin1String(id(c)); }

// Whether keys of `c` sync on this device. Absent means ON; only a category with a switch can be off.
inline bool isEnabled(const QSettings& s, Category c)
{
    if (!hasToggle(c)) return true;
    return s.value(toggleKey(c), true).toBool();
}

// A category switched back ON after being off CATCHES UP before it is sent again: it takes a peer's values at
// once, but this device sends nothing of it until a pull has landed (see CloudSync::setCategoryEnabled). The
// marker is device-local for the switch's own reason.
inline QString catchUpGroup() { return QStringLiteral("cloud/synccatchup"); }
inline QString catchUpKey(Category c) { return catchUpGroup() + QLatin1Char('/') + QLatin1String(id(c)); }
inline bool isCatchingUp(const QSettings& s, Category c) { return hasToggle(c) && s.value(catchUpKey(c), false).toBool(); }
// Whether this device SENDS `c`: switched on and not catching up. (isEnabled alone answers whether it TAKES it.)
inline bool isSent(const QSettings& s, Category c) { return isEnabled(s, c) && !isCatchingUp(s, c); }

// ---- the key table --------------------------------------------------------------------------------------------
// Longest matching prefix wins, so an exact leaf (ebook/fontSize) can sit inside a group that means something
// else (ebook/<hash>/ is a reading position). Device-local keys are NOT decided here: CloudSync::categoryFor asks
// isDeviceLocalKey first, because several groups below hold a device-local leaf beside syncing siblings
// (roms/folder vs roms/autoApplyPatches). A new settings group goes HERE, once, with the category a user would
// look for it under.
struct Rule { const char* prefix; Category cat; };
inline constexpr Rule kRules[] = {
    // -- watch progress & resume: where you are in something
    { "resume/",            Category::Progress },    // per-item playback position (merge document)
    { "recent/",            Category::Progress },    // Continue Watching / recents (merge document)
    { "missed/",            Category::Progress },    // "you missed" dismissals (merge document)
    { "speed/",             Category::Progress },    // per-item playback speed (merge document)
    { "bookmarks/",         Category::Progress },    // book reading bookmarks (merge document)
    { "audiobookmarks/",    Category::Progress },    // audio bookmarks (merge document)
    { "comic/",             Category::Progress },    // comic page position (bundle)
    { "pdf/",               Category::Progress },    // PDF page position (bundle)
    { "ebook/",             Category::Progress },    // e-book reading position (bundle)
    { "audiobook/",         Category::Progress },    // pre-generalisation audiobook positions (bundle, legacy)
    // -- favourites & playlists: collections you curate
    { "favorites/",         Category::Collections },
    { "playlists/",         Category::Collections },
    { "follow/",            Category::Collections }, // followed series
    { "filterpresets/",     Category::Collections }, // saved filters
    { "channels/",          Category::Collections }, // personal TV channels
    { "vocabulary/",        Category::Collections }, // looked-up words
    // -- tags, marks & hidden: what you said about an item
    { "marks/",             Category::Marks },       // watched / hidden / tags / pinned tags
    { "highlights/",        Category::Marks },       // book highlights
    { "metaoverrides/",     Category::Marks },       // title / artwork corrections
    { "trackerlink/",       Category::Marks },       // which AniList entry a row is
    // -- play statistics
    { "stats/",             Category::Stats },
    { "playstats/",         Category::Stats },
    // -- music
    { "music/",             Category::Music },
    { "lyricoffset/",       Category::Music },       // per-track lyric nudge (merge document)
    { "playback/gaplessAudio",     Category::Music },
    { "playback/crossfadeSeconds", Category::Music },
    { "playback/replayGain",       Category::Music }, // and playback/replayGainPreamp
    { "playback/lyricsPanel",      Category::Music },
    { "playback/onlineLyrics",     Category::Music },
    // -- add-ons & their configuration
    { "addon.",             Category::Addons },      // the flat roster/seed keys (addon.remote.urls, ...)
    { "addoncfg/",          Category::Addons },      // an add-on's settings (its password fields are device-local)
    { "roster/",            Category::Addons },      // the roster's stamped shadow (merge document)
    { "registry/",          Category::Addons },      // registries the user added (legacy keys)
    // -- appearance & theme
    { "theme/",             Category::Appearance },
    { "themes/",            Category::Appearance },  // the bundle's theme FILES (not a settings key)
    { "themedHome/",        Category::Appearance },
    { "homerows/",          Category::Appearance },  // the home arrangement (merge document)
    { "bgm/",               Category::Appearance },  // menu background music
    { "attract/",           Category::Appearance },  // idle screensaver
    { "miximage/",          Category::Appearance },
    // -- accounts & sign-ins: synced exactly as before, and not switchable (the owner's decision)
    { "trakt/",             Category::Accounts },    // access, refresh, expiry, clientId, clientSecret (the caches
                                                     // and backfill cursor are device-local and caught earlier)
    { "ra/",                Category::Accounts },    // RetroAchievements (ra/token and ra/user are device-local)
    { "steam/",             Category::Accounts },    // Steam API key + SteamID
    { "lastfm/",            Category::Accounts },    // Last.fm session
    { "lb/",                Category::Accounts },    // ListenBrainz (legacy flat keys)
    { "debrid/",            Category::Accounts },    // debrid API keys
    { "subs/osApiKey",      Category::Accounts },    // OpenSubtitles API key (the login is device-local)
    // -- other settings: the rest of the bundle, named group by group so a NEW group is Unmapped until placed
    { "ebook/fontSize",     Category::Other },
    { "playback/",          Category::Other },
    { "subs/",              Category::Other },
    { "video/",             Category::Other },
    { "gestures/",          Category::Other },
    { "reader/",            Category::Other },
    { "reading/",           Category::Other },
    { "readaloud/",         Category::Other },
    { "library/",           Category::Other },
    { "profiles/",          Category::Other },       // profiles/list (and each profile's passcode hash)
    { "parental/",          Category::Other },
    { "sync/",              Category::Other },       // sync/global/* A/V offsets (sync/files/* is device-local)
    { "general/",           Category::Other },
    { "display/",           Category::Other },
    { "onboarding/",        Category::Other },
    { "player/",            Category::Other },
    { "emu/",               Category::Other },
    { "emulators/",         Category::Other },
    { "cores/",             Category::Other },
    { "backends/",          Category::Other },
    { "retropark/",         Category::Other },
    { "opt/",               Category::Other },
    { "optdesc/",           Category::Other },
    { "optgame/",           Category::Other },
    { "pad/",               Category::Other },
    { "padgame/",           Category::Other },
    { "padscope/",          Category::Other },
    { "kbd/",               Category::Other },
    { "kbdgame/",           Category::Other },
    { "kbdscope/",          Category::Other },
    { "turbo/",             Category::Other },
    { "runaheadgame/",      Category::Other },
    { "input/",             Category::Other },
    { "launchopts/",        Category::Other },       // per-game launch overrides (merge document)
    { "pad2key/",           Category::Other },       // per-game pad-to-keyboard (merge document)
    { "roms/",              Category::Other },
    { "dos/",               Category::Other },
    { "ps3/",               Category::Other },
    { "netplay/",           Category::Other },
    { "watchtogether/",     Category::Other },
    { "remote/",            Category::Other },
    { "content/",           Category::Other },
    { "following/",         Category::Other },       // new-episode check schedule (follow/ is the list)
    { "photos/",            Category::Other },
    { "audiobooks/",        Category::Other },
    { "interstitials/",     Category::Other },
    { "scrape/",            Category::Other },
    { "cache/",             Category::Other },
    { "recomps/",           Category::Other },
    { "hashverify/",        Category::Other },
    { "pcgameremap/",       Category::Other },
    { "debug/",             Category::Other },
    { "epgcache/",          Category::Other },       // Live TV guide fetch stamp (syncs today; see #27's report)
};

// The merge document's root sections, each with the per-item store it carries. A section's category is its
// store's; resume and recent each carry a second, tombstone section that follows them.
struct Section { const char* name; const char* prefix; };
inline constexpr Section kSections[] = {
    { "resume", "resume/" },        { "resumeTombs", "resume/" },
    { "recent", "recent/" },        { "recentTombs", "recent/" },
    { "marks", "marks/" },          { "favorites", "favorites/" },
    { "follow", "follow/" },        { "bookmarks", "bookmarks/" },
    { "highlights", "highlights/" },{ "vocabulary", "vocabulary/" },
    { "audiobookmarks", "audiobookmarks/" },
    { "playlists", "playlists/" },  { "presets", "filterpresets/" },
    { "channels", "channels/" },    { "homerows", "homerows/" },
    { "metaoverrides", "metaoverrides/" },
    { "launchopts", "launchopts/" },{ "pad2key", "pad2key/" },
    { "speed", "speed/" },          { "lyricoffset", "lyricoffset/" },
    { "trackerlink", "trackerlink/" },
    { "missed", "missed/" },        { "roster", "roster/" },
    { "stats", "stats/" },          { "playstats", "playstats/" },
};

// The category the table gives `key`, or Unmapped. Pure: no device-local test (CloudSync::categoryFor adds it).
inline Category ofKey(const QString& key)
{
    // A tombstone belongs to the store it deletes from: deleted/<store>/... is <store>/...'s category.
    static const QLatin1String kDeleted("deleted/");
    if (key.startsWith(kDeleted)) return ofKey(key.mid(kDeleted.size()));
    Category best = Category::Unmapped;
    int bestLen = -1;
    for (const Rule& r : kRules)
    {
        const QLatin1String p(r.prefix);
        if (p.size() > bestLen && key.startsWith(p)) { best = r.cat; bestLen = int(p.size()); }
    }
    return best;
}

// The per-item store prefix a merge-document section carries, or empty for a section this build does not know.
inline QString sectionPrefix(const QString& section)
{
    for (const Section& s : kSections)
        if (section == QLatin1String(s.name)) return QLatin1String(s.prefix);
    return QString();
}

// Where CloudMerge keeps a PEER's copy of a section this device has switched off, to pass it back up untouched
// (see CloudMerge::serializeAll). Under cloud/, so it is device-local like the switches themselves.
inline QString carryGroup() { return QStringLiteral("cloud/carry"); }
inline QString carryKey(const QString& section) { return carryGroup() + QLatin1Char('/') + section; }

inline Category ofSection(const QString& section)
{
    const QString p = sectionPrefix(section);
    return p.isEmpty() ? Category::Unmapped : ofKey(p);
}

// What a key syncs AS: an unclaimed key keeps syncing, as "other settings", exactly as it did before #27.
inline Category effective(Category c) { return c == Category::Unmapped ? Category::Other : c; }
} // namespace synccat
