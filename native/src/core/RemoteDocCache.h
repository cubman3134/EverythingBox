// WHERE A REMOTE DOCUMENT LANDS ON DISK, as a pure function of the url it came from.
//
// A book/comic/ROM fetched from a provider is cached under a SHA1 of that url, so opening the same thing
// twice does not download it twice. The rule used to live inline in the one function that fetched one. It
// has two callers now — the open you asked for, and the pre-fetch of the next volume — and a pre-fetched
// file is only useful if the open looks for it under exactly the same name. Two inline copies of a hashing
// rule are two chances to disagree, and the disagreement is silent: nothing breaks, the pre-fetch is simply
// never found and every crossing re-downloads a volume already sitting on disk.
//
// THE EXTENSION IS THE CALLER'S, not the url's. A signed provider link routinely ends in a token, and the
// reader dispatches on the extension of the path it is handed.
//
// Pure — no I/O and no directory creation (the caller mkpath's, as it always did), so probe_remotedoccache
// tests all of it directly. Note the folder this names sits under the app's cache location, which is
// exactly what ChapterOrder::isCachePath refuses to read as a folder of chapters: the files in here are
// unrelated downloads under url hashes, not a series.
#pragma once
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QString>

namespace RemoteDocCache
{
    inline QString dir()
    {
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               + QStringLiteral("/remote-docs");
    }

    // "" for an empty url: a file with no source has no cache identity, and hashing "" would hand every such
    // call the SAME name — one cache entry that unrelated documents would overwrite in turn.
    inline QString pathFor(const QString& url, const QString& ext)
    {
        if (url.isEmpty()) return QString();
        const QString hash = QString::fromUtf8(
            QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
        return dir() + QStringLiteral("/") + hash + ext;
    }
}
