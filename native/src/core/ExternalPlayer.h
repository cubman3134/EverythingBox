// External-player handoff (Stremio-style): detect installed desktop players (VLC / MPC-HC) or a user-picked
// custom exe, resolve the configured target, and hand a media URL/path off to it — instead of the built-in
// libmpv player. QtCore-only (QProcess/QFileInfo/QSettings live in QtCore), so this links into the app and
// runs headless in CI with no Quick/Widgets. Task 2 wires the settings UI + playback routing onto exactly
// these names.
//
// Detection is injectable for tests: detect(fsProbeRoot, regProbeRoot) — a non-empty fsProbeRoot confines the
// filesystem scan to that directory tree (never the real Program Files), and a non-empty regProbeRoot reads a
// fake registry (an INI file) instead of the live HKLM hive. Both empty = the real system. So the probe runs
// fully hermetic: point the roots at empty temp dirs and detection finds nothing, never leaking the host's
// actually-installed players. Probes NEVER launch anything and NEVER touch the registry/filesystem beyond the
// injected roots.
//
// Platform: desktop resolves VLC/MPC/Custom to an exe launched via QProcess::startDetached. On Android there
// are no exe paths — detect() returns just the AndroidIntent pseudo-entry and launch() fires an ACTION_VIEW
// intent (video/*); the whole Android branch is #ifdef Q_OS_ANDROID so desktop builds need no JNI headers.
#pragma once
#include <QString>
#include <QVector>

namespace ExternalPlayer
{
    enum class Kind { Builtin, Vlc, Mpc, Custom, AndroidIntent };

    struct Detected { Kind kind; QString path; QString display; };

    // Installed/available external targets. probeRoot overrides are for tests (empty = real system) and have
    // desktop-only meaning; on Android this returns just the single AndroidIntent pseudo-entry.
    QVector<Detected> detect(const QString& fsProbeRoot = QString(),
                             const QString& regProbeRoot = QString());

    Kind    configuredKind();   // Settings player/external -> Kind (unknown/empty -> Builtin)
    QString configuredPath();   // resolved exe for the configured kind (Custom -> externalPath; Vlc/Mpc -> detect())
    bool    available();        // a usable external target exists (config + detection + platform)

    // Hand the media off. Returns true on a successful handoff start. Desktop: QProcess::startDetached.
    // Android: ACTION_VIEW intent (video/*); false if no activity can handle it (the caller notifies).
    bool    launch(const QString& urlOrPath);
}
