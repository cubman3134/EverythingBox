#pragma once
#include <QDateTime>
#include <QString>
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

// Seconds since the newest lastModified of any FILE under gameDir (recursive), relative to nowUtc;
// -1 when the dir is missing or holds no files. pkg entries extract IN PLACE in entry order, so
// PARAM.SFO can land long before the rest of the update — version-at-target alone must never be taken
// as "done". This is the quiescence half of that predicate.
qint64 secsSinceNewestWrite(const QString& gameDir, const QDateTime& nowUtc);

} // namespace Ps3InstalledVersion
