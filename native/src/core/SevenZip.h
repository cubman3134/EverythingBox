// Minimal read-only .7z extraction, backed by the vendored public-domain LZMA SDK
// (native/third_party/lzma). Pure portable C underneath, so this works on every platform the app
// builds for (Windows/macOS/Linux/Android/iOS) with no system tools or extra dependencies — unlike
// shelling out to tar/7z. Used to open ROMs delivered as .7z (e.g. No-Intro sets from lolroms).
#pragma once
#include <QString>
#include <QStringList>
#include <QByteArray>

namespace SevenZip
{
    // Extract one file from a .7z archive into memory. Picks the first member whose name ends in one
    // of wantedExts (case-insensitive, each like ".sfc"); if none match (or wantedExts is empty), the
    // largest regular file. On success sets memberName (the inner file's base name, so its extension is
    // preserved) and data, and returns true. Decompresses the chosen file wholly into memory, so it
    // suits cartridge-sized ROMs (the usual .7z case); multi-GB disc images are better left as .chd/.iso.
    bool extractBest(const QString& sevenZipPath, const QStringList& wantedExts,
                     QString& memberName, QByteArray& data, QString* error = nullptr);
}
