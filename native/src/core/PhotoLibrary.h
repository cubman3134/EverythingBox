// A local PHOTO library — the one common media type EverythingBox could not open (issue #102). The root is
// Settings::photosFolder() (default <data>/photos). We walk it, keep every file whose extension is an image
// format Qt's built-in plugins decode, and surface them grouped by their containing folder. The pure
// functions (isPhotoFile/scanFolder/imagesInFolder/groupByFolder) take an explicit root and are probe-tested;
// root() reads Settings and is main-thread only — mirroring LocalLibrary exactly.
//
// This is the SCAN/decision layer only. The on-screen viewer is ComicView in "photo mode" (openFolder),
// which reuses its existing render/page/zoom widget over a plain folder of images instead of a CBZ's ZIP
// entries — see ComicView::openFolder. Format support is whatever Qt's image plugins provide: JPEG/PNG/
// WebP/GIF/BMP everywhere; HEIC/AVIF only where the platform Qt ships that plugin (state honestly, never
// promise it). EXIF orientation is applied at decode time by the viewer (QImageReader::setAutoTransform).
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

namespace PhotoLibrary
{
    struct PhotoEntry
    {
        QString path;     // absolute file path
        QString folder;   // absolute path of the directory containing the file (the group key)
        qint64  mtime = 0; // file last-modified, seconds since epoch (0 if unavailable)
    };

    // Extension predicate — the ONE place the set of viewable image formats is decided. Case-insensitive.
    // jpg/jpeg/png/webp/gif/bmp in (Qt decodes these out of the box on every platform); everything else
    // (txt, mp4, …) out. avif is accepted too (Qt ships the plugin on desktop); heic is deliberately NOT
    // listed — it is present only where the platform Qt provides the plugin, so promising it in the scan
    // filter would surface files that then fail to decode.
    bool isPhotoFile(const QString& path);

    // Recursive scan of a library root -> every image file under it, in natural (numeric-aware) path order
    // (folderA/img1, img2, img10 — not img1, img10, img2). Empty/missing root => empty (feature-dormant).
    QVector<PhotoEntry> scanFolder(const QString& root);

    // The images DIRECTLY in one folder (non-recursive), as absolute paths in natural filename order. This is
    // what the viewer pages through: open a JPEG -> its siblings in the same folder, in order. Empty/missing
    // folder => empty.
    QStringList imagesInFolder(const QString& folder);

    // Group scanned entries by their containing folder. Within each group the entries keep the natural
    // filename order scanFolder already imposed. A QMap keeps the folders themselves in sorted order.
    QMap<QString, QVector<PhotoEntry>> groupByFolder(const QVector<PhotoEntry>& entries);

    // Cached convenience (main-thread only): the configured photo-library root. Reads Settings::photosFolder().
    QString root();
}
