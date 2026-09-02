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
#include "DecorationPack.h"   // #187: the `decorations` section of the SAME index document — block 12

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEMEREG-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

#ifdef Q_OS_WIN
// Both defines go BEFORE windows.h, not after: NOMINMAX in particular suppresses the min/max function-like
// macros, which otherwise sit in front of every standard header included later and turn an unrelated future
// include (<algorithm>, <limits>) into a build break on the Windows leg alone. WIN32_LEAN_AND_MEAN drops the
// winsock/OLE/RPC bulk this probe has no use for.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <vector>

// Block 11 needs a directory that still lets its children be MOVED AWAY but refuses to accept a new one, so
// that a swap can half-complete: exactly what happens when another process takes the destination name back
// between the two renames. NTFS has a right for precisely that half — FILE_ADD_SUBDIRECTORY — so one
// non-inherited deny ACE on the themes root arranges it without touching anything else. Non-inherited
// matters: the staging directory below it must stay writable or the install never reaches the renames.
//
// The original DACL is handed back to the caller and MUST be restored, or the temp tree cannot be cleaned up
// the way every other block cleans up after itself.
static bool denyAddSubdirectory(const QString& dir, PSECURITY_DESCRIPTOR* sdOut, PACL* originalOut)
{
    *sdOut = nullptr;
    *originalOut = nullptr;
    std::vector<wchar_t> path(dir.size() + 1);
    path[dir.toWCharArray(path.data())] = L'\0';

    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &oldDacl, nullptr, &sd) != ERROR_SUCCESS)
        return false;

    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
    PSID everyone = nullptr;
    if (!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone))
    { LocalFree(sd); return false; }

    EXPLICIT_ACCESS_W ea = {};
    ea.grfAccessPermissions = FILE_ADD_SUBDIRECTORY;
    ea.grfAccessMode        = DENY_ACCESS;
    ea.grfInheritance       = NO_INHERITANCE;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = static_cast<LPWSTR>(everyone);

    PACL newDacl = nullptr;
    const DWORD built = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    FreeSid(everyone);
    if (built != ERROR_SUCCESS) { LocalFree(sd); return false; }

    const DWORD set = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, newDacl, nullptr);
    LocalFree(newDacl);
    if (set != ERROR_SUCCESS) { LocalFree(sd); return false; }

    *sdOut = sd;                // oldDacl points INTO sd, so the descriptor outlives the guard
    *originalOut = oldDacl;
    return true;
}

static void restoreDacl(const QString& dir, PSECURITY_DESCRIPTOR sd, PACL original)
{
    if (!sd) return;
    std::vector<wchar_t> path(dir.size() + 1);
    path[dir.toWCharArray(path.data())] = L'\0';
    SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, original, nullptr);
    LocalFree(sd);
}

// Can this machine host block 11's provocation at all? Two ways it cannot, and NEITHER is a statement about
// installFiles: a non-NTFS %TEMP% (a FAT/exFAT scratch volume, some container mounts) makes
// SetNamedSecurityInfoW fail outright, and a context holding SeRestorePrivilege bypasses DACLs entirely so
// the ACE applies but does not bite. Asserting straight through either one turns a CI host's configuration
// into a red probe that reads as a regression in the code under test, which is worse than a named gap.
//
// The bite is checked directly rather than inferred from the install's outcome: creating a subdirectory of
// `dir` is the one right being denied, so if that still succeeds the deny is decorative. On false nothing is
// left applied and nothing is left on disk.
static bool canDenyAddSubdirectory(const QString& dir, PSECURITY_DESCRIPTOR* sdOut, PACL* originalOut)
{
    if (!denyAddSubdirectory(dir, sdOut, originalOut)) return false;

    const QString canary = dir + QStringLiteral("/.eb-acl-canary");
    if (QDir().mkpath(canary))
    {
        QDir().rmdir(canary);
        restoreDacl(dir, *sdOut, *originalOut);
        *sdOut = nullptr;
        *originalOut = nullptr;
        return false;
    }
    return true;
}
#endif

