// The Google Drive half of the brand migration, plus run() — the orchestration that walks all four steps
// safest-first. Split out of BrandMigration.cpp so that the QtCore-only callers that merely consult a flag
// (AddonManager's reserved-namespace guard, CloudSync's first-party exclusion) don't drag Qt Network in.
//
// Every Drive step is written to be SAFE WHEN IT FAILS: a failure leaves the remote exactly as it was and
// leaves the flag unset, so the next launch simply tries again. Critically, an unreachable Drive is never
// mistaken for an empty one — the flag is set only on a query that actually succeeded, because a flag set on
// a network error would permanently skip a rename that never happened.
#include "BrandMigration.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "CloudSync.h"
#include "DriveSyncBackend.h"  // brand migration is Drive-only: force the Drive backend, never the config-chosen one

#include <QString>

namespace
{

// Rename ONE document inside the app folder. cb(true) also covers "there was nothing under the old name" and
// "a file already exists under the new name" — both are states with no work left to do. cb(false) means the
// remote could not be read or written, so the caller must not flag the step.
void renameOne(CloudSync* cloud, const QString& folderId, const QString& legacyName, const QString& newName,
               std::function<void(bool)> cb)
{
    cloud->findFile(folderId, legacyName, [cloud, folderId, newName, cb](bool listOk, const QString& id,
                                                                        const QString&, const QString&) {
        if (!listOk) { cb(false); return; }          // couldn't reach Drive — retry next launch
        if (id.isEmpty()) { cb(true); return; }      // provably nothing under the old name
        cloud->findFile(folderId, newName, [cloud, id, newName, cb](bool ok2, const QString& existing,
                                                                    const QString&, const QString&) {
            if (!ok2) { cb(false); return; }
            // A document already sits under the new name — another device migrated first. Renaming now would
            // leave two same-named files in one folder, and findFile takes files.first(), so which one wins
            // becomes Drive's choice. Leave the legacy copy as it is; it is a backup, not a conflict.
            if (!existing.isEmpty()) { cb(true); return; }
            cloud->renameFile(id, newName, cb);
        });
    });
}

// Step 3 — the Drive folder. A rename, never a create-and-move: the folder id is unchanged, so nothing inside
// it moves, no sharing is lost, and a failure mid-way is indistinguishable from not having started.
void migrateDriveFolder(CloudSync* cloud, std::function<void(bool)> cb)
{
    using S = BrandMigration::Step;
    if (BrandMigration::done(S::DriveFolder)) { cb(true); return; }

    cloud->findFolderNamed(QString::fromLatin1(AppBrand::kDriveFolder),
                           [cloud, cb](bool queryOk, const QString& id) {
        if (!queryOk) { cb(false); return; }
        // Already under the current name (this device, or a peer, got there first).
        if (!id.isEmpty()) { BrandMigration::setDone(S::DriveFolder, true); cb(true); return; }
        cloud->findFolderNamed(QString::fromLatin1(AppBrand::Legacy::kDriveFolder),
                               [cloud, cb](bool legacyOk, const QString& legacyId) {
            if (!legacyOk) { cb(false); return; }
            // Neither folder exists: nothing was ever synced from this account, so there is nothing to
            // rename. ensureFolder will create the folder under the current name when a sync first needs it.
            if (legacyId.isEmpty()) { BrandMigration::setDone(S::DriveFolder, true); cb(true); return; }
            cloud->renameFile(legacyId, QString::fromLatin1(AppBrand::kDriveFolder), [cb](bool ok) {
                if (ok) BrandMigration::setDone(S::DriveFolder, true);
                cb(ok);
            });
        });
    });
}

// Step 4 — the two documents inside the folder (the state bundle and the progress doc). Runs after the folder
// step, so the folder resolves under the current name.
void migrateDriveFiles(CloudSync* cloud, std::function<void(bool)> cb)
{
    using S = BrandMigration::Step;
    if (BrandMigration::done(S::DriveFiles)) { cb(true); return; }

    cloud->findFolderNamed(QString::fromLatin1(AppBrand::kDriveFolder),
                           [cloud, cb](bool queryOk, const QString& folderId) {
        if (!queryOk) { cb(false); return; }
        // No folder under the current name. Either the folder step hasn't succeeded yet (in which case its
        // flag is unset and both steps run again next launch) or the account never synced. Either way there
        // is provably nothing to rename here, and creating a folder just to look inside it would be wrong.
        if (folderId.isEmpty()) { BrandMigration::setDone(S::DriveFiles, true); cb(true); return; }
        renameOne(cloud, folderId, QString::fromLatin1(AppBrand::Legacy::kSyncZip),
                  QString::fromLatin1(AppBrand::kSyncZip), [cloud, folderId, cb](bool okZip) {
            if (!okZip) { cb(false); return; }
            renameOne(cloud, folderId, QString::fromLatin1(AppBrand::Legacy::kProgressDoc),
                      QString::fromLatin1(AppBrand::kProgressDoc), [cb](bool okDoc) {
                if (okDoc) BrandMigration::setDone(S::DriveFiles, true);
                cb(okDoc);
            });
        });
    });
}

bool localStepsDone()
{
    return BrandMigration::done(BrandMigration::Step::LocalIni)
        && BrandMigration::done(BrandMigration::Step::AddonIds);
}

} // namespace

void BrandMigration::run(std::function<void(bool allDone)> cb)
{
    const QString dir = AppPaths::dataDir();

    // Safest-first, and LocalIni strictly first: setting ANY flag writes to the new ini, and once that file
    // exists the ini step has to distinguish "already migrated" from "about to be lost". Running it before
    // any other step can write means that ambiguity never arises on a real install.
    migrateLocalIni(dir);
    migrateAddonIds(dir);

    // Drive is opt-in. When it isn't configured, or this device has never signed in, the two Drive flags stay
    // UNSET on purpose: there may still be a folder under the previous name waiting for the day the user signs
    // in, and a flag set now would skip it forever. The migration is idempotent and runs every launch, so it
    // will be there when they do.
    if (!CloudSync::isConfigured()) { cb(localStepsDone()); return; }
    // Force the Drive backend regardless of cloud/backend: this migration renames the OLD brand's Drive folder
    // and files, which is meaningless on the server backend, and isConfigured() above gates it on Drive anyway.
    auto* cloud = new CloudSync(new DriveSyncBackend());
    if (!cloud->isSignedIn()) { cloud->deleteLater(); cb(localStepsDone()); return; }

    migrateDriveFolder(cloud, [cloud, cb](bool) {
        migrateDriveFiles(cloud, [cloud, cb](bool) {
            cloud->deleteLater();
            cb(localStepsDone() && done(Step::DriveFolder) && done(Step::DriveFiles));
        });
    });
}
