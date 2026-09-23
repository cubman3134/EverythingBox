// A DOWNLOAD LINK AS IT MAY SIT IN A FILE (issue #437).
//
// downloads/queue.json keeps a plain-link download's url so an interrupted download can resume, and that url
// is whatever the source handed us: a debrid link's token, an add-on stream link with a key in its query.
// The file is an ordinary file in the data folder. Three rules keep that link from outliving its use there,
// and this header is the platform half of them (DownloadManager applies the rest):
//
//   1. NO URL AT ALL, where the source can mint the link again. That is DownloadJob::sourceRef — Jellyfin,
//      Subsonic, Audiobookshelf (#110/#193/#197) and, since #437, every add-on download whose resolve left a
//      #224 re-mint recipe (core/DownloadRecipe.h). Nothing here is involved.
//   2. A LINK THAT CAN STILL RESUME is sealed at rest where the platform has a secret store this app
//      already links against:
//        * Windows — DPAPI (CryptProtectData), CURRENT-USER scope. The stored field is "urlp":
//          "dpapi1:<base64>", readable only by this Windows account on this machine. crypt32 ships with
//          every Windows install; it is not a new dependency.
//        * Linux, Android, macOS — no secret store is linked today, and #437 decides against adding a
//          keychain dependency for this. The url is stored as it is, and the FILE is restricted to its
//          owner (0600). On Android the data folder is already app-private; on a desktop Linux the
//          0600 keeps it from other local accounts. That is weaker than DPAPI and is stated as such.
//   3. A LINK THAT CAN NO LONGER RESUME (the job failed, or was cancelled) loses its query and fragment at
//      once — StoredUrl::location, the one rule for a stored playback location (#200). See DownloadManager.
//
// Header-only and QtCore-only on every platform but Windows, where it links crypt32 through a pragma rather
// than a CMake line: five targets compile DownloadManager.cpp, and a link line in each would be five places
// for the Windows build to break.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "crypt32.lib")
#  endif
#endif

namespace UrlAtRest
{
// The versioned prefix of a sealed value. A future scheme gets "dpapi2:" (or another name) and this reader
// refuses what it does not know rather than guessing.
inline QLatin1String sealedPrefix() { return QLatin1String("dpapi1:"); }

// Whether a resumable link is sealed on this platform (rule 2 above), as opposed to stored as it is in a
// file restricted to its owner.
inline bool sealsAtRest()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
namespace detail
{
// Bound into every seal as DPAPI's optional entropy: a value sealed for this purpose does not open as any
// other DPAPI blob of the same user, and vice versa. Not a secret — DPAPI's key is the user's.
inline DATA_BLOB entropy()
{
    static const char kPurpose[] = "EverythingBox/downloads/queue.json/url/1";
    DATA_BLOB b;
    b.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kPurpose));
    b.cbData = DWORD(sizeof(kPurpose) - 1);
    return b;
}
} // namespace detail
#endif

// "dpapi1:<base64>" for `plain`, or an EMPTY string when it cannot be sealed (not Windows, an empty input, or
// DPAPI refused). An empty answer means "write nothing": the caller never falls back to the plaintext.
inline QString seal(const QString& plain)
{
    if (plain.isEmpty()) return QString();
#ifdef Q_OS_WIN
    const QByteArray bytes = plain.toUtf8();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()));
    in.cbData = DWORD(bytes.size());
    DATA_BLOB ent = detail::entropy();
    DATA_BLOB out{ 0, nullptr };
    // No LOCAL_MACHINE flag: current-user scope. UI_FORBIDDEN: never a prompt from a background save.
    if (!CryptProtectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return QString();
    const QByteArray blob(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return sealedPrefix() + QString::fromLatin1(blob.toBase64());
#else
    return QString();
#endif
}

// The plaintext a seal() produced, or an EMPTY string when it cannot be opened: an unknown prefix, bad base64,
// another user's or another machine's blob, or a platform with no sealing at all.
inline QString unseal(const QString& sealed)
{
    if (!sealed.startsWith(sealedPrefix())) return QString();
#ifdef Q_OS_WIN
    const QByteArray blob = QByteArray::fromBase64(sealed.mid(sealedPrefix().size()).toLatin1(),
                                                   QByteArray::AbortOnBase64DecodingErrors);
    if (blob.isEmpty()) return QString();
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.constData()));
    in.cbData = DWORD(blob.size());
    DATA_BLOB ent = detail::entropy();
    DATA_BLOB out{ 0, nullptr };
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return QString();
    const QString plain = QString::fromUtf8(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
#else
    return QString();
#endif
}

// Rule 2's other half: open `f` to be rewritten so that it is its owner's alone. On POSIX the file is created
// 0600 and an existing one (a queue.json from before #437 was 0644) is brought down to 0600 as well. On
// Windows the protection is the seal, and this is a plain open: Qt's permission mapping there is not an ACL
// this app wants to start writing.
inline bool openRestricted(QFile& f)
{
#ifdef Q_OS_WIN
    return f.open(QIODevice::WriteOnly);
#else
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!f.open(QIODevice::WriteOnly, owner)) return false;
    f.setPermissions(owner);
    return true;
#endif
}
} // namespace UrlAtRest
