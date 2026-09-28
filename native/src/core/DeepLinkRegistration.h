// everythingbox:// deep links (issue #80): telling the OS that this app opens them. Per user, never admin.
//
// Only while the user has "Open everythingbox:// links" on (Settings::deepLinksEnabled, default off):
//   * Windows — HKEY_CURRENT_USER\Software\Classes\<scheme>, written from DeepLink::windowsRegisterPlan (the
//     plan is pure and probe-pinned; this file only executes it). Turning the setting off deletes that key.
//     A moved exe is re-registered on the next launch, because apply(true) at startup compares the stored
//     command with this exe's and rewrites it when they differ.
//   * Linux — ~/.local/share/applications/<scheme>-url-handler.desktop (DeepLink::linuxDesktopEntry), made the
//     default handler with `xdg-mime default`. Off removes the file. Inside an AppImage the registered path is
//     the AppImage itself ($APPIMAGE), not the temporary mount the process runs from.
//   * macOS — the scheme is declared in the bundle's Info.plist (CFBundleURLTypes), which LaunchServices reads
//     on its own; there is nothing to write, and the setting decides only whether a received link is acted on.
//
// The scheme is DeepLinkChannel::registrationScheme(): under the UI-test channel that is a TEST scheme or
// nothing at all, so a test rig can never write the real key on the machine running it.
#pragma once
#include <QString>

namespace DeepLinkRegistration
{
struct Outcome
{
    bool    ok = false;
    bool    changed = false;   // something was written or removed
    QString scheme;            // the scheme acted on ("" when none may be registered here)
    QString detail;            // for the log: what was done or why not (never a URL)
};

// The path the OS should launch for a link: this exe, or the AppImage around it.
QString launcherPath();

// Register (enable) or unregister (disable) for this user.
Outcome apply(bool enable);
} // namespace DeepLinkRegistration
