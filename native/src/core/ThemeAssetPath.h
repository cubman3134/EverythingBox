// Where a theme's asset path is allowed to point.
//
// A theme manifest names its artwork, fonts and sounds as paths relative to the theme's own folder. Turning
// one of those into a path the engine will open is a pure string decision, and it is the decision that
// decides whether a theme can read files that are none of its business — so it lives here, on its own, with
// no filesystem in it, and probe_theme pins it (section 8). QtCore only, header-only: the classic theme
// parser and the probe both get it by including this file, with nothing to register in CMake.
#pragma once
#include <QDir>
#include <QString>

namespace ThemeAssetPath
{

// Resolve `path` (as written in a theme manifest) against `dir` (the theme's own folder). Returns the
// cleaned path, or an empty string if the manifest is asking for something outside its folder.
//
// REJECT, DO NOT SANITIZE. A manifest that names `../../../secret.png` is refused outright rather than
// trimmed back to something inside the folder: trimming invents a file the theme never asked for, and it
// turns a manifest bug into a silently different render. The theme registry's installer holds the same line
// on the paths it writes; this is the same policy applied to the paths an installed manifest then READS.
//
// Existence is deliberately not checked: the caller owns that, and keeping this half free of I/O is what
// lets the probe pin every escape shape without a fixture on disk.
inline QString resolve(const QString& dir, const QString& path)
{
    if (path.isEmpty() || dir.isEmpty()) return QString();

    const QString root = QDir::cleanPath(dir);
    if (root.isEmpty()) return QString();

    QString candidate;
    if (QDir::isAbsolutePath(path))
    {
        // An absolute path is judged by the same containment rule, so one inside the folder is simply the
        // same file spelled the long way. (Spelled with different case on Windows it is refused — a manifest
        // that ships to other machines has no business naming an absolute path in the first place.)
        candidate = QDir::cleanPath(path);
    }
    else
    {
        // Refused on EVERY platform, not just the one where the OS would act on them: a backslash is a
        // separator on Windows, and a colon makes a path drive-relative there. A manifest travels between
        // machines, so a path that escapes on Windows must not resolve to a merely odd filename elsewhere.
        if (path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))) return QString();
        candidate = QDir::cleanPath(root + QLatin1Char('/') + path);
    }

    // Anchored on the separator, so a SIBLING whose name merely extends this one ("…/NightMare" beside
    // "…/Night") is outside. A bare startsWith(root) would hand another theme's folder over as this one's.
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (!candidate.startsWith(prefix)) return QString();   // outside the folder — or the folder itself
    return candidate;
}

} // namespace ThemeAssetPath
