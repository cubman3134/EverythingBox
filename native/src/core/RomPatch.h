// ROM soft-patching: apply a sidecar IPS / BPS / UPS patch to a ROM at launch time, producing a *derived*
// patched file so the original ROM on disk is never touched. This is the client-side equivalent of what
// RetroArch does automatically for a `Game.ips` sitting next to `Game.sfc` — the romhack / translation-patch
// world, with the originals never at risk.
//
// Two layers:
//   * apply()/detectFormat() are the pure, in-memory appliers. They take a source byte buffer and a patch
//     byte buffer and produce the patched bytes. Format is decided by the patch's *magic*, not its name, and
//     BPS/UPS embed a source CRC32 which is verified — a patch built for a different ROM is refused rather
//     than silently producing a corrupt game. These are what probe_softpatch exercises against hand-built
//     fixtures, and they never mutate the source.
//   * resolvePatchedRom() is the launch-seam entry point: given a ROM path it looks for a sidecar patch,
//     applies it into a content-addressed cache file, and returns that path (or the original path unchanged
//     when there is no patch). GameLauncher::prepareCore() calls it for both libretro and standalone launches.
//
// The three formats are small, fully-specified binary standards implemented in-tree with no dependency.
// xdelta is deliberately out of scope (bigger format, disc-image streaming — see issue #128).
#pragma once
#include <QByteArray>
#include <QString>

namespace RomPatch
{
    enum class Format { None, Ips, Bps, Ups };

    // Sidecar patch extensions we recognise by name when hunting beside a ROM (content still decides the
    // format once a candidate is found). Lower-case, no dot.
    bool isPatchExtension(const QString& suffixLower);

    // Decide a patch's format from its leading bytes (its magic): "PATCH" = IPS, "UPS1" = UPS, "BPS1" = BPS.
    // Returns None for anything else — a `.ips` file that is not actually an IPS patch resolves to None here
    // and is refused by apply(), never launched as if it were valid.
    Format detectFormat(const QByteArray& patch);

    // Apply `patch` to `source`, writing the patched bytes to `out`. Returns false (with *error set, if given)
    // on any malformed patch, a magic we do not recognise, or — for BPS/UPS — a source-checksum mismatch,
    // which means the patch was built for a different ROM. `source` is never modified. Deterministic: the same
    // source + patch always yields the same `out`.
    bool apply(const QByteArray& source, const QByteArray& patch, QByteArray& out, QString* error = nullptr);

    // Launch-seam resolver. If a sidecar patch exists beside `romPath` (same folder + base name, a recognised
    // patch extension), apply it into a derived cache file and return that file's path; the original ROM is
    // left byte-for-byte untouched. Returns `romPath` unchanged when there is no sidecar. On a patch that
    // exists but fails to apply (bad magic, checksum mismatch, corrupt) returns an empty string with *error
    // set — the caller must surface that and NOT fall through to launching the unpatched ROM silently.
    //
    // The cache file is content-addressed on the ROM+patch bytes, so it is deterministic and idempotent: a
    // relaunch with an already-valid cached result re-uses it without re-patching. Disabling the feature
    // (Settings::autoApplyRomPatches() == false) makes this a no-op that returns `romPath`.
    QString resolvePatchedRom(const QString& romPath, QString* error = nullptr);

    // The directory patched ROMs are cached under (derived, disposable). Exposed for the probe.
    QString cacheDir();
}
