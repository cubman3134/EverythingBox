// Minimal read-only .7z extraction, backed by the vendored public-domain LZMA SDK
// (native/third_party/lzma). Pure portable C underneath, so this works on every platform the app
// builds for (Windows/macOS/Linux/Android/iOS) with no system tools or extra dependencies — unlike
// shelling out to tar/7z. Used to open ROMs delivered as .7z (e.g. No-Intro sets from lolroms).
#pragma once
#include <QString>
#include <QStringList>

namespace SevenZip
{
    // Extract one file from a .7z archive directly to a file under destDir (named by the inner member, so
    // its extension is preserved) and return the written path; empty on failure. Picks the member whose
    // name ends in one of wantedExts (case-insensitive, each like ".sfc"); if none match (or wantedExts is
    // empty), the largest regular file (a No-Intro .7z holds one ROM, so "largest" picks it). An existing
    // file of the right size is reused. The decoder output is written straight to disk — peak memory is the
    // uncompressed size once, not twice — but the LZMA SDK still decodes a 7z file into a single buffer, so
    // a multi-GB image needs that much RAM to unpack (the Internet Archive's direct .chd/.iso is better for
    // those: no extraction at all).
    QString extractBestToFile(const QString& sevenZipPath, const QStringList& wantedExts,
                              const QString& destDir, QString* error = nullptr);

    // Extract every file in the .7z under destDir, preserving the archive's internal paths (for a whole
    // release, e.g. a PC-game repack: setup.exe + its .bin parts). Returns false on the first failure.
    bool extractAllToDir(const QString& sevenZipPath, const QString& destDir, QString* error = nullptr);
}
