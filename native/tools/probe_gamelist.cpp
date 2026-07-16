// Verifies GamelistStore reads real EmulationStation / RetroBat gamelist.xml: parse a system's list, match a
// ROM by filename, resolve ES media (marquee->logo, thumbnail/image->box, video, fanart) to local files, and
// map the metadata. Points at a RetroBat roms dir (argv[1], default C:\RetroBat\roms). When no data is
// present it passes trivially (so CI, which has no RetroBat install, is fine). Prints GAMELIST-OK.
#include "GamelistStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <cstdio>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "GAMELIST-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

// First <path> in a gamelist that has at least one media tag present (so lookup has art to resolve).
static QString firstScrapedRom(const QString& gamelistPath)
{
    QFile f(gamelistPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QXmlStreamReader xml(&f);
    while (!xml.atEnd())
    {
        xml.readNext();
        if (!(xml.isStartElement() && xml.name() == QLatin1String("game"))) continue;
        QString path; bool hasMedia = false;
        while (!xml.atEnd() && !(xml.isEndElement() && xml.name() == QLatin1String("game")))
        {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            const QString tag = xml.name().toString();
            const QString val = xml.readElementText(QXmlStreamReader::SkipChildElements);
            if (tag == QStringLiteral("path")) path = val;
            else if (tag == QStringLiteral("image") || tag == QStringLiteral("thumbnail")
                  || tag == QStringLiteral("marquee")) hasMedia = true;
        }
        if (!path.isEmpty() && hasMedia) return QFileInfo(path).fileName();
    }
    return {};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString romsRoot = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("C:/RetroBat/roms");

    // Find any system that has a gamelist + a scraped game we can resolve.
    QString sysDir, romFile;
    for (const QString& sys : { QStringLiteral("atari2600"), QStringLiteral("nes"), QStringLiteral("snes"),
                                QStringLiteral("dreamcast"), QStringLiteral("megadrive"), QStringLiteral("gb") })
    {
        const QString d = romsRoot + QLatin1Char('/') + sys;
        const QString gl = d + QStringLiteral("/gamelist.xml");
        if (!QFile::exists(gl)) continue;
        const QString rom = firstScrapedRom(gl);
        if (!rom.isEmpty()) { sysDir = d; romFile = rom; break; }
    }
    if (sysDir.isEmpty())
    {
        std::printf("GAMELIST-OK (skipped: no EmulationStation/RetroBat data at %s)\n", romsRoot.toUtf8().constData());
        return 0;
    }

    const QString romPath = sysDir + QLatin1Char('/') + romFile;
    std::printf("testing: %s\n", romPath.toUtf8().constData());

    CHECK(GamelistStore::has(romPath), "has() finds the ROM in the gamelist");
    const MediaDetail d = GamelistStore::lookup(romPath);
    CHECK(d.valid, "lookup returns a valid card");
    CHECK(!d.title.isEmpty(), "title parsed");

    // At least one artwork role resolved to a real local file (offline-ready).
    int localArt = 0;
    for (auto it = d.art.images.constBegin(); it != d.art.images.constEnd(); ++it)
        for (const QString& p : it.value())
            if (!p.startsWith(QStringLiteral("http")) && QFile::exists(p)) ++localArt;
    CHECK(localArt > 0, "at least one media role resolved to an existing local file");
    CHECK(!d.imageUrl.isEmpty() && QFile::exists(d.imageUrl), "primary cover (box) is a local file");
    std::printf("  title=\"%s\" facts=%d roles=%d logo=%s video=%d\n",
                d.title.toUtf8().constData(), int(d.facts.size()), int(d.art.images.size()),
                d.art.image(QStringLiteral("logo")).isEmpty() ? "-" : "yes", int(d.art.videos.size()));

    // A miss (a ROM name not in the list) must be invalid, not a crash.
    CHECK(!GamelistStore::lookup(sysDir + QStringLiteral("/__nonexistent_game__.zip")).valid, "a miss is invalid");

    // --- write-back round-trip (metadata + facts; no network) -----------------------------------------
    {
        const QString tmp = QDir::tempPath() + QStringLiteral("/mmv-gltest");
        QDir(tmp).removeRecursively(); QDir().mkpath(tmp);
        MediaDetail w; w.title = QStringLiteral("My Test Game"); w.overview = QStringLiteral("A description.");
        w.subtitle = QStringLiteral("1998"); w.valid = true;
        w.facts << MediaFact{ QStringLiteral("Developer"), QStringLiteral("Acme") }
                << MediaFact{ QStringLiteral("Genre"), QStringLiteral("Puzzle") };
        GamelistStore::write(tmp + QStringLiteral("/My Test Game.zip"), w); // no http media -> writes synchronously
        GamelistStore::clearCache();
        const MediaDetail rb = GamelistStore::lookup(tmp + QStringLiteral("/My Test Game.zip"));
        CHECK(rb.valid && rb.title == QStringLiteral("My Test Game"), "write-back: entry reads back");
        CHECK(rb.overview == QStringLiteral("A description."), "write-back: desc round-trips");
        CHECK(rb.facts.size() >= 2, "write-back: facts round-trip");

        MediaDetail w2; w2.title = QStringLiteral("Second Game"); w2.valid = true;
        GamelistStore::write(tmp + QStringLiteral("/Second Game.zip"), w2);
        GamelistStore::clearCache();
        CHECK(GamelistStore::lookup(tmp + QStringLiteral("/My Test Game.zip")).valid, "write-back: first entry preserved");
        CHECK(GamelistStore::lookup(tmp + QStringLiteral("/Second Game.zip")).valid, "write-back: second entry appended");
        QDir(tmp).removeRecursively();
    }

    if (failures) { std::fprintf(stderr, "GAMELIST-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("GAMELIST-OK\n");
    return 0;
}
