#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

// Reads what RPCS3 has actually written to disk for a title's game-update data. `rpcs3.exe --installpkg`
// stays open as the normal GUI after installing, so its exit is no signal at all — the installed version
// on disk is. RPCS3 records it as APP_VER in <gameDir>/PARAM.SFO (its own
// package_reader::check_target_app_version reads exactly this file/key).
namespace Ps3InstalledVersion {

// <rpcs3Root>/dev_hdd0/game/<titleId> — where RPCS3 lands a title's update data. rpcs3Root is the
// per-OS config root Ps3Firmware::devFlashRoot() resolves; dev_hdd0 sits beside dev_flash under it.
QString gameDir(const QString& rpcs3Root, const QString& titleId);

// APP_VER from <gameDir>/PARAM.SFO, falling back to VERSION when APP_VER is absent (some updates carry
// only the latter). nullopt when the file is missing/malformed or carries neither key.
std::optional<QString> installedVersion(const QString& gameDir);

// true iff installedVersion(gameDir) exists and is >= target — a version PAST the target counts, so a
// hand-installed newer update is never re-run.
bool reachedTarget(const QString& gameDir, const QString& targetVersion);

// A stable fingerprint of every FILE under gameDir (recursive): two scans of an unchanged tree are
// byte-identical, any write changes it. This is the quiescence half of the install predicate — pkg
// entries extract IN PLACE in entry order, so PARAM.SFO can land long before the rest of the update
// and version-at-target alone must never be taken as "done".
//
// It is a fingerprint rather than "newest mtime" because on Windows/NTFS a path-based lastModified()
// reads the DIRECTORY ENTRY, which is updated LAZILY — typically only when the writer closes or
// flushes the handle. One large payload file being written for minutes with no other file activity
// looks perfectly quiet to a mtime scan, so RPCS3 gets killed mid-write and the truncated update is
// recorded as applied. The HANDLE-based QFile::size() (we open each file to ask) is real-time, so a
// growing file always moves the fingerprint. The mtime is folded in too as extra churn signal; its
// laziness can only make the caller wait longer, which is the safe direction.
//
// nullopt means "definitely busy": some file could not be opened (the writer holds it exclusively).
// A missing dir or a dir with no files is NOT busy — it is "nothing there yet" — and yields a valid,
// stable value. `abort` is polled once per file and also yields nullopt, which keeps the app-quit join
// bound real on a huge tree (the scan itself, not just the poll loop around it, gives up on quit).
std::optional<QByteArray> dirFingerprint(const QString& gameDir,
                                         const std::function<bool()>& abort = {});

// Entry-state capture/rollback for <gameDir>/PARAM.SFO, around an --installpkg run that may be KILLED
// (app-quit interruption, or the 10-minute wedge deadline). PARAM.SFO extracts EARLY — that is the
// whole premise of the quiescence wait — so a killed run leaves the file claiming the TARGET version
// over a truncated tree. Nothing that run wrote is trustworthy, and if the lie survives, the next
// launch's already-applied check skips the whole chain before downloading it and records the truncated
// update as permanently applied. Restoring the bytes that were there on entry makes the next launch see
// the truth and re-run the chain, which heals the tree (pkg entries overwrite in place).
//
// The firmware twin scrubs its version.txt after a killed run (Ps3Firmware.cpp) — this is restore, not
// delete, because a game dir may legitimately already hold an OLDER version from a previously completed
// update, and RPCS3's check_target_app_version refuses a version-dependent patch when no installed
// PARAM.SFO exists at all, so deleting could wedge the very retry this is protecting.
//
// snapshotSfo returns a NULL QByteArray (isNull(), distinct from empty) when the file is absent.
QByteArray snapshotSfo(const QString& gameDir);
// prior.isNull() → remove the file; otherwise write prior back verbatim (creating gameDir if needed).
void restoreSfo(const QString& gameDir, const QByteArray& prior);

} // namespace Ps3InstalledVersion
