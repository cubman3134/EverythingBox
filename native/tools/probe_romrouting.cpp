// Headless test for platform-aware ROM routing (issue #53). Two units under test, both pinned here:
//
//   * RomRouting.h — the pure junk filter + the folder-authoritative accept decision that RomLibrary::scan()
//     now uses INSTEAD of the old per-system extension allowlist. Under a recognised <root>/<system>/ folder
//     a file is that system's unless it is a non-ROM sidecar (save / patch / art / metadata / temp).
//
//   * SystemCatalog.h — PS2/PSP/Dreamcast/Xbox now DECLARE their real (colliding) disc extensions, and the
//     loose-file extension router (first-match-wins over the catalog) must be UNCHANGED by that: a hint-less
//     loose file still resolves to the earlier incumbent (saturn owns .iso, psx owns .chd/.cue/.pbp, psp
//     owns .cso). That "unchanged" property is the regression tripwire for the SystemCatalog change.
//
// Every expected value is hand-authored (an independent oracle) — never read back out of the function under
// test — so no fixture is a fixed point of the code it checks. The first-claimant oracle (firstForExt below)
// is a local re-implementation of forExtension's first-match rule, not a call to it, so a mutation to the
// real catalog order is what the assertion sees. Prints ROMROUTING-OK on success; ROMROUTING-FAIL on any miss.
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <cstdio>

#include "../src/core/RomRouting.h"
#include "../src/core/SystemCatalog.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) std::printf("PASS %s\n", name); \
    else { std::printf("FAIL %s\n", name); ++fails; } } while (0)

// Independent oracle: the FIRST system in a catalog list claiming an extension — exactly the rule
// SystemCatalog::forExtension applies, re-implemented here so the assertion tests the catalog's ORDER
// rather than trusting the function under test. Returns an empty id when nothing claims it.
static QString firstForExt(const QList<GameSystem>& cat, const QString& ext)
{
    for (const GameSystem& s : cat)
        if (s.extensions.contains(ext))
            return s.id;
    return QString();
}

