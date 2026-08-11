// Headless probe for the local photo library core (issue #102): the pure scan/decision layer PhotoLibrary —
// the extension predicate, the recursive scan (image files only), natural (numeric-aware) ordering,
// non-recursive per-folder listing, and folder grouping. Builds a hermetic QTemporaryDir fixture and asserts
// the matrix against HAND-COMPUTED expectations (never against the function under test).
//
// The on-screen viewer (ComicView photo mode) and the EXIF auto-transform it applies are NOT drivable
// headlessly — this probe pins the scan/oracle only. Prints PHOTOS-OK on success; any failure prints
// PHOTOS-FAIL <cond> (line) and exits non-zero.
#include "PhotoLibrary.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PHOTOS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static void writeFile(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); f.open(QIODevice::WriteOnly); f.write("x"); f.close();
}

static QStringList baseNames(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << QFileInfo(p).fileName();
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. isPhotoFile: the viewable-format predicate (independent oracle = the format list itself) --------
    // In: jpg/jpeg/png/webp/gif/bmp/avif, case-insensitive. Out: everything else, and a file with no extension.
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.jpg")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.jpeg")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.png")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.webp")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.gif")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.bmp")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("a.avif")));
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("PHOTO.JPG")));   // case-insensitive
    CHECK(PhotoLibrary::isPhotoFile(QStringLiteral("b.Jpeg")));      // mixed case
    CHECK(!PhotoLibrary::isPhotoFile(QStringLiteral("a.txt")));
    CHECK(!PhotoLibrary::isPhotoFile(QStringLiteral("a.mp4")));
    CHECK(!PhotoLibrary::isPhotoFile(QStringLiteral("a.mov")));
    CHECK(!PhotoLibrary::isPhotoFile(QStringLiteral("a.heic")));     // deliberately NOT promised (platform-only)
    CHECK(!PhotoLibrary::isPhotoFile(QStringLiteral("noextension")));

    // ---- Fixture tree --------------------------------------------------------------------------------------
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();
    const QString album  = root + QStringLiteral("/album");
    const QString trip   = root + QStringLiteral("/trip");
    const QString nested = trip + QStringLiteral("/nested");

    // album/: 8 images (with numeric names to prove natural sort + two non-images to prove the filter)
    writeFile(album + QStringLiteral("/anim.gif"));
    writeFile(album + QStringLiteral("/img1.jpg"));
    writeFile(album + QStringLiteral("/img2.jpg"));
    writeFile(album + QStringLiteral("/img10.jpg"));
    writeFile(album + QStringLiteral("/photo.JPEG"));   // uppercase extension
    writeFile(album + QStringLiteral("/pic.PNG"));      // uppercase extension
    writeFile(album + QStringLiteral("/shot.webp"));
    writeFile(album + QStringLiteral("/x.bmp"));
    writeFile(album + QStringLiteral("/notes.txt"));    // excluded
    writeFile(album + QStringLiteral("/clip.mp4"));     // excluded
    // trip/: 2 images directly + 1 image one level deeper (proves recursive scan vs non-recursive listing)
    writeFile(trip + QStringLiteral("/one.png"));
    writeFile(trip + QStringLiteral("/two.png"));
    writeFile(nested + QStringLiteral("/deep.jpg"));

    // Hand-computed natural order of album's filenames (numeric-aware, case-insensitive):
    // anim < img1 < img2 < img10 (1<2<10, not lexicographic) < photo < pic < shot < x.
    const QStringList albumOrder = {
        QStringLiteral("anim.gif"), QStringLiteral("img1.jpg"), QStringLiteral("img2.jpg"),
        QStringLiteral("img10.jpg"), QStringLiteral("photo.JPEG"), QStringLiteral("pic.PNG"),
        QStringLiteral("shot.webp"), QStringLiteral("x.bmp")
    };

    // ---- 2. scanFolder: recursive, image-only ---------------------------------------------------------------
    const QVector<PhotoLibrary::PhotoEntry> scanned = PhotoLibrary::scanFolder(root);
    CHECK(scanned.size() == 11);   // 8 (album) + 2 (trip) + 1 (nested); notes.txt + clip.mp4 excluded
    bool sawNonImage = false, sawDeep = false;
    for (const auto& e : scanned)
    {
        if (!PhotoLibrary::isPhotoFile(e.path)) sawNonImage = true;   // the filter must have dropped every non-image
        if (QFileInfo(e.path).fileName() == QStringLiteral("deep.jpg")) sawDeep = true;
        CHECK(!QFileInfo(e.path).fileName().endsWith(QStringLiteral(".txt")));
        CHECK(!QFileInfo(e.path).fileName().endsWith(QStringLiteral(".mp4")));
        CHECK(!e.folder.isEmpty());   // every entry carries its containing folder (the group key)
    }
    CHECK(!sawNonImage);
    CHECK(sawDeep);                    // recursive: a file one level down is included

    // ---- 3. imagesInFolder: non-recursive, natural order ----------------------------------------------------
    const QStringList albumImages = PhotoLibrary::imagesInFolder(album);
    CHECK(albumImages.size() == 8);
    CHECK(baseNames(albumImages) == albumOrder);   // exact natural order (kills a non-numeric / broken comparator)

    const QStringList tripImages = PhotoLibrary::imagesInFolder(trip);
    CHECK(tripImages.size() == 2);                                     // NON-recursive: deep.jpg (in nested/) is excluded
    CHECK(!baseNames(tripImages).contains(QStringLiteral("deep.jpg")));
    CHECK(baseNames(tripImages) == (QStringList{ QStringLiteral("one.png"), QStringLiteral("two.png") }));

    // ---- 4. groupByFolder: one bucket per containing folder -------------------------------------------------
    const QMap<QString, QVector<PhotoLibrary::PhotoEntry>> groups = PhotoLibrary::groupByFolder(scanned);
    CHECK(groups.size() == 3);   // album, trip, trip/nested
    CHECK(groups.contains(QFileInfo(album).absoluteFilePath()));
    CHECK(groups.contains(QFileInfo(trip).absoluteFilePath()));
    CHECK(groups.contains(QFileInfo(nested).absoluteFilePath()));
    CHECK(groups.value(QFileInfo(album).absoluteFilePath()).size() == 8);
    CHECK(groups.value(QFileInfo(trip).absoluteFilePath()).size() == 2);
    CHECK(groups.value(QFileInfo(nested).absoluteFilePath()).size() == 1);
    // The album group keeps the natural order scanFolder imposed.
    {
        QStringList names;
        for (const auto& e : groups.value(QFileInfo(album).absoluteFilePath())) names << QFileInfo(e.path).fileName();
        CHECK(names == albumOrder);
    }

    // ---- 4b. hasImages: the cheap "offer the Photos category?" gate (#102) ----------------------------------
    // A folder that holds ONLY non-image files (independent oracle: it has no viewable file by construction).
    const QString textOnly = root + QStringLiteral("/textonly");
    writeFile(textOnly + QStringLiteral("/readme.txt"));
    writeFile(textOnly + QStringLiteral("/movie.mp4"));
    CHECK(PhotoLibrary::hasImages(root));                 // the tree has images (album/trip) -> the gate is open
    CHECK(PhotoLibrary::hasImages(album));                // a folder of images -> true
    CHECK(!PhotoLibrary::hasImages(textOnly));            // a folder of only non-images -> false (gate stays shut)
    CHECK(!PhotoLibrary::hasImages(QString()));           // empty root -> false
    CHECK(!PhotoLibrary::hasImages(root + QStringLiteral("/does-not-exist"))); // missing root -> false

    // ---- 5. Empty / missing root => empty (feature-dormant contract) ----------------------------------------
    CHECK(PhotoLibrary::scanFolder(QString()).isEmpty());
    CHECK(PhotoLibrary::scanFolder(root + QStringLiteral("/does-not-exist")).isEmpty());
    CHECK(PhotoLibrary::imagesInFolder(QString()).isEmpty());
    CHECK(PhotoLibrary::imagesInFolder(root + QStringLiteral("/does-not-exist")).isEmpty());
    CHECK(PhotoLibrary::groupByFolder({}).isEmpty());

    if (failures == 0) { std::puts("PHOTOS-OK"); return 0; }
    std::fprintf(stderr, "PHOTOS: %d check(s) failed\n", failures);
    return 1;
}
