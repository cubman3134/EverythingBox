// Headless check of the remote-document cache naming (src/core/RemoteDocCache.h). PURE — QtCore only, no
// disk, no network. Prints REMOTEDOCCACHE-OK on success; any failure prints REMOTEDOCCACHE-FAIL <cond>
// (line) and exits non-zero.
//
// THE BUG IT PINS: two callers write into this cache — the open that fetches a document because you pressed
// it, and the pre-fetch that fetches the NEXT volume before you ask for it. They are the same file only if
// they agree on its name, and "agree" cannot mean "both happen to build the same string inline". A drift
// between two inline copies of a hashing rule is invisible: nothing breaks, the pre-fetch simply never gets
// used and every crossing downloads a volume that is already on disk.
#include "../src/core/RemoteDocCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REMOTEDOCCACHE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    const QString url = QStringLiteral("https://example.test/a/file.cbz?token=abc");
    const QString p = RemoteDocCache::pathFor(url, QStringLiteral(".cbz"));

    // The name is the SHA1 of the WHOLE url, hex, plus the extension — and the extension is the caller's,
    // because a signed provider link routinely ends in a token and carries no usable one of its own.
    // The oracle is computed here, independently of the code under test.
    const QString sha = QString::fromUtf8(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    CHECK(QFileInfo(p).fileName() == sha + QStringLiteral(".cbz"));
    CHECK(p.startsWith(RemoteDocCache::dir()));
    CHECK(QDir::cleanPath(QFileInfo(p).absolutePath()) == QDir::cleanPath(RemoteDocCache::dir()));

    // Same url, same answer — the property the two callers depend on.
    CHECK(RemoteDocCache::pathFor(url, QStringLiteral(".cbz")) == p);
    // A different url is a different file, and a different extension is a different file.
    CHECK(RemoteDocCache::pathFor(url + QStringLiteral("x"), QStringLiteral(".cbz")) != p);
    CHECK(RemoteDocCache::pathFor(url, QStringLiteral(".epub")) != p);
    // A query string is part of the identity: two signed links to different files differ only there.
    CHECK(RemoteDocCache::pathFor(QStringLiteral("https://example.test/a/file.cbz?token=zzz"),
                                  QStringLiteral(".cbz")) != p);

    // An empty url has no cache identity at all. Hashing "" would give EVERY such call the same name — one
    // cache entry that unrelated documents would overwrite in turn, and each other's pages would be read.
    CHECK(RemoteDocCache::pathFor(QString(), QStringLiteral(".cbz")).isEmpty());

    // The folder is under the app's cache location, which is what makes ChapterOrder::isCachePath refuse to
    // read it as a folder of chapters. These two rules have to keep pointing at the same place.
    CHECK(!RemoteDocCache::dir().isEmpty());
    CHECK(RemoteDocCache::dir().endsWith(QStringLiteral("/remote-docs")));

    if (failures == 0) std::printf("REMOTEDOCCACHE-OK\n");
    return failures == 0 ? 0 : 1;
}
