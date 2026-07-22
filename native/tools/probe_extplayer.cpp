// Headless check of the ExternalPlayer detection/handoff core (src/core/ExternalPlayer) — the foundation the
// external-player settings UI + playback routing (Task 2) wire onto. ExternalPlayer is a QtCore-only wrapper
// (QProcess/QFileInfo/QSettings — no Quick/Widgets), so this runs under the offscreen QPA in CI and pins the
// contract Task 2 leans on:
//
//   * a fake fs root containing VideoLAN/VLC/vlc.exe + MPC-HC/mpc-hc64.exe -> detect(fsRoot, regRoot) finds
//     BOTH, with the right Kind and the exact resolved exe path;
//   * an EMPTY fs root (an existing but empty dir) -> no detections, and crucially NO real-system leakage:
//     both probe roots are injected, so the host's actually-installed players are never consulted;
//   * Settings round-trip: externalPlayer()/externalPlayerPath() persist, and configuredKind() maps the
//     stored string to a Kind with any unknown/empty string collapsing to Builtin;
//   * configuredPath() resolves Custom -> the stored custom path.
//
// Prints EXTPLAYER-OK on success; any failure prints EXTPLAYER-FAIL <cond> and exits non-zero.
//
// Isolation: like the other core probes (see probe_sync), AppPaths::dataDir() is the probe exe's own folder
// in the build tree (portable app), so the mymediavault.ini it reads/writes is next to the probe and never
// touches a deployed install. We wipe the "player" group at start so a leftover ini can't skew the asserts.
// The fs/reg roots live in a QTemporaryDir wiped when the probe exits — nothing outside it is written.
#include "ExternalPlayer.h"
#include "Settings.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "EXTPLAYER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Create an empty file at path, making parent dirs first. Returns the path.
static QString touchExe(const QString& dir, const QString& rel)
{
    const QString full = dir + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile f(full);
    f.open(QIODevice::WriteOnly);
    f.close();
    return QFileInfo(full).absoluteFilePath();
}

static const ExternalPlayer::Detected* find(const QVector<ExternalPlayer::Detected>& v,
                                            ExternalPlayer::Kind k)
{
    for (const auto& d : v) if (d.kind == k) return &d;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Reset: wipe any leftover player/* keys so the Settings asserts start from defaults.
    {
        QSettings reset(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        reset.remove(QStringLiteral("player"));
        reset.sync();
    }

    using ExternalPlayer::Kind;

    // A fake Program-Files-style root holding both players, and an empty root, plus a fake (empty) registry
    // ini path. All under one temp dir wiped on exit — no real-system files are read or written.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString fsRoot    = tmp.path() + QStringLiteral("/pf");
    const QString emptyRoot = tmp.path() + QStringLiteral("/empty");
    const QString regRoot   = tmp.path() + QStringLiteral("/fakereg.ini"); // non-empty => real registry skipped
    QDir().mkpath(emptyRoot);

    const QString vlcExe = touchExe(fsRoot, QStringLiteral("VideoLAN/VLC/vlc.exe"));
    const QString mpcExe = touchExe(fsRoot, QStringLiteral("MPC-HC/mpc-hc64.exe"));

    // 1. Fake fs root -> both players detected with the right kind + exact resolved path.
    {
        const auto found = ExternalPlayer::detect(fsRoot, regRoot);
        const auto* vlc = find(found, Kind::Vlc);
        const auto* mpc = find(found, Kind::Mpc);
        CHECK(vlc != nullptr);
        CHECK(mpc != nullptr);
        if (vlc) CHECK(QFileInfo(vlc->path) == QFileInfo(vlcExe));
        if (mpc) CHECK(QFileInfo(mpc->path) == QFileInfo(mpcExe));
        if (vlc) CHECK(!vlc->display.isEmpty());
    }

    // 2. Empty (existing but bare) root -> nothing found, and NO leakage of the host's real installs (both
    //    probe roots are injected, so the real Program Files / registry are never consulted).
    {
        const auto found = ExternalPlayer::detect(emptyRoot, regRoot);
        CHECK(find(found, Kind::Vlc) == nullptr);
        CHECK(find(found, Kind::Mpc) == nullptr);
        CHECK(found.isEmpty());
    }

    // 3. Settings round-trip + configuredKind() string->Kind mapping (unknown/empty => Builtin).
    Settings::setExternalPlayer(QStringLiteral("vlc"));
    CHECK(Settings::externalPlayer() == QStringLiteral("vlc"));
    CHECK(ExternalPlayer::configuredKind() == Kind::Vlc);

    Settings::setExternalPlayer(QStringLiteral("mpc"));
    CHECK(ExternalPlayer::configuredKind() == Kind::Mpc);

    Settings::setExternalPlayer(QStringLiteral("custom"));
    CHECK(ExternalPlayer::configuredKind() == Kind::Custom);

    Settings::setExternalPlayer(QStringLiteral("builtin"));
    CHECK(ExternalPlayer::configuredKind() == Kind::Builtin);

    Settings::setExternalPlayer(QStringLiteral("wat-is-this")); // unknown => Builtin
    CHECK(ExternalPlayer::configuredKind() == Kind::Builtin);

    Settings::setExternalPlayer(QString());                     // empty => Builtin
    CHECK(ExternalPlayer::configuredKind() == Kind::Builtin);

    // 4. Custom path round-trips and configuredPath() resolves the Custom kind to it.
    Settings::setExternalPlayerPath(mpcExe);
    CHECK(Settings::externalPlayerPath() == mpcExe);
    Settings::setExternalPlayer(QStringLiteral("custom"));
    CHECK(ExternalPlayer::configuredPath() == mpcExe);
    // A Custom target pointing at an existing exe is "available"; Builtin never is.
    CHECK(ExternalPlayer::available());
    Settings::setExternalPlayer(QStringLiteral("builtin"));
    CHECK(!ExternalPlayer::available());

    // Restore a clean player group so a rerun starts from defaults.
    {
        QSettings reset(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"), QSettings::IniFormat);
        reset.remove(QStringLiteral("player"));
        reset.sync();
    }

    if (failures == 0) { std::puts("EXTPLAYER-OK"); return 0; }
    std::fprintf(stderr, "EXTPLAYER: %d check(s) failed\n", failures);
    return 1;
}
