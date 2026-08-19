#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>

// PS3 firmware auto-install for RPCS3. RPCS3 cannot boot any game without Sony's console firmware
// (dev_flash), and a fresh auto-downloaded RPCS3 has none — historically a manual one-time user step.
// This unit detects a missing/incomplete dev_flash, locates the official PS3UPDAT.PUP via Sony's public
// updatelist feed, and drives a headless `rpcs3.exe --installfw` — with every external effect (network,
// disk download, process spawn) injected as a seam so the pipeline is probe-testable offline.
namespace Ps3Firmware {

struct Info {
    QString version; // SystemSoftwareVersion, e.g. "4.9200" (may be empty if the feed omits it)
    QString url;     // absolute http(s) url of PS3UPDAT.PUP, from the CDN= field
};

enum class Os { Windows, MacOS, Linux };

// The directory RPCS3 keeps dev_flash under, per OS. Pure — every branch is exercisable from the probe
// on any host: Windows = binDir (RPCS3 runs portable there, config next to the exe), macOS =
// <home>/Library/Application Support/rpcs3, Linux = <xdgConfigHome>/rpcs3 falling back to
// <home>/.config/rpcs3 when XDG_CONFIG_HOME is unset/empty. Mirrors RPCS3's fs::get_config_dir().
QString devFlashRoot(Os os, const QString& binDir, const QString& home, const QString& xdgConfigHome);

// Host-OS convenience: picks the running OS and reads home/XDG from the real environment.
QString devFlashRoot(const QString& binDir);

// Firmware present? True iff <fwRoot>/dev_flash/vsh/etc/version.txt exists and is non-empty — the file
// RPCS3 itself reads to display the installed firmware version. fwRoot is the *firmware root*, i.e. the
// directory dev_flash lives under: binDir on Windows (portable install), the devFlashRoot() result
// elsewhere. An empty file counts as incomplete: better to reinstall than to boot into RPCS3's
// missing-firmware error.
bool installed(const QString& fwRoot);

// Parses Sony's ps3-updatelist.txt: one record per line, each a run of ;-separated Key=Value fields.
// Returns version + url from the first line carrying an http(s) CDN= field (other lines are
// compatibility records without one); nullopt when no line qualifies (or the body is empty/garbage).
std::optional<Info> parseUpdateList(const QByteArray& body);

using FeedFetcher = std::function<std::optional<QByteArray>()>;
using Downloader  = std::function<bool(const QString& url, const QString& destPath)>;
using Installer   = std::function<int(const QString& rpcs3Exe, const QString& pupPath)>;
using Progress    = std::function<void(const QString& message)>;

// The whole pipeline: if firmware is already installed, does nothing. Otherwise fetch the updatelist,
// parse it, download the PUP into tmpDir, run the installer, delete the PUP, and report success only
// if dev_flash actually appeared (an installer exit code alone is not proof). Any failure returns
// false — callers treat that as "boot anyway" (RPCS3 then shows its own missing-firmware error) — and
// drops a `fw-install-failed` marker in tmpDir that backs the next attempt off for an hour, so a
// persistently failing install cannot re-download ~230MB before every launch.
// A failed installer run additionally scrubs any dev_flash version.txt it left behind, so a killed
// --installfw (timeout, or app-quit interruption) can never make installed() read half-true.
// fwRoot is the firmware root the caller resolved per-OS via devFlashRoot(); it is only ever read
// through installed().
bool maybeInstall(const QString& fwRoot, const QString& rpcs3Exe, const QString& tmpDir,
                  const FeedFetcher& fetch, const Downloader& download,
                  const Installer& install, const Progress& progress);

} // namespace Ps3Firmware
