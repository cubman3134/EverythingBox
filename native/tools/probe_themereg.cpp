// Headless check of ThemeRegistry (src/core/ThemeRegistry) — the pure core of the in-app theme gallery.
// Both Appearance surfaces (classic RegistryBrowser::Themes, themed presentThemeRegistry) parse the
// community registry index and validate remote paths through this one unit, so the contract is pinned here
// rather than twice in UI code that no probe can reach.
//
// The registry entry shape is `{name, author, description, dir: "themes2/<Name>"}`; the index array key is
// `themes2` (what github.com/cubman3134/everythingbox-themes actually serves) with `themes` accepted as the
// legacy spelling. Every path in a listing arrives over the network and is about to become a filename, so
// isSafeRelPath REJECTS rather than sanitises — a rewritten path is a guess about intent.
//
// Prints THEMEREG-OK on success; any failure prints THEMEREG-FAIL <cond> and exits non-zero.
#include "ThemeRegistry.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEMEREG-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. The live index shape: a "themes2" array of dir-entries.
    {
        const QByteArray json = R"({"themes2":[
            {"name":"Grid","author":"EverythingBox","description":"A dense poster grid.","dir":"themes2/Grid"},
            {"name":"Night","author":"EverythingBox","description":"A dark desktop library.",
             "dir":"themes2/Night","formFactors":["desktop"]}]})";
        const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(json);
        CHECK(es.size() == 2);
        CHECK(es.value(0).name == QStringLiteral("Grid"));
        CHECK(es.value(0).author == QStringLiteral("EverythingBox"));
        CHECK(es.value(0).dir == QStringLiteral("themes2/Grid"));
        CHECK(es.value(0).folder() == QStringLiteral("Grid"));
        CHECK(es.value(0).formFactors.isEmpty());
        CHECK(es.value(1).folder() == QStringLiteral("Night"));
        CHECK(es.value(1).formFactors == QStringList{ QStringLiteral("desktop") });
    }

    // 2. The legacy "themes" key still parses — one line of compatibility, and the key the pre-existing
    //    RegistryBrowser code assumed. "themes2" WINS when both are present.
    {
        const QByteArray legacy = R"({"themes":[{"name":"Old","dir":"themes2/Old"}]})";
        CHECK(ThemeRegistry::parseIndex(legacy).size() == 1);
        CHECK(ThemeRegistry::parseIndex(legacy).value(0).folder() == QStringLiteral("Old"));

        const QByteArray both = R"({"themes2":[{"name":"New","dir":"themes2/New"}],
                                     "themes":[{"name":"Old","dir":"themes2/Old"}]})";
        const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(both);
        CHECK(es.size() == 1);
        CHECK(es.value(0).folder() == QStringLiteral("New"));

        // "themes2" wins on PRESENCE, not on being non-empty. A registry that deliberately empties themes2
        // — a takedown, a migration, a bad deploy — means "nothing to offer", and must NOT silently
        // re-serve the legacy list it was replaced by.
        CHECK(ThemeRegistry::parseIndex(
                  QByteArray(R"({"themes2":[],"themes":[{"name":"Old","dir":"themes2/Old"}]})")).isEmpty());
        // Likewise a malformed themes2 is an error, not a downgrade to the legacy key.
        CHECK(ThemeRegistry::parseIndex(
                  QByteArray(R"({"themes2":"nope","themes":[{"name":"Old","dir":"themes2/Old"}]})")).isEmpty());
    }

    // 3. Junk in, nothing out — a malformed index must never yield a half-entry that later becomes a path.
    {
        CHECK(ThemeRegistry::parseIndex(QByteArray("not json at all")).isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray("{}")).isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":"nope"})")).isEmpty());
        // An entry with no dir has nothing to install and is dropped, not kept with an empty folder.
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":[{"name":"NoDir"}]})")).isEmpty());
        // The legacy flat colour-theme shape ("file"/"assets", no "dir") is NOT a themes2 entry.
        CHECK(ThemeRegistry::parseIndex(
                  QByteArray(R"({"themes2":[{"name":"Flat","file":"flat.json","assets":["a.png"]}]})")).isEmpty());
        // One bad element does not poison the array: a non-object is skipped, the valid sibling survives.
        {
            const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":["junk",{"name":"Grid","dir":"themes2/Grid"},42]})"));
            CHECK(es.size() == 1);
            CHECK(es.value(0).folder() == QStringLiteral("Grid"));
        }
    }

    // 4. A traversing or absolute dir is dropped at parse time — folder() is the ONLY thing that ever becomes
    //    a directory name, so it must be a single plain segment or nothing.
    {
        const char* bad[] = { R"({"themes2":[{"name":"X","dir":"themes2/../../etc"}]})",
                              R"({"themes2":[{"name":"X","dir":"/abs/Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":"C:/Windows/Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":"themes2\\Grid"}]})",
                              R"({"themes2":[{"name":"X","dir":".."}]})",
                              R"({"themes2":[{"name":"X","dir":"themes2/"}]})",
                              // Win32 strips trailing dots and spaces from a component before resolving it,
                              // so this dir would be created as "Grid" — a folder whose name on disk is not
                              // the name we recorded, which the uninstall path could then fail to remove.
                              R"({"themes2":[{"name":"X","dir":"themes2/Grid "}]})",
                              R"({"themes2":[{"name":"X","dir":"themes2/Grid."}]})" };
        for (const char* b : bad) CHECK(ThemeRegistry::parseIndex(QByteArray(b)).isEmpty());

        // A dir may be deeper than two segments — it is relative to the index URL's directory, and only its
        // LAST segment ever becomes a local folder name. Intentional, so pin it.
        {
            const QVector<ThemeRegistry::Entry> es = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":[{"name":"X","dir":"a/b/Grid"}]})"));
            CHECK(es.size() == 1);
            CHECK(es.value(0).folder() == QStringLiteral("Grid"));
        }
    }

    // 5. isSafeRelPath — the gate every listed file passes before it becomes a filename.
    {
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("theme.json")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/move.wav")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("fonts/VarelaRound-Regular.ttf")));
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral("a/b/c/d.png")));

        CHECK(!ThemeRegistry::isSafeRelPath(QString()));                                  // empty
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("../theme.json")));            // traversal
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/../../theme.json")));  // buried traversal
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("/etc/passwd")));              // absolute
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("C:/Windows/x.dll")));         // drive letter
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds\\move.wav")));         // backslash
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds//move.wav")));         // empty segment
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/")));                  // trailing slash
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral(".")));                        // dot segment
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a/./b.png")));                // buried dot
        // Windows reserved device names, at any depth and with any extension. On Windows these do not name
        // files at all; writing one opens a DEVICE, and the failure is baffling rather than loud.
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("CON")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("con.wav")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/NUL.wav")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("COM1")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("lpt9.txt")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("CONIN$")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/clock$.wav")));

        // Win32 strips trailing dots and spaces from the final component BEFORE resolving it. So a padded
        // device name is still the device — "sounds/con " opens CON — and two listed paths that differ only
        // in that padding land on the same file, one silently overwriting the other. Reject rather than
        // trim: the padding is never meaningful, and trimming would return a path the listing did not name.
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("con ")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/con ")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("con .wav")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("nul.")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("theme.json ")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("theme.json.")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds /move.wav")));   // padding at any depth
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("theme.json  ")));
        // A leading space is a legal (if odd) filename on Win32 and is NOT stripped, so it stays accepted —
        // the rule is about what the OS silently rewrites, not about tidy names.
        CHECK(ThemeRegistry::isSafeRelPath(QStringLiteral(" theme.json")));

        // The rest of the Win32-illegal set. No traversal is possible through these — backslash and ".."
        // are already gone — but an embedded NUL truncates the name in the Win32 file APIs, and the others
        // turn a write into a confusing failure or a collision instead of an install.
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a<b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a>b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a\"b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a|b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a?b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a*b.png")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("sounds/mo*ve.wav")));
        // U+0000 arrives through a JSON unicode escape and truncates the name at the Win32 boundary.
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("theme") + QChar(u'\0') + QStringLiteral(".json")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("theme") + QChar(u'\n') + QStringLiteral(".json")));
        CHECK(!ThemeRegistry::isSafeRelPath(QStringLiteral("a") + QChar(0x1F) + QStringLiteral("b.png")));
    }

    if (failures == 0) std::printf("THEMEREG-OK\n");
    return failures == 0 ? 0 : 1;
}
