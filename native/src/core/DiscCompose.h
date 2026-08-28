// Composing a patched disc image, by driving the disc tool as a subprocess.
//
// Nothing here knows how a Wii disc is encrypted, hashed or laid out. The tool is built from the same
// Dolphin source the app boots discs with, so composition is done by the code that already reads them.
//
// A half-made disc must never be found by the library scan and shown as a playable game. That takes TWO
// things, not one: staging lives OUTSIDE the ROMs folder, AND the image is composed under a ".part" name
// inside it and renamed only on success. Staging alone was not enough -- the final output path is in the
// ROMs folder by definition, and the tool wrote into it for the minutes the convert takes.
#pragma once
#include <QString>

namespace DiscCompose
{
    struct Outcome
    {
        bool ok = false;
        QString error;        // non-empty iff !ok; phrased for the user
        QString outputPath;   // the composed image, when ok
    };

    // Bytes that must be free before starting: the extracted tree is about the size of the disc, and the
    // composed image is at most that again. Measured deliberately as a MULTIPLE of the source rather than a
    // fixed number, because the two disc generations this covers differ by an order of magnitude in size.
    qint64 requiredFreeBytes(qint64 discBytes);

    // True when `dir`'s volume has at least `needed` bytes free.
    bool hasFreeSpace(const QString& dir, qint64 needed);

    // Extract `discPath`, overlay the mod at `modRoot`, compose to `outputPath`. `toolPath` is the disc
    // tool. `stagingParent` must not be inside the ROMs folder. The base ROM is only ever read.
    //
    // `outputPath` appears only on success: the composition runs into `outputPath + ".part"` and is renamed
    // there once the tool exits 0. On any failure the ".part" file is removed and `outputPath` is left
    // exactly as it was, so a failed rebuild does not destroy an already-installed image.
    Outcome composePatchedDisc(const QString& toolPath, const QString& discPath, const QString& modRoot,
                               const QByteArray& riivolutionXml, const QString& outputPath,
                               const QString& stagingParent);
}