static const GameSystem* sys(const QList<GameSystem>& cat, const QString& id)
{
    for (const GameSystem& s : cat) if (s.id == id) return &s;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using RomRouting::isLibraryJunkExtension;
    using RomRouting::acceptUnderSystemFolder;

    // ================= 1. junk filter REJECTS the non-ROM sidecars ========================================
    CHECK(isLibraryJunkExtension(QStringLiteral("srm")),   "junk: .srm save");
    CHECK(isLibraryJunkExtension(QStringLiteral("sav")),   "junk: .sav save");
    CHECK(isLibraryJunkExtension(QStringLiteral("state")), "junk: .state save");
    CHECK(isLibraryJunkExtension(QStringLiteral("state3")),"junk: numbered .state3 slot");
    CHECK(isLibraryJunkExtension(QStringLiteral("ss0")),   "junk: .ss0 save slot");
    CHECK(isLibraryJunkExtension(QStringLiteral("mcr")),   "junk: .mcr memory card");
    CHECK(isLibraryJunkExtension(QStringLiteral("ips")),   "junk: .ips patch");
    CHECK(isLibraryJunkExtension(QStringLiteral("bps")),   "junk: .bps patch");
    CHECK(isLibraryJunkExtension(QStringLiteral("png")),   "junk: .png box art");
    CHECK(isLibraryJunkExtension(QStringLiteral("jpg")),   "junk: .jpg box art");
    CHECK(isLibraryJunkExtension(QStringLiteral("xml")),   "junk: .xml gamelist");
    CHECK(isLibraryJunkExtension(QStringLiteral("nfo")),   "junk: .nfo metadata");
    CHECK(isLibraryJunkExtension(QStringLiteral("txt")),   "junk: .txt metadata");
    CHECK(isLibraryJunkExtension(QStringLiteral("bak")),   "junk: .bak backup");

    // ================= 2. junk filter does NOT junk real ROM/disc formats (conservative) =================
    CHECK(!isLibraryJunkExtension(QStringLiteral("bin")),  "keep: .bin is a real disc/rom image");
    CHECK(!isLibraryJunkExtension(QStringLiteral("md")),   "keep: .md is a Mega Drive ROM (not Markdown junk)");
    CHECK(!isLibraryJunkExtension(QStringLiteral("iso")),  "keep: .iso disc image");
    CHECK(!isLibraryJunkExtension(QStringLiteral("chd")),  "keep: .chd disc image");
    CHECK(!isLibraryJunkExtension(QStringLiteral("cso")),  "keep: .cso disc image");
    CHECK(!isLibraryJunkExtension(QStringLiteral("gdi")),  "keep: .gdi disc image");
    CHECK(!isLibraryJunkExtension(QStringLiteral("cue")),  "keep: .cue sheet");
    CHECK(!isLibraryJunkExtension(QStringLiteral("nes")),  "keep: .nes cartridge");
    CHECK(!isLibraryJunkExtension(QStringLiteral("gba")),  "keep: .gba cartridge");
    CHECK(!isLibraryJunkExtension(QStringLiteral("st")),   "keep: .st is an SNES ROM extension");
    CHECK(!isLibraryJunkExtension(QStringLiteral("sv")),   "keep: .sv is a Supervision ROM extension");
    CHECK(!isLibraryJunkExtension(QStringLiteral("m3u")),  "keep: .m3u disc playlist");
    CHECK(!isLibraryJunkExtension(QStringLiteral("zip")),  "keep: .zip archived ROM");
    CHECK(!isLibraryJunkExtension(QStringLiteral("7z")),   "keep: .7z archived ROM");
    CHECK(!isLibraryJunkExtension(QString()),              "keep: extensionless file is not junk");

    // ================= 3. folder-authoritative accept: under a system folder, accept iff not junk ========
    // These stand in for ".chd/.iso under the ps2/ folder is accepted as ps2" — the scan calls exactly this.
    CHECK(acceptUnderSystemFolder(QStringLiteral("iso")),  "folder accepts .iso (e.g. ps2/)");
    CHECK(acceptUnderSystemFolder(QStringLiteral("chd")),  "folder accepts .chd (e.g. ps2/)");
    CHECK(acceptUnderSystemFolder(QStringLiteral("cso")),  "folder accepts .cso (e.g. psp/)");
    CHECK(acceptUnderSystemFolder(QStringLiteral("gdi")),  "folder accepts .gdi (e.g. dreamcast/)");
    CHECK(!acceptUnderSystemFolder(QStringLiteral("srm")), "folder rejects .srm save");
    CHECK(!acceptUnderSystemFolder(QStringLiteral("jpg")), "folder rejects .jpg art");
    CHECK(!acceptUnderSystemFolder(QStringLiteral("xml")), "folder rejects .xml metadata");

    // ================= 4. the disc systems now CARRY their real extension sets ============================
    const QList<GameSystem> cat = SystemCatalog::builtinSystems();
    const GameSystem* ps2 = sys(cat, QStringLiteral("ps2"));
    const GameSystem* psp = sys(cat, QStringLiteral("psp"));
    const GameSystem* dc  = sys(cat, QStringLiteral("dreamcast"));
    const GameSystem* xbx = sys(cat, QStringLiteral("xbox"));
    CHECK(ps2 && ps2->extensions.contains(QStringLiteral("iso")), "ps2 declares .iso");
    CHECK(ps2 && ps2->extensions.contains(QStringLiteral("chd")), "ps2 declares .chd");
    CHECK(ps2 && ps2->extensions.contains(QStringLiteral("cso")), "ps2 declares .cso");
    CHECK(psp && psp->extensions.contains(QStringLiteral("iso")), "psp declares .iso");
    CHECK(psp && psp->extensions.contains(QStringLiteral("pbp")), "psp declares .pbp");
    CHECK(psp && psp->extensions.contains(QStringLiteral("chd")), "psp declares .chd");
    CHECK(dc  && dc->extensions.contains(QStringLiteral("chd")),  "dreamcast declares .chd");
    CHECK(dc  && dc->extensions.contains(QStringLiteral("cue")),  "dreamcast declares .cue");
    CHECK(dc  && dc->extensions.contains(QStringLiteral("iso")),  "dreamcast declares .iso");
    CHECK(xbx && xbx->extensions.contains(QStringLiteral("iso")), "xbox declares .iso");

    // ================= 5. loose-file routing (extension, no hint) is UNCHANGED ============================
    // The regression tripwire for item 3: despite the new declarations above, forExtension's first-match rule
    // must still return the earlier incumbent for the ambiguous formats. If a disc system displaced the
    // incumbent (e.g. by being reordered ahead, or the incumbent losing the extension), these go red.
    CHECK(firstForExt(cat, QStringLiteral("iso")) == QStringLiteral("saturn"),
          "loose .iso still routes to saturn (first-match-wins), not ps2/psp/xbox");
    CHECK(firstForExt(cat, QStringLiteral("chd")) == QStringLiteral("psx"),
          "loose .chd still routes to psx, not ps2/psp/dreamcast");
    CHECK(firstForExt(cat, QStringLiteral("cso")) == QStringLiteral("psp"),
          "loose .cso still routes to psp, not ps2");
    CHECK(firstForExt(cat, QStringLiteral("pbp")) == QStringLiteral("psx"),
          "loose .pbp still routes to psx, not psp");
    CHECK(firstForExt(cat, QStringLiteral("cue")) == QStringLiteral("psx"),
          "loose .cue still routes to psx, not dreamcast");
    CHECK(firstForExt(cat, QStringLiteral("gdi")) == QStringLiteral("dreamcast"),
          "loose .gdi still routes to dreamcast (its unique format)");

    if (fails == 0) std::printf("ROMROUTING-OK\n");
    else            std::printf("ROMROUTING had %d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
