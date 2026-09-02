// DecorationInstall — the ONE impure step of a decoration pack install: a downloaded zip on disk becomes
// folders under <data>/bezels/<system>/<packId>/ (issue #187).
//
// Split from DecorationPack for the reason every unit in src/core is split that way: DecorationPack is
// QtCore-only, so probe_themereg can pin the index parse and probe_bezel's neighbours can pin the layout
// without linking an archiver. This file is where miniz and ArchiveRom come in, and it holds no decisions of
// its own — the digest rule, the layout rule and the wrapper-folder strip all live in DecorationPack, and
// the traversal rule lives in ArchiveSafePath, which is the same guard ArchiveRom::extractAll already gates
// every ROM archive on. There is deliberately no second unzipper in this product.
//
// Order of operations, and why: the zip is HASHED first, then LISTED from its central directory and planned,
// and only then extracted. Every refusal a hostile or broken pack can trigger therefore happens before a
// single byte is written into the user's bezels folder.
#pragma once
#include "DecorationPack.h"

#include <QString>
#include <QStringList>

namespace DecorationInstall {

// What an install did. `ignored` is the top-level names the pack carried that are not systems this build
// knows about — reported so the caller can log the one line the issue asks for, never a failure.
struct Result {
    QStringList systems;   // the systems the pack landed for, sorted
    QStringList ignored;
    int         files = 0;
};

// Install `zipPath` as `entry` under `root` (DecorationPack::bezelsRoot(dataDir)).
//
// Refuses, without having written anything into `root`, when: the entry is not installable at all; the file
// is not a readable zip; its SHA-256 does not match `entry.sha256`; the archive is too large uncompressed or
// holds too many members; any member would escape its destination; or the pack carries nothing for any
// system it is allowed to land for.
//
// ALLOWED = `knownSystems` ∩ `entry.systems`. Both halves matter. A system the app does not emulate has no
// bezels directory worth writing; and a system the ENTRY did not declare is one the user was not shown on the
// card they pressed, so a zip that quietly carries a fourth console's art does not get to install it. Either
// way the folder is reported in Result::ignored rather than refusing the pack — one extra console is not a
// reason to withhold the ones the pack was published for.
//
// A pack already installed is REPLACED, per system: each destination folder is swapped by a single directory
// rename out of a staging area, so a half-written pack is never what the renderer finds. The staging area is
// dot-prefixed inside `root`, which is what makes that rename a same-volume move (and what keeps it out of
// installedPacks()/packsForSystem(), both of which skip dot-names).
bool installZip(const QString& zipPath, const QString& root, const DecorationPack::Entry& entry,
                const QStringList& knownSystems, Result* out, QString* error);

// The same install from BYTES that are already in hand — what both download surfaces actually have. This
// exists rather than each surface spelling out its own "write to a temp file, then call installZip":
//
//   the obvious spelling of that, QTemporaryFile, DOES NOT WORK on Windows and fails in a way no probe would
//   have caught. QTemporaryFileEngine::close() deliberately does not close the OS handle (it seeks to 0 and
//   keeps the name reserved), so the file's size is still cached against the writer's handle. A second opener
//   — miniz's fopen + fseek(END) + ftell, which is how every zip reader finds the central directory — sees a
//   ZERO-length file and reports "not a readable zip", even though QFile::exists(), QFileInfo::size() and a
//   plain fopen() on the same path all say the bytes are there. Measured on Qt 6.8.3 / Windows 11 while
//   driving the live UI: the download arrived, the SHA-256 matched the file, and the zip reader then refused
//   it. A plain QFile inside a QTemporaryDir does not have the problem, because a plain QFile really closes.
//
// So: one function, one temp scheme, both surfaces. Anything a future caller needs is here rather than
// re-invented at a call site that cannot know the above.
bool installBytes(const QByteArray& zipBytes, const QString& root, const DecorationPack::Entry& entry,
                  const QStringList& knownSystems, Result* out, QString* error);

// The member names inside a zip, exactly as its central directory spells them, plus the total size they
// would occupy once decompressed. Exposed because installZip's caller has no other way to say WHY a pack was
// refused before download, and because it is the seam probe_decopack drives the size caps through.
// Returns false with a reason when the file is not a readable zip.
bool listMembers(const QString& zipPath, QStringList* members, qint64* uncompressedBytes, QString* error);

} // namespace DecorationInstall
