// THE ONE PREFER-LOCAL RULE (#193, #417, #197) — which file on this disk a server-qualified id plays from.
//
// A download is filed in DownloadsStore under the id the SERVER knows the thing by (a qualified Subsonic
// track id, a qualified Audiobookshelf book id), and every door that opens such an id asks the same question
// first: is there a Downloads entry keyed by exactly this id, whose file is still on disk? If so, that file;
// if not, whatever the caller would have done without a download (a stream url minted now, or the ordinary
// online open) — asked LAZILY, so nothing is minted and no socket is opened for something that is here.
//
// This used to live inside SubsonicDownload (#193, #417), where it was already "the one rule" for a music
// track. Audiobookshelf downloads (#197) need the identical answer for a book, so the rule moved here — out
// from under Subsonic's own gate — rather than being written a second time. SubsonicDownload::localCopy and
// ::preferLocal are now this, behind their "is it a Subsonic TRACK" gate, and the Audiobookshelf open path is
// this behind its "is it a qualified Audiobookshelf id" gate. An EMPTY key never matches anything, which is
// how a caller's gate refuses: a local-library path or an http url must never be looked up as though it were
// a server's id.
//
// Header-only and QtCore-only, so every probe that already links DownloadsStore's struct gets it for free.
#pragma once
#include "DownloadsStore.h"   // DownloadedItem

#include <QFileInfo>
#include <QString>
#include <QVector>
#include <functional>

namespace PreferLocal
{
    // The file on this disk for `key`, or "" — a Downloads entry keyed by exactly `key` whose file still
    // exists. `exists` is a parameter so a probe can drive a deleted file; empty means QFileInfo::exists.
    inline QString localCopy(const QString& key, const QVector<DownloadedItem>& downloads,
                             const std::function<bool(const QString&)>& exists)
    {
        if (key.isEmpty()) return QString();
        for (const DownloadedItem& d : downloads)
            if (d.key == key && !d.path.isEmpty() && (exists ? exists(d.path) : QFileInfo::exists(d.path)))
                return d.path;
        return QString();
    }

    // The local copy when there is one, otherwise `otherwise()` — asked only then.
    inline QString prefer(const QString& key, const QVector<DownloadedItem>& downloads,
                          const std::function<bool(const QString&)>& exists,
                          const std::function<QString()>& otherwise)
    {
        const QString local = localCopy(key, downloads, exists);
        if (!local.isEmpty()) return local;
        return otherwise ? otherwise() : QString();
    }
}