// THE REGISTRY'S OWN BYTES, captured verbatim from
// raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json on 2026-08-01.
//
// Why a capture rather than a fixture written to suit the reader: a fixture built from what parseIndex
// expects is a fixed point of the function under test — it passes whatever key names the reader happens to
// use, so it would have gone on passing through the entire life of the "themes"/"themes2" mismatch that
// made the classic gallery list nothing at all (theme-gallery design, §"It could not list a theme"). This
// document is the producer's output, so an assertion against it fails the moment the two sides disagree,
// in whichever direction they disagree.
//
// It is also the base of the shape-mismatch fixture in block 6, which is this same document with ONE
// byte-range changed — the container's key. Deriving the negative from the positive is what makes it a
// statement about a real registry drifting rather than about a string the probe made up.
//
// Kept whole, descriptions included, because trimming it would make it a paraphrase and the whole point is
// that it is not one. It goes stale the day the registry adds a theme; that is intended — a count that has
// to be updated is a count somebody looked at.
static const char* const kLiveIndex = R"JSON({
  "themes2": [
    {
      "name": "Default",
      "author": "EverythingBox",
      "description": "The default themed home: a centred system carousel with a poster, title, rating and a help bar.",
      "dir": "themes2/Default"
    },
    {
      "name": "Channels",
      "author": "cubman3134",
      "description": "A Nintendo Wii-menu-style launcher: your catalogs as rounded colour channel tiles with name plates on a soft light-blue gradient; the selected channel gets a blue ring and pops. Arrow-key / mouse navigable.",
      "dir": "themes2/Channels"
    },
    {
      "name": "Grid",
      "author": "EverythingBox",
      "description": "A dense poster grid of your catalogs with a live preview + detail panel.",
      "dir": "themes2/Grid"
    },
    {
      "name": "Lumen",
      "author": "EverythingBox",
      "description": "A light theme: a five-wide poster grid on a near-white background.",
      "dir": "themes2/Lumen"
    },
    {
      "name": "Midnight",
      "author": "EverythingBox",
      "description": "A dark theme with a centred carousel, a full detail view, and a subtle starfield.",
      "dir": "themes2/Midnight"
    },
    {
      "name": "Night",
      "author": "EverythingBox",
      "description": "A dark desktop library: a persistent category rail down the left, a dense cover grid as the main surface, and a details pane that follows the selection with artwork and metadata. Low-chrome and high-contrast — the content is the bright thing. Built for a monitor and a mouse, fully keyboard navigable.",
      "dir": "themes2/Night",
      "formFactors": [
        "desktop"
      ]
    },
    {
      "name": "Triple",
      "author": "cubman3134",
      "description": "A PlayStation-style XrossMediaBar: four media categories (Video / Games / Audio / Reading) crossed with the selected category's item column, over an animated wave, with navigation sounds. Drawn icons, fully arrow-key / controller / mouse navigable.",
      "dir": "themes2/Triple"
    }
  ]
})JSON";

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. THE DOCUMENT THE REGISTRY ACTUALLY SERVES. Not a fixture shaped to the reader — see the note on
    //    kLiveIndex. If the index key, the entry key names or the dir convention ever move on either side,
    //    this is what goes red, and it goes red whichever side moved.
    {
        const ThemeRegistry::Index ix = ThemeRegistry::parseIndex(QByteArray(kLiveIndex));
        CHECK(ix.ok());
        CHECK(ix.shapeError.isEmpty());
        CHECK(ix.entries.size() == 7);
        QStringList folders;
        for (const ThemeRegistry::Entry& e : ix.entries) folders << e.folder();
        CHECK(folders == (QStringList{ QStringLiteral("Default"), QStringLiteral("Channels"),
                                       QStringLiteral("Grid"), QStringLiteral("Lumen"),
                                       QStringLiteral("Midnight"), QStringLiteral("Night"),
                                       QStringLiteral("Triple") }));
        CHECK(ix.entries.value(0).name == QStringLiteral("Default"));
        CHECK(ix.entries.value(0).author == QStringLiteral("EverythingBox"));
        CHECK(ix.entries.value(0).dir == QStringLiteral("themes2/Default"));
        CHECK(!ix.entries.value(0).description.isEmpty());
        CHECK(ix.entries.value(0).formFactors.isEmpty());
        // Night is the one entry that declares a form factor, so it is the one that proves the key is read
        // at all rather than that the field defaults to empty.
        CHECK(ix.entries.value(5).folder() == QStringLiteral("Night"));
        CHECK(ix.entries.value(5).formFactors == QStringList{ QStringLiteral("desktop") });
    }

    // 2. The legacy "themes" key still parses — one line of compatibility, and the key the pre-existing
    //    RegistryBrowser code assumed. "themes2" WINS when both are present.
    {
        const QByteArray legacy = R"({"themes":[{"name":"Old","dir":"themes2/Old"}]})";
        CHECK(ThemeRegistry::parseIndex(legacy).entries.size() == 1);
        CHECK(ThemeRegistry::parseIndex(legacy).entries.value(0).folder() == QStringLiteral("Old"));
        // The legacy key is a container this reader RECOGNISES, so reading one is not a shape complaint.
        CHECK(ThemeRegistry::parseIndex(legacy).ok());

        const QByteArray both = R"({"themes2":[{"name":"New","dir":"themes2/New"}],
                                     "themes":[{"name":"Old","dir":"themes2/Old"}]})";
        const ThemeRegistry::Index ix = ThemeRegistry::parseIndex(both);
        CHECK(ix.entries.size() == 1);
        CHECK(ix.entries.value(0).folder() == QStringLiteral("New"));

        // "themes2" wins on PRESENCE, not on being non-empty. A registry that deliberately empties themes2
        // — a takedown, a migration, a bad deploy — means "nothing to offer", and must NOT silently
        // re-serve the legacy list it was replaced by.
        {
            const ThemeRegistry::Index emptied = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":[],"themes":[{"name":"Old","dir":"themes2/Old"}]})"));
            CHECK(emptied.entries.isEmpty());
            // …and it is NOT a shape complaint. This is the other half of #174: an emptied themes2 is a
            // registry saying "nothing to offer", and if that were reported as "I could not read this" the
            // distinction would simply have been inverted rather than drawn.
            CHECK(emptied.ok());
        }
        // Likewise a malformed themes2 is an error, not a downgrade to the legacy key — and unlike the
        // emptied one it IS a shape complaint, because "themes2" holding a string is not a registry
        // saying anything at all.
        {
            const ThemeRegistry::Index bad = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":"nope","themes":[{"name":"Old","dir":"themes2/Old"}]})"));
            CHECK(bad.entries.isEmpty());
            CHECK(!bad.ok());
            CHECK(bad.shapeError.contains(QStringLiteral("themes2")));
        }
    }

    // 3. Junk in, nothing out — a malformed index must never yield a half-entry that later becomes a path.
    {
        CHECK(ThemeRegistry::parseIndex(QByteArray("not json at all")).entries.isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray("{}")).entries.isEmpty());
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":"nope"})")).entries.isEmpty());
        // An entry with no dir has nothing to install and is dropped, not kept with an empty folder.
        CHECK(ThemeRegistry::parseIndex(QByteArray(R"({"themes2":[{"name":"NoDir"}]})")).entries.isEmpty());
        // The legacy flat colour-theme shape ("file"/"assets", no "dir") is NOT a themes2 entry.
        CHECK(ThemeRegistry::parseIndex(
                  QByteArray(R"({"themes2":[{"name":"Flat","file":"flat.json","assets":["a.png"]}]})"))
                  .entries.isEmpty());
        // One bad element does not poison the array: a non-object is skipped, the valid sibling survives.
        {
            const ThemeRegistry::Index ix = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":["junk",{"name":"Grid","dir":"themes2/Grid"},42]})"));
            CHECK(ix.entries.size() == 1);
            CHECK(ix.entries.value(0).folder() == QStringLiteral("Grid"));
            // A PARTIAL drop is deliberately silent. Two of three elements were thrown away here and the
            // list is still not empty, so nothing is being hidden — and one malformed row in a registry that
            // takes pull requests is not a reason to tell every user the registry is broken.
            CHECK(ix.ok());
        }
    }

    // 3b. AN EMPTY REGISTRY AND A MISREAD ONE MUST NOT LOOK THE SAME (#174).
    //
    //     Both used to come back as an empty QVector, so the gallery printed "No entries found. Check the
    //     registry URLs." for a registry that was reachable, correct and serving a document whose container
    //     this app simply did not recognise — which sends the reader to check a URL that is fine and hides a
    //     key-name drift for as long as nobody thinks to attach a debugger to a television.
    {
        // THE FIXTURE IS THE REAL DOCUMENT WITH ONE THING CHANGED: the container's key. Everything else —
        // the entry objects, their key names, their order, their dir convention — is the registry's own.
        // A fixture written from the reader's expectations would be a fixed point of the reader: rename
        // "themes2" to "catalog" on BOTH sides and it would still pass, which is exactly how the original
        // "themes"/"themes2" mismatch survived. This one cannot, because it is derived from the producer.
        const QByteArray live(kLiveIndex);
        // Guard the derivation itself. If the capture ever stopped containing the key, `renamed` would be
        // the unmodified live document and every assertion below would be testing the wrong thing while
        // still reading like a shape test.
        CHECK(live.contains(QByteArray("\"themes2\":")));
        QByteArray renamed(live);
        renamed.replace(QByteArray("\"themes2\":"), QByteArray("\"catalog\":"));
        CHECK(renamed != live);
        CHECK(!renamed.contains(QByteArray("\"themes2\":")));
        CHECK(renamed.contains(QByteArray("\"catalog\":")));
        // The entries themselves are untouched — this is a container-key mismatch, not a mangled document.
        CHECK(renamed.contains(QByteArray("\"dir\": \"themes2/Grid\"")));

        const ThemeRegistry::Index ix = ThemeRegistry::parseIndex(renamed);
        CHECK(ix.entries.isEmpty());
        CHECK(!ix.ok());                 // the whole issue: this is NOT an empty registry
        CHECK(!ix.shapeError.isEmpty());
        // Diagnosable FROM THE SCREEN. The message has to carry both halves of the mismatch, or the reader
        // is left knowing only that something is wrong: what the document actually holds…
        CHECK(ix.shapeError.contains(QStringLiteral("catalog")));
        // …and what this app was looking for.
        CHECK(ix.shapeError.contains(QStringLiteral("themes2")));

        // AND THE CONTRAST, which is the half that makes it a distinction rather than a new blanket
        // complaint: a container this reader recognises, holding nothing, is a registry saying it has
        // nothing. That must stay a plain empty list with no shape complaint attached.
        const ThemeRegistry::Index genuinelyEmpty = ThemeRegistry::parseIndex(QByteArray(R"({"themes2":[]})"));
        CHECK(genuinelyEmpty.entries.isEmpty());
        CHECK(genuinelyEmpty.ok());
        CHECK(genuinelyEmpty.shapeError.isEmpty());
    }

    // 3c. The other documents that are not a registry saying "nothing to offer".
    {
        // Not JSON at all, and — the one that actually happens in the field — an HTML page served with a
        // 200 because the URL points at a repository page, a login wall or a captive portal.
        const ThemeRegistry::Index junk = ThemeRegistry::parseIndex(QByteArray("not json at all"));
        CHECK(junk.entries.isEmpty());
        CHECK(!junk.ok());
        const ThemeRegistry::Index html =
            ThemeRegistry::parseIndex(QByteArray("<!DOCTYPE html><html><body>Sign in</body></html>"));
        CHECK(!html.ok());
        // A bare JSON ARRAY at the top level is not an object and cannot hold a container key.
        const ThemeRegistry::Index topArray =
            ThemeRegistry::parseIndex(QByteArray(R"([{"name":"Grid","dir":"themes2/Grid"}])"));
        CHECK(!topArray.ok());
        // WHICH complaint matters, not just that there is one. QJsonDocument::object() on a null document
        // hands back an EMPTY OBJECT, so all three of these would fall through to the missing-container
        // branch and be described as an index whose keys are wrong — sending the reader to compare a
        // sign-in page against the registry format. They get the sentence that fits instead.
        CHECK(junk.shapeError.contains(QStringLiteral("not a JSON object")));
        CHECK(html.shapeError.contains(QStringLiteral("not a JSON object")));
        CHECK(topArray.shapeError.contains(QStringLiteral("not a JSON object")));

        // An object with neither key. The message names the keys it DID find, because that string is the
        // one piece of evidence that turns "I don't understand this" into a fix.
        const ThemeRegistry::Index other =
            ThemeRegistry::parseIndex(QByteArray(R"({"addons":[{"id":"x"}],"version":2})"));
        CHECK(other.entries.isEmpty());
        CHECK(!other.ok());
        CHECK(other.shapeError.contains(QStringLiteral("addons")));
        CHECK(other.shapeError.contains(QStringLiteral("version")));
        CHECK(!other.shapeError.contains(QStringLiteral("not a JSON object")));

        // An empty object holds no keys to name, so it gets the sentence that does not promise any — and
        // that sentence is not the one a response which was never JSON gets. "{}" is a registry serving a
        // document; an HTML page is a URL that is not serving one, and the reader has to fix different
        // things in the two cases.
        const ThemeRegistry::Index bare = ThemeRegistry::parseIndex(QByteArray("{}"));
        CHECK(!bare.ok());
        CHECK(bare.shapeError.contains(QStringLiteral("themes2")));
        CHECK(bare.shapeError != junk.shapeError);

        // ATTACKER-SUPPLIED TEXT ON ITS WAY TO A LABEL. A public registry takes pull requests, so a
        // top-level key is a string someone else chose; the message is bounded here rather than trusted to
        // be short. One 200-character key: it is named, but ELIDED.
        QByteArray longKey("{\"");
        longKey += QByteArray(200, 'k');
        longKey += "\":1}";
        const ThemeRegistry::Index elided = ThemeRegistry::parseIndex(longKey);
        CHECK(!elided.ok());
        CHECK(elided.shapeError.contains(QString(24, QLatin1Char('k'))));       // said something about it
        CHECK(!elided.shapeError.contains(QString(200, QLatin1Char('k'))));     // but not all of it
        CHECK(elided.shapeError.size() < 300);

        // …and the COUNT of keys is bounded too. QJsonObject::keys() is sorted, so "aaa" is first and "fff"
        // is sixth: the first four are named and the rest are not.
        const ThemeRegistry::Index manyKeys = ThemeRegistry::parseIndex(
            QByteArray(R"({"aaa":1,"bbb":1,"ccc":1,"ddd":1,"eee":1,"fff":1})"));
        CHECK(!manyKeys.ok());
        CHECK(manyKeys.shapeError.contains(QStringLiteral("aaa")));
        CHECK(manyKeys.shapeError.contains(QStringLiteral("ddd")));
        CHECK(!manyKeys.shapeError.contains(QStringLiteral("eee")));
        CHECK(!manyKeys.shapeError.contains(QStringLiteral("fff")));

        // THE ENTRY SHAPE DRIFTING, which presents exactly like the container shape drifting: a recognised
        // array, elements in it, and not one of them usable. Here "dir" has been renamed "path" — the
        // change that would follow a registry format revision — and the message says how many were dropped,
        // because "0 of them were readable" and "2 of them were readable" are different conversations.
        const ThemeRegistry::Index drifted = ThemeRegistry::parseIndex(
            QByteArray(R"({"themes2":[{"name":"Grid","path":"themes2/Grid"},
                                      {"name":"Night","path":"themes2/Night"}]})"));
        CHECK(drifted.entries.isEmpty());
        CHECK(!drifted.ok());
        CHECK(drifted.shapeError.contains(QStringLiteral("2")));
        // The count is the number DROPPED, not a constant: one bad entry says one.
        const ThemeRegistry::Index oneDrifted = ThemeRegistry::parseIndex(
            QByteArray(R"({"themes2":[{"name":"Grid","path":"themes2/Grid"}]})"));
        CHECK(!oneDrifted.ok());
        CHECK(oneDrifted.shapeError.contains(QStringLiteral("1")));
        CHECK(!oneDrifted.shapeError.contains(QStringLiteral("2")));
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
        for (const char* b : bad) CHECK(ThemeRegistry::parseIndex(QByteArray(b)).entries.isEmpty());

        // A dir may be deeper than two segments — it is relative to the index URL's directory, and only its
        // LAST segment ever becomes a local folder name. Intentional, so pin it.
        {
            const ThemeRegistry::Index ix = ThemeRegistry::parseIndex(
                QByteArray(R"({"themes2":[{"name":"X","dir":"a/b/Grid"}]})"));
            CHECK(ix.entries.size() == 1);
            CHECK(ix.entries.value(0).folder() == QStringLiteral("Grid"));
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

    // 6. treeApiUrl — a raw index URL becomes the Trees API URL for the same repo and branch. Anything that
    //    is not raw.githubusercontent.com yields "", which is how a user-added registry on another host is
    //    told apart from a GitHub one (it lists, but cannot be installed from in-app).
    {
        CHECK(ThemeRegistry::treeApiUrl(
                  QStringLiteral("https://raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json"))
              == QStringLiteral("https://api.github.com/repos/cubman3134/everythingbox-themes/git/trees/main?recursive=1"));
        // A non-default branch survives.
        CHECK(ThemeRegistry::treeApiUrl(
                  QStringLiteral("https://raw.githubusercontent.com/o/r/dev/index.json"))
              == QStringLiteral("https://api.github.com/repos/o/r/git/trees/dev?recursive=1"));
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://example.com/themes/index.json")).isEmpty());
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://github.com/o/r/blob/main/index.json")).isEmpty());
        CHECK(ThemeRegistry::treeApiUrl(QString()).isEmpty());
        // Too few path segments to name a repo and a branch.
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://raw.githubusercontent.com/o/index.json")).isEmpty());
        // A percent-escape in the source URL stays escaped rather than being decoded back into the API URL,
        // where a decoded '?' or '#' would truncate it into a different request entirely.
        CHECK(ThemeRegistry::treeApiUrl(QStringLiteral("https://raw.githubusercontent.com/o/r/a%3Fb/index.json"))
              == QStringLiteral("https://api.github.com/repos/o/r/git/trees/a%3Fb?recursive=1"));
    }

    // 7. filesUnder — keep the blobs under dir/, return them RELATIVE to dir, ignore everything else.
    {
        const QByteArray tree = R"({"truncated":false,"tree":[
            {"path":"README.md","type":"blob","size":10},
            {"path":"index.json","type":"blob","size":20},
            {"path":"themes2","type":"tree"},
            {"path":"themes2/Channels","type":"tree"},
            {"path":"themes2/Channels/theme.json","type":"blob","size":4898},
            {"path":"themes2/Channels/sounds","type":"tree"},
            {"path":"themes2/Channels/sounds/move.wav","type":"blob","size":6658},
            {"path":"themes2/Channels/fonts/VarelaRound-Regular.ttf","type":"blob","size":132748},
            {"path":"themes2/Night/theme.json","type":"blob","size":11136}]})";
        const ThemeRegistry::Listing l = ThemeRegistry::filesUnder(tree, QStringLiteral("themes2/Channels"));
        CHECK(l.ok());
        CHECK(l.error.isEmpty());
        QStringList got = l.files; got.sort();
        const QStringList want = { QStringLiteral("fonts/VarelaRound-Regular.ttf"),
                                   QStringLiteral("sounds/move.wav"),
                                   QStringLiteral("theme.json") };
        CHECK(got == want);
        // A sibling folder whose name merely PREFIXES this one must not bleed in.
        const ThemeRegistry::Listing n = ThemeRegistry::filesUnder(tree, QStringLiteral("themes2/Night"));
        CHECK(n.ok());
        CHECK(n.files == QStringList{ QStringLiteral("theme.json") });
    }

    // 8. Every refusal is a REASON, never an empty success. An empty file list that reads as "installed"
    //    is the exact failure the dead Themes path had ("Nothing to download for this entry.").
    {
        // truncated: the listing is incomplete, so a "complete" install would silently omit files.
        const QByteArray trunc = R"({"truncated":true,"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(trunc, QStringLiteral("themes2/Grid")).ok());

        // No theme.json at the folder root: not a theme.
        const QByteArray notheme = R"({"tree":[
            {"path":"themes2/Grid/sounds/move.wav","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(notheme, QStringLiteral("themes2/Grid")).ok());

        // A nested theme.json does not count as the folder's own.
        const QByteArray nested = R"({"tree":[
            {"path":"themes2/Grid/sub/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(nested, QStringLiteral("themes2/Grid")).ok());

        // Folder absent from the tree entirely.
        const QByteArray missing = R"({"tree":[{"path":"themes2/Other/theme.json","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(missing, QStringLiteral("themes2/Grid")).ok());

        // An oversized file fails the WHOLE entry rather than being skipped — a theme missing its font is
        // a broken theme, and skipping quietly would install one.
        const QByteArray big = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/huge.bin","type":"blob","size":9000000}]})";
        CHECK(!ThemeRegistry::filesUnder(big, QStringLiteral("themes2/Grid")).ok());

        // A traversing path anywhere in the folder fails the whole entry.
        const QByteArray evil = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/../../../etc/passwd","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(evil, QStringLiteral("themes2/Grid")).ok());

        CHECK(!ThemeRegistry::filesUnder(QByteArray("not json"), QStringLiteral("themes2/Grid")).ok());
        // Never installable through a listing: an unusable dir.
        CHECK(!ThemeRegistry::filesUnder(QByteArray(R"({"tree":[]})"), QString()).ok());

        // Two listed paths that differ only in case are DISTINCT in the tree and the SAME file on Windows and
        // macOS, so installing both would have one silently overwrite the other and the theme would ship
        // whichever download happened to finish last. No per-path predicate can see this, so the listing as a
        // whole is refused.
        const QByteArray dupe = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/Theme.JSON","type":"blob","size":11}]})";
        CHECK(!ThemeRegistry::filesUnder(dupe, QStringLiteral("themes2/Grid")).ok());
        // The collapse can happen in a directory segment too, not just the filename.
        const QByteArray dupeDir = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/sounds/move.wav","type":"blob","size":10},
            {"path":"themes2/Grid/SOUNDS/Move.WAV","type":"blob","size":10}]})";
        CHECK(!ThemeRegistry::filesUnder(dupeDir, QStringLiteral("themes2/Grid")).ok());

        // A blob with NO size key is refused rather than read as size 0: toDouble() on an absent value is
        // zero, so treating "no size" as "small" would let anything through the one cap that decides how much
        // this app agrees to download.
        const QByteArray nosize = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/mystery.bin","type":"blob"}]})";
        CHECK(!ThemeRegistry::filesUnder(nosize, QStringLiteral("themes2/Grid")).ok());
        // A size that is not a number is the same claim in a different shape.
        const QByteArray strsize = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/mystery.bin","type":"blob","size":"10"}]})";
        CHECK(!ThemeRegistry::filesUnder(strsize, QStringLiteral("themes2/Grid")).ok());

        // The oversize message must state the cap that is actually enforced, not a number typed once and
        // left behind when kMaxFileBytes changes.
        const QByteArray big2 = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/huge.bin","type":"blob","size":9000000}]})";
        CHECK(ThemeRegistry::filesUnder(big2, QStringLiteral("themes2/Grid")).error
              .contains(QString::number(double(ThemeRegistry::kMaxFileBytes) / (1024.0 * 1024.0))));
    }

    // 8b. assetUrl — every segment percent-encoded, so a space in a font name resolves.
    {
        const QString base = QStringLiteral("https://raw.githubusercontent.com/o/r/main");
        CHECK(ThemeRegistry::assetUrl(base, QStringLiteral("themes2/Grid"), QStringLiteral("theme.json"))
              == base + QStringLiteral("/themes2/Grid/theme.json"));
        CHECK(ThemeRegistry::assetUrl(base, QStringLiteral("themes2/Grid"), QStringLiteral("sounds/move.wav"))
              == base + QStringLiteral("/themes2/Grid/sounds/move.wav"));
        CHECK(ThemeRegistry::assetUrl(base, QStringLiteral("themes2/Grid"), QStringLiteral("fonts/My Font.ttf"))
              == base + QStringLiteral("/themes2/Grid/fonts/My%20Font.ttf"));
        // The separators must survive encoding — a fully-encoded path would 404 on every asset.
        CHECK(!ThemeRegistry::assetUrl(base, QStringLiteral("themes2/Grid"),
                                       QStringLiteral("a/b.png")).contains(QStringLiteral("%2F")));
        // The registry's own folder name is just as free to hold a space as a font's is.
        CHECK(ThemeRegistry::assetUrl(base, QStringLiteral("themes2/My Grid"), QStringLiteral("theme.json"))
              == base + QStringLiteral("/themes2/My%20Grid/theme.json"));
        // A file literally NAMED "%2e%2e" is a legal filename and stays one: the '%' is escaped for the URL,
        // so the server is asked for that name rather than for "..".
        CHECK(ThemeRegistry::assetUrl(base, QStringLiteral("themes2/Grid"), QStringLiteral("%2e%2e/x.png"))
              == base + QStringLiteral("/themes2/Grid/%252e%252e/x.png"));
    }

    // 9. The file cap. kMaxFiles + 1 blobs (theme.json included) is a refusal — and exactly kMaxFiles is not,
    //    which is the assertion that keeps the check honest now that it is made INSIDE the loop (a hostile
    //    tree stops being accumulated at the limit rather than being collected in full and then refused).
    {
        auto tree = [](int extra) {
            QByteArray b = R"({"tree":[{"path":"themes2/Big/theme.json","type":"blob","size":10})";
            for (int i = 0; i < extra; ++i)
                b += QByteArray(",{\"path\":\"themes2/Big/f") + QByteArray::number(i)
                   + QByteArray(".png\",\"type\":\"blob\",\"size\":10}");
            return b + "]}";
        };
        const ThemeRegistry::Listing atCap =
            ThemeRegistry::filesUnder(tree(ThemeRegistry::kMaxFiles - 1), QStringLiteral("themes2/Big"));
        CHECK(atCap.ok());
        CHECK(atCap.files.size() == ThemeRegistry::kMaxFiles);
        CHECK(!ThemeRegistry::filesUnder(tree(ThemeRegistry::kMaxFiles), QStringLiteral("themes2/Big")).ok());
    }

    // 9b. themesRoot — the ONE definition of where a theme folder lives. It had been written out by hand in
    //     three places (ThemeEngine, AssetBootstrap twice, the gallery dialog), and the failure mode of a
    //     fourth is silent rather than loud: the installer writes into one directory and the picker scans
    //     another, so the install "succeeds" and the theme is nowhere. Asserted against the literal the
    //     shipped themes are extracted into, so a change to either has to change this line too.
    {
        CHECK(ThemeRegistry::themesRoot(QStringLiteral("/data/app")) == QStringLiteral("/data/app/themes2"));
        // No normalisation: callers hand this a data dir they already resolved, and a cleanPath here would
        // quietly rewrite it. The separator is added, nothing else is.
        CHECK(ThemeRegistry::themesRoot(QStringLiteral("C:/EverythingBox"))
              == QStringLiteral("C:/EverythingBox/themes2"));
        // Empty in, empty out — an unknown data dir must not resolve to "/themes2" at the filesystem root,
        // which is a real directory a real install could be written into. installFiles turns the emptiness
        // into a refusal with a reason rather than a write.
        CHECK(ThemeRegistry::themesRoot(QString()).isEmpty());
        QVector<QPair<QString, QByteArray>> one;
        one << qMakePair(QStringLiteral("theme.json"), QByteArray("{}"));
        QString rootErr;
        CHECK(!ThemeRegistry::installFiles(ThemeRegistry::themesRoot(QString()),
                                           QStringLiteral("Nowhere"), one, &rootErr));
        CHECK(!rootErr.isEmpty());
    }

    // 10. installFiles — the folder lands complete, WITH its subdirectories. The flattening both existing
    //     installers do (destDir + "/" + QFileInfo(rel).fileName()) would put sounds/move.wav at the theme
    //     root and leave every sound reference in the theme dangling.
    {
        const QString root = QDir::tempPath() + QStringLiteral("/eb-themereg-probe");
        QDir(root).removeRecursively();

        QVector<QPair<QString, QByteArray>> files;
        files << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Probe\"}"));
        files << qMakePair(QStringLiteral("sounds/move.wav"), QByteArray("RIFFwave"));
        files << qMakePair(QStringLiteral("fonts/F.ttf"), QByteArray("ttf"));

        QString err;
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), files, &err));
        CHECK(err.isEmpty());
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));
        CHECK(QFile::exists(root + QStringLiteral("/Probe/sounds/move.wav")));   // subpath PRESERVED
        CHECK(QFile::exists(root + QStringLiteral("/Probe/fonts/F.ttf")));
        CHECK(!QFile::exists(root + QStringLiteral("/Probe/move.wav")));         // NOT flattened

        // The staging an atomic install needs does not survive it. Anything left beside the theme is a
        // directory ThemeEngine::availableThemes() would scan, and one holding a theme.json would be OFFERED.
        //
        // QDir::Hidden is included deliberately, even though availableThemes()'s own filter does not: the
        // staging directory is dot-prefixed, so on Unix the default filter would hide the very leftover this
        // line exists to catch and the assertion would pass without checking anything. What is asserted here
        // is that the cleanup RAN, on every platform — not that a leftover would be invisible on some.
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == QStringList{ QStringLiteral("Probe") });

        // Re-installing the same folder replaces it wholesale rather than merging: a theme that dropped a
        // file must not keep the old one lying around.
        QVector<QPair<QString, QByteArray>> fewer;
        fewer << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Probe2\"}"));
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), fewer, &err));
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));
        CHECK(!QFile::exists(root + QStringLiteral("/Probe/sounds/move.wav")));
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == QStringList{ QStringLiteral("Probe") });   // and the REPLACED copy is gone too

        // A refusal leaves NOTHING behind — not a partial folder, and not a damaged previous install. This
        // is what makes a failed install safe: availableThemes() picks up anything with a theme.json.
        QVector<QPair<QString, QByteArray>> bad;
        bad << qMakePair(QStringLiteral("theme.json"), QByteArray("{}"));
        bad << qMakePair(QStringLiteral("../escape.txt"), QByteArray("nope"));
        err.clear();
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe3"), bad, &err));
        CHECK(!err.isEmpty());
        CHECK(!QDir(root + QStringLiteral("/Probe3")).exists());
        CHECK(!QFile::exists(root + QStringLiteral("/escape.txt")));

        // A refused install must not have touched the theme that was already there.
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe"), bad, &err) == false);
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));

        // An empty file set is a refusal, not an empty "success".
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe4"), {}, &err));
        // A folder name that is not a plain segment never becomes a directory.
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("../evil"), files, &err));
        CHECK(!ThemeRegistry::installFiles(root, QString(), files, &err));
        CHECK(!QDir(root + QStringLiteral("/Probe4")).exists());
        CHECK(!QDir(QDir::tempPath() + QStringLiteral("/evil")).exists());

        // The staging directory's own name IS a plain segment, so nothing in the path rules refuses it — and
        // installing "into" it would make the destination and the staging root the same directory, with every
        // rename below targeting a child of itself. Refused by name, on purpose, rather than by arithmetic.
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral(".eb-installing"), files, &err));
        CHECK(!QDir(root + QStringLiteral("/.eb-installing")).exists());
        CHECK(QFile::exists(root + QStringLiteral("/Probe/theme.json")));   // and it touched nothing

        // Two paths that collide case-insensitively are refused HERE too, not only in the listing: this is
        // the function that turns a string into a filename, and it does not get to assume its caller checked.
        QVector<QPair<QString, QByteArray>> clash;
        clash << qMakePair(QStringLiteral("theme.json"),  QByteArray("{\"n\":1}"));
        clash << qMakePair(QStringLiteral("Theme.JSON"),  QByteArray("{\"n\":2}"));
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe5"), clash, &err));
        CHECK(!QDir(root + QStringLiteral("/Probe5")).exists());

        // A percent-escape in a listed name is a LITERAL filename and must stay one. Decoding it anywhere on
        // the filesystem side would put "%2e%2e" back to ".." and walk the file out of the theme folder.
        QVector<QPair<QString, QByteArray>> pct;
        pct << qMakePair(QStringLiteral("theme.json"), QByteArray("{}"));
        pct << qMakePair(QStringLiteral("%2e%2e/x.png"), QByteArray("png"));
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Probe6"), pct, &err));
        CHECK(QFile::exists(root + QStringLiteral("/Probe6/%2e%2e/x.png")));   // the literal name
        CHECK(!QFile::exists(root + QStringLiteral("/x.png")));                // never decoded to ".."

        // A failure PART WAY THROUGH the writing: theme.json is already on disk when a later file cannot be
        // written, because a plain file of that name was just written where its parent folder would go.
        // Every other refusal above is caught before the first byte, so this is the only one that exercises
        // the unwind — and an unwind that leaves its staging behind is a phantom entry in the theme picker.
        QVector<QPair<QString, QByteArray>> midway;
        midway << qMakePair(QStringLiteral("theme.json"), QByteArray("{}"));
        midway << qMakePair(QStringLiteral("a"),          QByteArray("a file, not a folder"));
        midway << qMakePair(QStringLiteral("a/b.png"),    QByteArray("png"));
        err.clear();
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Probe7"), midway, &err));
        CHECK(!err.isEmpty());
        CHECK(!QDir(root + QStringLiteral("/Probe7")).exists());
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == (QStringList{ QStringLiteral("Probe"), QStringLiteral("Probe6") }));

        QDir(root).removeRecursively();
    }

    // 11. The one unwind that can LOSE the user's data: the swap half-completed. The existing theme was
    //     renamed out of the way, the new copy could not be renamed into its place, and putting the old one
    //     back failed too — at which point the folder parked in staging is the ONLY copy in existence and an
    //     unconditional tidy-up deletes it. Every other refusal in block 10 is caught before staging, and the
    //     mid-write case targets a folder that did not exist, so nothing above reaches this path at all.
    //
    //     Windows-only, and not for want of trying: POSIX rename() needs write+execute on BOTH parent
    //     directories, so the move OUT of themesRoot and the move BACK IN require exactly the same rights and
    //     no static permission state can fail one while allowing the other. NTFS separates them —
    //     FILE_ADD_SUBDIRECTORY governs creating an entry, DELETE the object being moved away — so denying
    //     just that one right on themesRoot lets the first rename succeed and fails both of the others,
    //     which is precisely the shape of the accident ("another process took the name back") this guards.
    //     The code under test is platform-neutral; only the way of provoking it is not.
    //
    //     TWO installs run under the deny, not one. Surviving the failure is only half the promise: the
    //     message tells the user their theme is safe in staging, and the next thing a user does with a
    //     failed install is retry it. A guarantee with a one-call lifetime is not a guarantee, so the second
    //     call is asserted to leave the parked copy exactly where the first one put it.
