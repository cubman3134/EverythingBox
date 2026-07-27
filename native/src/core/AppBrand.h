#pragma once

// Every string that names this product. The rename that created this file touched 214 files and ~1000
// literals; collecting the load-bearing ones here means the next rename is a one-file change, and — more
// immediately — it is what makes "no mentions of the old name remain" a checkable property rather than a
// claim. Comments and prose are renamed textually; these are the ones code actually depends on.
namespace AppBrand
{
    inline constexpr const char* kName        = "EverythingBox";
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
        inline constexpr const char* kIniFile     = "mymediavault.ini";
        inline constexpr const char* kDriveFolder = "MyMediaVault";
        inline constexpr const char* kSyncZip     = "mymediavault-sync.zip";
        inline constexpr const char* kProgressDoc = "mymediavault-progress.json";
        inline constexpr const char* kAddonPrefix = "com.mymediavault.";
    }
}
