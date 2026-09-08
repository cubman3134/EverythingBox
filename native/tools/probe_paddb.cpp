// Headless check that the bundled SDL controller database actually TRAVELS with the binary (issue #317).
//
// native/gamecontrollerdb.txt is the community SDL_GameControllerDB this app bundles so that uncommon and
// third-party pads map to the standard layout. Both loaders spell the same path: Gamepad::Impl::bringUp()
// and EmulatorManager's out-of-process enumeration each call
// SDL_GameControllerAddMappingsFromFile(SDL_GetBasePath() + "gamecontrollerdb.txt") — the executable's OWN
// directory — and both are deliberately best-effort, because a missing database must not stop the app. That
// is the whole trap: SDL returns -1, nobody looks, the app quietly runs on SDL's built-in mappings, and the
// only symptom is a pad reported as unrecognized somewhere else entirely. For the file's entire life in this
// repo there was no build rule that put it in that directory, and nothing said so.
//
// Every probe binary is built into the same directory as the app (see the data-dir isolation note at the end
// of native/CMakeLists.txt), so "beside this probe" and "what SDL_GetBasePath() returns for the app" are the
// same folder on the platforms this suite runs on. Check 2 asserts that rather than assuming it, so the
// probe cannot pass by looking in a directory SDL never reads.
//
// What is pinned:
//   1. the database EXISTS in the app directory and is not empty;
//   2. that directory IS the app/probe directory — the one SDL_GetBasePath() hands the loaders;
//   3. the staged bytes are IDENTICAL to native/gamecontrollerdb.txt, so it is the bundled, provenance-
//      recorded database that travelled and not a leftover or a truncated copy;
//   4. SDL's own loader reports a NON-ZERO mapping count for it. A file that is present but unreadable or
//      unparseable is the same bug wearing a hat, so the count comes from the real call, not from a parse.
//      It is asserted as "> 0" and printed, never compared against the line count: SDL keeps only the
//      entries whose `platform:` field matches the running platform, so the accepted count is a small
//      fraction of the file and pinning a number here would fail on the next database refresh.
//
// Without SDL2 at configure time (controller support is optional) claim 4 falls back to counting parseable
// mapping lines in the staged file and says so on stdout; claims 1-3 are unaffected.
//
// Prints PADDB-OK on success; any failure prints PADDB-FAIL <cond> (line) and exits non-zero.
#include <QCoreApplication>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cstdio>

#ifdef EB_PADDB_HAVE_SDL
#  define SDL_MAIN_HANDLED   // we never let SDL take over main() (same rule as Gamepad.cpp)
#  include <SDL.h>
#endif

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PADDB-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The fallback counter: a mapping line is "<guid>,<name>,<binding>,..." — at least two commas, and neither
// blank nor a comment. Deliberately dumber than SDL's parser; it only has to prove the file has content of
// the right shape when SDL is not linked in.
static int countMappingLines(const QByteArray& blob)
{
    int n = 0;
    for (const QByteArray& raw : blob.split('\n'))
    {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.count(',') >= 2) ++n;
    }
    return n;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString staged = QStringLiteral(EB_PADDB_STAGED);
    const QString source = QStringLiteral(EB_PADDB_SOURCE);

    // 1. Present, and not a zero-byte placeholder.
    const QFileInfo stagedInfo(staged);
    CHECK(stagedInfo.exists() && stagedInfo.isFile());
    if (!stagedInfo.exists())
    {
        std::fprintf(stderr,
                     "PADDB: no controller database at '%s'.\n"
                     "PADDB: that path is staged by the everythingbox target's POST_BUILD rule, so build the\n"
                     "PADDB: app (cmake --build <dir> --target everythingbox) before running this probe. If the\n"
                     "PADDB: app HAS been built, the staging rule in native/CMakeLists.txt is gone or wrong and\n"
                     "PADDB: SDL is running on its built-in mappings alone — issue #317.\n",
                     qPrintable(staged));
        std::fprintf(stderr, "PADDB: %d check(s) failed\n", failures);
        return 1;
    }
    CHECK(stagedInfo.size() > 0);

    // 2. It is in the directory SDL_GetBasePath() returns for the app — which is this probe's own directory,
    //    because probes are built beside the app exe. macOS is the exception: there the bundle's
    //    Contents/Resources is what SDL_GetBasePath() returns, and the probe is not inside the bundle.
#ifndef Q_OS_MACOS
    const QString stagedDir = stagedInfo.canonicalPath();
    const QString appDir    = QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
    CHECK(!stagedDir.isEmpty() && stagedDir == appDir);
    if (stagedDir != appDir)
        std::fprintf(stderr, "PADDB: staged in '%s' but SDL will read '%s'\n",
                     qPrintable(stagedDir), qPrintable(appDir));
#endif

    // 3. Byte-identical to the bundled source. copy_if_different guarantees this; asserting it is what turns
    //    "a gamecontrollerdb.txt is there" into "OUR gamecontrollerdb.txt is there".
    QFile stagedFile(staged);
    QFile sourceFile(source);
    CHECK(stagedFile.open(QIODevice::ReadOnly));
    CHECK(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray stagedBytes = stagedFile.readAll();
    const QByteArray sourceBytes = sourceFile.readAll();
    CHECK(!sourceBytes.isEmpty());
    CHECK(stagedBytes == sourceBytes);

    // 4. The mapping count, from the loader the app actually uses.
#ifdef EB_PADDB_HAVE_SDL
    SDL_SetMainReady();   // no SDL_Init: adding mappings does not need a subsystem, and CI has no display
    const int added = SDL_GameControllerAddMappingsFromFile(staged.toLocal8Bit().constData());
    std::printf("PADDB: SDL_GameControllerAddMappingsFromFile accepted %d mapping(s) for this platform\n",
                added);
    CHECK(added > 0);
    if (added < 0)
        std::fprintf(stderr, "PADDB: SDL refused the file: %s\n", SDL_GetError());
#else
    const int parsed = countMappingLines(stagedBytes);
    std::printf("PADDB: SDL2 not linked into this probe; %d parseable mapping line(s) in the staged file\n",
                parsed);
    CHECK(parsed > 0);
#endif

    if (failures == 0) std::printf("PADDB-OK\n");
    else               std::fprintf(stderr, "PADDB: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