#ifdef Q_OS_WIN
    {
        const QString root = QDir::tempPath() + QStringLiteral("/eb-themereg-probe-unwind");
        QDir(root).removeRecursively();
        CHECK(QDir().mkpath(root));

        QVector<QPair<QString, QByteArray>> before;
        before << qMakePair(QStringLiteral("theme.json"),      QByteArray("{\"name\":\"Before\"}"));
        before << qMakePair(QStringLiteral("sounds/move.wav"), QByteArray("RIFFold"));
        QString err;
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Keep"), before, &err));

        // The deny below blocks creating a subdirectory of root, which includes the staging directory itself;
        // pre-create it so the install gets as far as the renames rather than failing at mkpath.
        CHECK(QDir().mkpath(root + QStringLiteral("/.eb-installing")));

        PSECURITY_DESCRIPTOR sd = nullptr;
        PACL original = nullptr;
        const bool hosted = canDenyAddSubdirectory(root, &sd, &original);
        if (!hosted)
            // Skipped LOUDLY, and distinguishable from a failure on purpose: "this host cannot stage the
            // accident" must not read as "the code lost the user's theme".
            std::printf("THEMEREG-SKIP block 11: this filesystem or security context cannot host a deny ACE "
                        "(non-NTFS TEMP, or SeRestorePrivilege in effect) - the half-completed-swap unwind "
                        "and its retry are unasserted on this machine.\n");

        if (hosted)
        {
            QVector<QPair<QString, QByteArray>> after;
            after << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"After\"}"));
            err.clear();
            const bool installed = ThemeRegistry::installFiles(root, QStringLiteral("Keep"), after, &err);

            const QString parked = root + QStringLiteral("/.eb-installing/Keep.replaced");

            // Snapshot BEFORE the retry runs. Asserting the parked copy only at the END cannot tell "the
            // first call never parked it" from "the first call parked it and the second call deleted it" —
            // which is exactly the distinction the retry assertions below exist to make.
            const bool parkedByFirst = QFile::exists(parked + QStringLiteral("/theme.json"))
                                    && QFile::exists(parked + QStringLiteral("/sounds/move.wav"));

            // THE RETRY, under the SAME deny — because the cause of a double-rename failure is typically
            // persistent (a parent that stopped accepting subdirectories, a name another process is holding),
            // so pressing Install again is the same roll rather than a fresh one. The error the first call
            // produced tells the user their theme is safe in `parked`, so the obvious next thing they do is
            // press Install again — and that call must not be what deletes it.
            QString retryErr;
            const bool retried = ThemeRegistry::installFiles(root, QStringLiteral("Keep"), after, &retryErr);
            restoreDacl(root, sd, original);        // before the assertions, so a failure still unwinds it

            CHECK(!installed);
            CHECK(!err.isEmpty());

            // The theme the user had is still on disk, whole, and is the OLD one — not the half-installed
            // new copy wearing its name.
            CHECK(parkedByFirst);
            CHECK(QFile::exists(parked + QStringLiteral("/theme.json")));
            CHECK(QFile::exists(parked + QStringLiteral("/sounds/move.wav")));
            QFile kept(parked + QStringLiteral("/theme.json"));
            CHECK(kept.open(QIODevice::ReadOnly));
            CHECK(kept.readAll() == QByteArray("{\"name\":\"Before\"}"));
            kept.close();

            // A folder the user cannot find is a folder we lost. The message has to name where it went.
            CHECK(err.contains(parked));

            // The half-built copy IS dropped — it is the one of the two that is safe to delete.
            CHECK(!QDir(root + QStringLiteral("/.eb-installing/Keep")).exists());

            // And the RETRY refused the same way rather than succeeding, going quiet, or staging over the
            // top: the destination is still unwritable, so the only honest outcome is another refusal that
            // names where the folder is. The install path opens with an unconditional tidy-up of both
            // staging names, so without a guard this second call is precisely what destroys the copy the
            // first call's message promised was safe.
            CHECK(!retried);
            CHECK(retryErr.contains(parked));
            CHECK(!QDir(root + QStringLiteral("/.eb-installing/Keep")).exists());
        }

        QDir(root).removeRecursively();
    }
