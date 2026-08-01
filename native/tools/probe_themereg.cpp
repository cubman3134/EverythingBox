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

    if (failures == 0) std::printf("THEMEREG-OK\n");
    return failures == 0 ? 0 : 1;
}
