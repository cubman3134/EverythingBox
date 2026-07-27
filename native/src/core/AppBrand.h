#pragma once

// Every string that names this product. The rename that created this file touched 214 files and ~1000
// literals; collecting the load-bearing ones here means the next rename is a one-file change, and — more
// immediately — it is what makes "no mentions of the old name remain" a checkable property rather than a
// claim. Comments and prose are renamed textually; these are the ones code actually depends on.
namespace AppBrand
{
    inline constexpr const char* kName        = "EverythingBox";
    // The human-facing spaced form. Identical to kName here only because "EverythingBox" has no spaced
    // variant (the design maps BOTH "MyMediaVault" and "My Media Vault" onto it) — the two are separate
    // constants because Legacy's forms genuinely differ, and because kDisplayName feeds
    // QApplication::setApplicationName, which is load-bearing for paths (see Legacy::kDisplayName).
    inline constexpr const char* kDisplayName = "EverythingBox";
    inline constexpr const char* kIniFile     = "everythingbox.ini";
    inline constexpr const char* kDriveFolder = "EverythingBox";
    inline constexpr const char* kSyncZip     = "everythingbox-sync.zip";
    inline constexpr const char* kProgressDoc = "everythingbox-progress.json";
    inline constexpr const char* kAddonPrefix = "com.everythingbox.";
    inline constexpr const char* kUserAgent   = "EverythingBox";
    inline constexpr const char* kConfigHeader= "X-EB-Config";
    inline constexpr const char* kEnvPrefix   = "EB_";

    // The previous identity. Referenced ONLY by BrandMigration and by the lookups that tolerate it until
    // migration is confirmed. Nothing else in the tree may name these — the probe gate enforces that.
    namespace Legacy
    {
        inline constexpr const char* kName        = "MyMediaVault";
        // DATA-BEARING on mobile, not prose. This is the value currently passed to
        // QApplication::setApplicationName, and on Android/iOS AppPaths::dataDir() resolves through
        // QStandardPaths::AppDataLocation, which incorporates applicationName (on iOS literally
        // ~/Library/Application Support/<applicationName>). Swapping it for kDisplayName relocates the
        // entire mobile data directory — ini, saves, states, addons — with no migration, i.e. a silent
        // wipe. It stays legacy at the setApplicationName call site until the brand migration owns the
        // mobile path move; see the comment there.
        inline constexpr const char* kDisplayName = "My Media Vault";
        inline constexpr const char* kIniFile     = "mymediavault.ini";
        inline constexpr const char* kDriveFolder = "MyMediaVault";
        inline constexpr const char* kSyncZip     = "mymediavault-sync.zip";
        inline constexpr const char* kProgressDoc = "mymediavault-progress.json";
        inline constexpr const char* kAddonPrefix = "com.mymediavault.";

        // DATA-BEARING, not prose. The parental PIN is stored as SHA-256(salt + pin) (Settings::pinHash),
        // so the salt is an INPUT to a hash already written to every existing user's ini. Renaming it does
        // not rename anything — it changes the function, so no existing PIN ever matches again and the
        // parental lock cannot be opened with the PIN the user set. It stays legacy forever, or until
        // something re-derives every stored hash, which is impossible without the plaintext PINs.
        inline constexpr const char* kParentalPinSalt = "mmv-parental:";
    }
}