#endif

    // 12. The retry's other half, and the one that needs no special filesystem — so this runs on EVERY leg,
    //     including the one where block 11 skips. The stranded state is hand-built rather than provoked (a
    //     copy parked in staging, the destination empty: exactly what block 11 leaves behind), and the retry
    //     is then given a file set that fails PART WAY THROUGH the writing.
    //
    //     What is asserted is that the parked copy is back in its normal place afterwards. That is the whole
    //     point of restoring before staging rather than merely refusing: the cause that stranded the folder
    //     may well have cleared by the time the user retries, and when it has, the right outcome is the
    //     user's theme where it belongs — not a folder they still have to move by hand. Without the restore
    //     the opening tidy-up deletes the parked copy and the mid-write unwind then leaves nothing at all:
    //     the last copy of the theme, destroyed by the act of retrying.
    {
        const QString root   = QDir::tempPath() + QStringLiteral("/eb-themereg-probe-restore");
        const QString parked = root + QStringLiteral("/.eb-installing/Strand.replaced");
        QDir(root).removeRecursively();
        CHECK(QDir().mkpath(parked + QStringLiteral("/sounds")));
        {
            QFile f(parked + QStringLiteral("/theme.json"));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("{\"name\":\"Stranded\"}");
            f.close();
            QFile g(parked + QStringLiteral("/sounds/move.wav"));
            CHECK(g.open(QIODevice::WriteOnly));
            g.write("RIFFold");
            g.close();
        }
        // The signal that tells a parked SURVIVOR from stale residue: the destination is empty.
        CHECK(!QDir(root + QStringLiteral("/Strand")).exists());

        QVector<QPair<QString, QByteArray>> midway;
        midway << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Retry\"}"));
        midway << qMakePair(QStringLiteral("a"),          QByteArray("a file, not a folder"));
        midway << qMakePair(QStringLiteral("a/b.png"),    QByteArray("png"));
        QString err;
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Strand"), midway, &err));

        QFile back(root + QStringLiteral("/Strand/theme.json"));
        CHECK(back.open(QIODevice::ReadOnly));
        CHECK(back.readAll() == QByteArray("{\"name\":\"Stranded\"}"));   // the OLD one, whole
        back.close();
        CHECK(QFile::exists(root + QStringLiteral("/Strand/sounds/move.wav")));
        // Restored, not merely spared: nothing is left in staging for the user to deal with by hand.
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == QStringList{ QStringLiteral("Strand") });

        // And the mirror image: a parked copy whose destination is OCCUPIED is the residue of a swap that
        // succeeded, so it is stale and must still be cleaned away. A guard that spared both would turn every
        // crash-after-success into a permanent second copy of the theme.
        QVector<QPair<QString, QByteArray>> good;
        good << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Fresh\"}"));
        CHECK(QDir().mkpath(parked));
        {
            QFile f(parked + QStringLiteral("/theme.json"));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("{\"name\":\"Residue\"}");
            f.close();
        }
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Strand"), good, &err));
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == QStringList{ QStringLiteral("Strand") });
        CHECK(!QDir(parked).exists());

        QDir(root).removeRecursively();
    }

    // 13. THE ALIAS. `tmp` is <staging>/<folder> and a parked survivor is <staging>/<folder>.replaced, so a
    //     registry entry whose dir is "themes2/Keep.replaced" — a plain segment, and a name anyone may open a
    //     pull request for against a public registry — has a staging path byte-identical to the place Keep's
    //     survivor is parked. installFiles opens with an UNCONDITIONAL removeRecursively() of `tmp`, well
    //     before the survivor guard, so merely pressing Install on that entry destroys the one folder blocks
    //     11 and 12 exist to preserve — whether or not the install then succeeds.
    //
    //     Staged exactly as block 12 stages it: a copy parked in staging with its destination EMPTY, which is
    //     the state that says "survivor", not "residue". The install under test names the COLLIDING folder,
    //     not the parked one, so nothing about it looks like a retry of the theme it would destroy.
    //
    //     CONSIDERED AND NOT CLOSED, so the next reader knows it was weighed rather than missed: on a
    //     case-insensitive volume a case-VARIANT of an ordinary folder name can still reach a parked
    //     survivor. With Keep's survivor parked and themes2/Keep empty, installing "KEEP" has QDir resolve
    //     `old` (<staging>/KEEP.replaced) to the same directory as <staging>/Keep.replaced, so the restore
    //     branch above picks it up and renames it to themes2/KEEP — the survivor is recovered, under a name
    //     one letter different from the one it had. That is a far narrower thing than the alias this block
    //     pins: it destroys nothing, it needs the rare stranded state to already exist, and the only visible
    //     consequence is the folder's capitalisation. Closing it would mean case-folding the whole
    //     themes2 namespace on every install, which is a much larger rule than the harm justifies.
    {
        const QString root   = QDir::tempPath() + QStringLiteral("/eb-themereg-probe-alias");
        const QString parked = root + QStringLiteral("/.eb-installing/Keep.replaced");
        QDir(root).removeRecursively();
        CHECK(QDir().mkpath(parked + QStringLiteral("/sounds")));
        {
            QFile f(parked + QStringLiteral("/theme.json"));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("{\"name\":\"Survivor\"}");
            f.close();
            QFile g(parked + QStringLiteral("/sounds/move.wav"));
            CHECK(g.open(QIODevice::WriteOnly));
            g.write("RIFFold");
            g.close();
        }
        CHECK(!QDir(root + QStringLiteral("/Keep")).exists());   // destination empty => a survivor, not residue

        QVector<QPair<QString, QByteArray>> files;
        files << qMakePair(QStringLiteral("theme.json"), QByteArray("{\"name\":\"Attacker\"}"));

        QString err;
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Keep.replaced"), files, &err));
        CHECK(!err.isEmpty());
        // The survivor is untouched — every byte of it, not merely the folder.
        QFile kept(parked + QStringLiteral("/theme.json"));
        CHECK(kept.open(QIODevice::ReadOnly));
        CHECK(kept.readAll() == QByteArray("{\"name\":\"Survivor\"}"));
        kept.close();
        CHECK(QFile::exists(parked + QStringLiteral("/sounds/move.wav")));
        // And the refused install wrote nothing of its own.
        CHECK(!QDir(root + QStringLiteral("/Keep.replaced")).exists());
        CHECK(QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)
              == QStringList{ QStringLiteral(".eb-installing") });

        // Case does not launder it: on Windows and on a default macOS volume "Keep.REPLACED" and
        // "Keep.replaced" are one directory, so a case-exact refusal would admit the spelling that aliases.
        err.clear();
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral("Keep.REPLACED"), files, &err));
        CHECK(!err.isEmpty());
        CHECK(QFile::exists(parked + QStringLiteral("/theme.json")));
        // Same for the staging directory's own name, which is refused by name for the same class of reason.
        CHECK(!ThemeRegistry::installFiles(root, QStringLiteral(".EB-Installing"), files, &err));

        // The suffix is refused, not the substring: a theme legitimately called "Replaced" or one that merely
        // CONTAINS the word installs normally. Rejecting more than aliases would be a name rule nobody asked
        // for, applied to a public registry.
        err.clear();
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Replaced"), files, &err));
        CHECK(err.isEmpty());
        CHECK(QFile::exists(root + QStringLiteral("/Replaced/theme.json")));
        CHECK(ThemeRegistry::installFiles(root, QStringLiteral("Grid.replaced.v2"), files, &err));
        CHECK(QFile::exists(root + QStringLiteral("/Grid.replaced.v2/theme.json")));
        // …and the survivor is STILL there after two successful installs beside it.
        CHECK(QFile::exists(parked + QStringLiteral("/theme.json")));

        QDir(root).removeRecursively();
    }

    // 14. acceptDownloadedBytes — the caps applied to bytes that actually ARRIVED, which is a different
    //     question from the one filesUnder answers. filesUnder trusts the size in the TREE RESPONSE; the
    //     blobs are fetched afterwards, as separate requests against branch HEAD, so a registry that commits
    //     small files, gets listed, and then force-pushes large ones serves whatever it likes into an
    //     unbounded readAll(). Both surfaces run this on every file, which is why it lives here.
    {
        QString err;
        // A perfectly ordinary theme file, and the running total carried with it.
        CHECK(ThemeRegistry::acceptDownloadedBytes(4898, 0, QStringLiteral("theme.json"), &err));
        CHECK(err.isEmpty());
        CHECK(ThemeRegistry::acceptDownloadedBytes(132748, 4898, QStringLiteral("fonts/F.ttf"), &err));

        // Exactly the per-file cap is fine; one byte over is not. The boundary is asserted in both
        // directions so a cap written as >= or > cannot pass by accident.
        err.clear();
        CHECK(ThemeRegistry::acceptDownloadedBytes(ThemeRegistry::kMaxFileBytes, 0,
                                                   QStringLiteral("big.bin"), &err));
        CHECK(err.isEmpty());
        CHECK(!ThemeRegistry::acceptDownloadedBytes(ThemeRegistry::kMaxFileBytes + 1, 0,
                                                    QStringLiteral("big.bin"), &err));
        CHECK(!err.isEmpty());
        CHECK(err.contains(QStringLiteral("big.bin")));                 // says WHICH file
        // The message states the cap that is actually enforced, not a number typed once and left behind.
        CHECK(err.contains(QString::number(double(ThemeRegistry::kMaxFileBytes) / (1024.0 * 1024.0))));

        // THE TOTAL. Every one of these files is inside the per-file cap, so the per-file rule alone lets the
        // whole set through: kMaxFiles of them is 512 MB held in memory at once, on a product that ships to a
        // 32-bit armv7 box. This is the budget that was deferred from an earlier task and never landed.
        {
            qint64 total = 0;
            int accepted = 0;
            QString terr;
            for (int i = 0; i < ThemeRegistry::kMaxFiles; ++i)
            {
                if (!ThemeRegistry::acceptDownloadedBytes(ThemeRegistry::kMaxFileBytes, total,
                                                          QStringLiteral("f%1.bin").arg(i), &terr))
                    break;
                total += ThemeRegistry::kMaxFileBytes;
                ++accepted;
            }
            CHECK(accepted < ThemeRegistry::kMaxFiles);                 // it stopped before the 512 MB
            CHECK(total <= ThemeRegistry::kMaxTotalBytes);
            CHECK(!terr.isEmpty());
            CHECK(terr.contains(QString::number(double(ThemeRegistry::kMaxTotalBytes) / (1024.0 * 1024.0))));
        }

        // The total boundary itself, both directions: a set landing exactly on the budget is accepted, and
        // one byte more is not — including when that byte arrives in a file that is itself well within the
        // per-file cap, which is the case the per-file check cannot see.
        err.clear();
        CHECK(ThemeRegistry::acceptDownloadedBytes(1, ThemeRegistry::kMaxTotalBytes - 1,
                                                   QStringLiteral("last.bin"), &err));
        CHECK(err.isEmpty());
        CHECK(!ThemeRegistry::acceptDownloadedBytes(2, ThemeRegistry::kMaxTotalBytes - 1,
                                                    QStringLiteral("last.bin"), &err));
        CHECK(!err.isEmpty());

        // The total budget must be the binding constraint, or it is decoration: kMaxFiles files at the
        // per-file cap has to exceed it, and one file at the per-file cap has to fit inside it.
        CHECK(ThemeRegistry::kMaxTotalBytes < qint64(ThemeRegistry::kMaxFiles) * ThemeRegistry::kMaxFileBytes);
        CHECK(ThemeRegistry::kMaxTotalBytes >= ThemeRegistry::kMaxFileBytes);

        // A negative count is not a caller we understand, and both comparisons above would invert on one.
        CHECK(!ThemeRegistry::acceptDownloadedBytes(-1, 0, QStringLiteral("x"), &err));
        CHECK(!ThemeRegistry::acceptDownloadedBytes(1, -1, QStringLiteral("x"), &err));

        // A null error pointer is a legal caller — the predicate is the answer, the string is a courtesy.
        CHECK(!ThemeRegistry::acceptDownloadedBytes(ThemeRegistry::kMaxFileBytes + 1, 0,
                                                    QStringLiteral("x"), nullptr));
    }

    // 15. remainingDownloadBudget — the SAME rule, asked before the bytes exist rather than after. This is
    //     what makes the caps a bound on peak memory instead of a bound on regret: the two download loops
    //     hand this number to the network layer, which caps the reply's read buffer at it and aborts the
    //     transfer when downloadProgress passes it. Without it, QNetworkAccessManager buffers a whole
    //     response by default and one 500 MB file is 500 MB resident before acceptDownloadedBytes is even
    //     reached — which on a 32-bit armv7 box is the OOM the caps were written to prevent.
    //
    //     WHAT THIS PROBE CANNOT DO, said plainly rather than implied by a green line: it cannot make a
    //     network request, so the arrival-time abort itself — setReadBufferSize, the downloadProgress
    //     handler, reply->abort() — is NOT covered here or anywhere else in this suite. What is covered is
    //     the only part that can be a pure predicate: the number those call sites are given, and the fact
    //     that it draws exactly the same boundary acceptDownloadedBytes does.
    {
        // A fresh entry may bring one full file. min(per-file, whole budget), and kMaxFileBytes is the
        // smaller by the constant relationship block 14 pins.
        CHECK(ThemeRegistry::remainingDownloadBudget(0) == ThemeRegistry::kMaxFileBytes);
        // Spent to the last byte: nothing more, and never a negative allowance — a downstream comparison
        // against a negative budget would read as unbounded, which is the failure this whole block is about.
        CHECK(ThemeRegistry::remainingDownloadBudget(ThemeRegistry::kMaxTotalBytes) == 0);
        CHECK(ThemeRegistry::remainingDownloadBudget(ThemeRegistry::kMaxTotalBytes + 1) == 0);
        CHECK(ThemeRegistry::remainingDownloadBudget(-1) == 0);
        // Near the end the TOTAL is what binds, not the per-file cap: what is left is what is left.
        CHECK(ThemeRegistry::remainingDownloadBudget(ThemeRegistry::kMaxTotalBytes - 1) == 1);
        CHECK(ThemeRegistry::remainingDownloadBudget(ThemeRegistry::kMaxTotalBytes
                                                     - ThemeRegistry::kMaxFileBytes / 2)
              == ThemeRegistry::kMaxFileBytes / 2);
        // …and while there is more than a file's worth left, the per-file cap is.
        CHECK(ThemeRegistry::remainingDownloadBudget(ThemeRegistry::kMaxFileBytes)
              == ThemeRegistry::kMaxFileBytes);

        // THE LOAD-BEARING ONE. The budget handed to the network and the predicate applied to what arrives
        // have to draw the SAME line: one byte tighter and transfers are aborted that the predicate would
        // have accepted (a theme that fits refuses to install); one byte looser and the abort is decoration,
        // with only the after-the-fact check really applying the cap. Swept across the interesting values of
        // the running total rather than asserted at one point.
        const qint64 marks[] = { 0,
                                 1,
                                 ThemeRegistry::kMaxFileBytes,
                                 ThemeRegistry::kMaxTotalBytes / 2,
                                 ThemeRegistry::kMaxTotalBytes - ThemeRegistry::kMaxFileBytes,
                                 ThemeRegistry::kMaxTotalBytes - 1 };
        for (const qint64 soFar : marks)
        {
            const qint64 budget = ThemeRegistry::remainingDownloadBudget(soFar);
            CHECK(budget > 0);
            CHECK(ThemeRegistry::acceptDownloadedBytes(budget, soFar, QStringLiteral("f.bin"), nullptr));
            CHECK(!ThemeRegistry::acceptDownloadedBytes(budget + 1, soFar, QStringLiteral("f.bin"), nullptr));
        }

        // Walking a whole entry the way the download loops do: the budget never lets the running total past
        // the cap, and it reaches exactly zero rather than stalling above it.
        qint64 got = 0;
        int steps = 0;
        while (got < ThemeRegistry::kMaxTotalBytes && steps < 1000)
        {
            const qint64 budget = ThemeRegistry::remainingDownloadBudget(got);
            CHECK(budget > 0);
            got += budget;
            ++steps;
        }
        CHECK(got == ThemeRegistry::kMaxTotalBytes);
        CHECK(ThemeRegistry::remainingDownloadBudget(got) == 0);

        // The listing budget is a separate allowance from the theme's, and has to be a real one.
        CHECK(ThemeRegistry::kMaxListingBytes > 0);
    }

    // 16. filesUnder's CLAIMED total. acceptDownloadedBytes stays authoritative — a listing may understate,
    //     and the bytes that arrive are what count — but a listing that says up front that it is over budget
    //     should be refused before the first request rather than after up to kMaxFiles downloads, each
    //     behind its own 20 s wall. This is the cheap half of a check whose expensive half already exists.
    {
        // A listing that CLAIMS exactly `target` bytes, in as few files as the per-file cap allows, the first
        // of them theme.json (filesUnder requires one). Built from the constants rather than spelled out, so
        // the fixture cannot drift from the numbers it exists to sit either side of.
        auto treeClaiming = [](qint64 target) {
            QByteArray t = "{\"tree\":[";
            qint64 left = target;
            int i = 0;
            while (left > 0)
            {
                const qint64 chunk = left < ThemeRegistry::kMaxFileBytes ? left : ThemeRegistry::kMaxFileBytes;
                if (i > 0) t += ",";
                const QString name = i == 0 ? QStringLiteral("theme.json")
                                            : QStringLiteral("f%1.bin").arg(i);
                t += QStringLiteral("{\"path\":\"themes2/Grid/%1\",\"type\":\"blob\",\"size\":%2}")
                         .arg(name, QString::number(chunk)).toUtf8();
                left -= chunk;
                ++i;
            }
            t += "]}";
            return t;
        };

        // EXACTLY the budget is installable — the boundary asserted from the accepting side first, so a check
        // written one comparison too tight cannot pass this block by refusing everything.
        const ThemeRegistry::Listing el =
            ThemeRegistry::filesUnder(treeClaiming(ThemeRegistry::kMaxTotalBytes),
                                      QStringLiteral("themes2/Grid"));
        CHECK(el.ok());
        CHECK(el.files.size() <= ThemeRegistry::kMaxFiles);   // or the file cap, not the total, is answering

        // One byte more is not, and the byte rides on a file that is itself well inside the per-file cap —
        // the case the per-file check cannot see.
        const ThemeRegistry::Listing ol =
            ThemeRegistry::filesUnder(treeClaiming(ThemeRegistry::kMaxTotalBytes + 1),
                                      QStringLiteral("themes2/Grid"));
        CHECK(!ol.ok());
        // …and it says which cap it is, with the number the code actually enforces rather than one typed in.
        CHECK(ol.error.contains(QString::number(double(ThemeRegistry::kMaxTotalBytes) / (1024.0 * 1024.0))));

        // Comfortably over, the shape a hostile listing would actually take: still refused, and still by the
        // total rather than by the file count.
        const ThemeRegistry::Listing bl =
            ThemeRegistry::filesUnder(treeClaiming(ThemeRegistry::kMaxTotalBytes * 2),
                                      QStringLiteral("themes2/Grid"));
        CHECK(!bl.ok());
        CHECK(bl.error.contains(QString::number(double(ThemeRegistry::kMaxTotalBytes) / (1024.0 * 1024.0))));

        // A NEGATIVE claimed size is refused, not subtracted: left alone it would buy back budget for the
        // files after it, which is the one way a running total can be talked out of binding.
        const QByteArray neg = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":10},
            {"path":"themes2/Grid/refund.bin","type":"blob","size":-2000000000}]})";
        CHECK(!ThemeRegistry::filesUnder(neg, QStringLiteral("themes2/Grid")).ok());

        // An ordinary theme is untouched by any of this — the check must not have become a second, tighter
        // file cap by arithmetic accident.
        const QByteArray ok = R"({"tree":[
            {"path":"themes2/Grid/theme.json","type":"blob","size":4898},
            {"path":"themes2/Grid/sounds/move.wav","type":"blob","size":6658},
            {"path":"themes2/Grid/fonts/V.ttf","type":"blob","size":132748}]})";
        CHECK(ThemeRegistry::filesUnder(ok, QStringLiteral("themes2/Grid")).ok());
    }

    // ---- 12. The `decorations` section of the SAME index document (#187) ------------------------------
    // Decoration (bezel) packs are published in the index the themes are published in, under their own key,
    // so a registry serves one document and this app reads it with two readers. What is pinned here is the
    // SHAPE contract — #174's rule, applied to a second section: an index this reader does not understand
    // presents as an error, and an index that simply has no packs presents as an empty list. The install
    // side lives in probe_decopack.
    {
        const QByteArray good = R"({
            "themes2": [ {"name":"Grid","dir":"themes2/Grid"} ],
            "decorations": [
              {"id":"arcade-shells","name":"Arcade Shells","systems":["snes","nes"],
               "author":"nobody","license":"CC0-1.0","version":"1.2.0",
               "zip":"decorations/arcade-shells-1.2.0.zip",
               "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "preview":"decorations/arcade-shells.png","size":4194304} ]})";
        const DecorationPack::Index ix = DecorationPack::parseDecorations(good);
        CHECK(ix.ok());
        CHECK(ix.entries.size() == 1);
        CHECK(ix.entries[0].id == QStringLiteral("arcade-shells"));
        CHECK(ix.entries[0].name == QStringLiteral("Arcade Shells"));
        CHECK(ix.entries[0].systems == (QStringList{ QStringLiteral("snes"), QStringLiteral("nes") }));
        CHECK(ix.entries[0].version == QStringLiteral("1.2.0"));
        CHECK(ix.entries[0].zip == QStringLiteral("decorations/arcade-shells-1.2.0.zip"));
        CHECK(ix.entries[0].size == 4194304);
        CHECK(ix.entries[0].isValid());
        // The two readers are independent: the themes half of this same document still parses, so adding a
        // section cannot be what breaks a registry for the themes it was already serving.
        CHECK(ThemeRegistry::parseIndex(good).ok());
        CHECK(ThemeRegistry::parseIndex(good).entries.size() == 1);
    }
    {
        // NO `decorations` key — every registry that exists today. Understood, and EMPTY: this is the one
        // place the decorations reader deliberately differs from the themes reader, because a themes-only
        // registry is a document this app understands perfectly. Calling it a shape error would put a red
        // "could not be read" card beside every theme in the default registry.
        const DecorationPack::Index ix =
            DecorationPack::parseDecorations(R"({"themes2":[{"name":"Grid","dir":"themes2/Grid"}]})");
        CHECK(ix.ok());
        CHECK(ix.entries.isEmpty());
    }
    {
        // An EMPTY decorations array is a registry saying it has no packs. Also not an error — that is the
        // statement #174 exists to keep distinguishable from the ones below.
        const DecorationPack::Index ix = DecorationPack::parseDecorations(R"({"decorations":[]})");
        CHECK(ix.ok());
        CHECK(ix.entries.isEmpty());
    }
    {
        // MALFORMED, four ways, and every one of them must be an ERROR with a readable reason rather than
        // an empty list. Each message is checked for the word that makes it actionable off a television.
        const DecorationPack::Index notJson = DecorationPack::parseDecorations("<html>404</html>");
        CHECK(!notJson.ok());
        CHECK(notJson.shapeError.contains(QStringLiteral("not a JSON object")));

        const DecorationPack::Index notArray = DecorationPack::parseDecorations(R"({"decorations":{"a":1}})");
        CHECK(!notArray.ok());
        CHECK(notArray.shapeError.contains(QStringLiteral("decorations")));
        CHECK(notArray.entries.isEmpty());

        // The ENTRY shape drifting: elements are present and not one is installable. Indistinguishable from
        // "no packs" if both just produce an empty list, which is the whole defect.
        const DecorationPack::Index allDropped = DecorationPack::parseDecorations(
            R"({"decorations":[{"id":"a","systems":["snes"],"url":"a.zip","sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}]})");
        CHECK(!allDropped.ok());
        CHECK(allDropped.shapeError.contains(QStringLiteral("none of them is installable")));

        const DecorationPack::Index notObjects = DecorationPack::parseDecorations(R"({"decorations":["a","b"]})");
        CHECK(!notObjects.ok());
    }
    {
        // A MISSING or malformed digest drops the entry — it is not "install it without checking". With one
        // good entry beside it the section still parses, so one bad publish does not hide the whole set.
        const DecorationPack::Index ix = DecorationPack::parseDecorations(R"({"decorations":[
            {"id":"nodigest","systems":["snes"],"zip":"a.zip"},
            {"id":"shortdigest","systems":["snes"],"zip":"b.zip","sha256":"abcd"},
            {"id":"UPPER","systems":["snes"],"zip":"c.zip",
             "sha256":"E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"},
            {"id":"good","systems":["snes"],"zip":"d.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}]})");
        CHECK(ix.ok());
        CHECK(ix.entries.size() == 1);
        CHECK(ix.entries[0].id == QStringLiteral("good"));
        // Uppercase hex is refused rather than folded: the comparison site takes lowercase toHex(), and an
        // index that ships it uppercase is a thing to fix in the index, not at every future comparison.
        CHECK(!DecorationPack::isSha256Hex(
            QStringLiteral("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855")));
        CHECK(DecorationPack::isSha256Hex(
            QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
    }
    {
        // An `id` or a `systems` entry that cannot become a folder name is dropped, for the same reason a
        // theme's `dir` is: it is about to be a path. Two entries claiming one id are collapsed to the
        // first — they would install over each other and uninstall as one.
        const DecorationPack::Index ix = DecorationPack::parseDecorations(R"({"decorations":[
            {"id":"../escape","systems":["snes"],"zip":"a.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"id":"nested/id","systems":["snes"],"zip":"b.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"id":"ok","systems":["../etc"],"zip":"c.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"id":"dup","systems":["snes"],"zip":"d.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"id":"DUP","systems":["nes"],"zip":"e.zip",
             "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}]})");
        CHECK(ix.ok());
        CHECK(ix.entries.size() == 1);
        CHECK(ix.entries[0].id == QStringLiteral("dup"));
    }
    {
        // The install layout, stated here rather than derived: bezelsRoot is the SAME <data>/bezels folder
        // #106's renderer already scans, and packDir refuses anything that could climb out of it.
        CHECK(DecorationPack::bezelsRoot(QStringLiteral("/data")) == QStringLiteral("/data/bezels"));
        CHECK(DecorationPack::bezelsRoot(QString()).isEmpty());
        CHECK(DecorationPack::packDir(QStringLiteral("/data/bezels"), QStringLiteral("snes"),
                                      QStringLiteral("shells"))
              == QStringLiteral("/data/bezels/snes/shells"));
        CHECK(DecorationPack::packDir(QStringLiteral("/data/bezels"), QStringLiteral("snes"),
                                      QStringLiteral("..")).isEmpty());
        CHECK(DecorationPack::packDir(QStringLiteral("/data/bezels"), QStringLiteral(".."),
                                      QStringLiteral("shells")).isEmpty());
        CHECK(DecorationPack::packDir(QString(), QStringLiteral("snes"), QStringLiteral("shells")).isEmpty());
    }

    if (failures == 0) std::printf("THEMEREG-OK\n");
    return failures == 0 ? 0 : 1;
}
