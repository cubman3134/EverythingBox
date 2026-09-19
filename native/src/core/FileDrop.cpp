#include "FileDrop.h"

#include "LibraryBundle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <io.h>
#  include <windows.h>
#else
#  include <cerrno>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace FileDrop
{
    namespace
    {
        QString hashHex(const QByteArray& material)
        {
            return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(32));
        }

        Answer refuse(int http, const char* code, const QString& reason)
        {
            Answer a;
            a.http = http;
            a.code = QString::fromLatin1(code);
            a.reason = reason;
            return a;
        }

        qint64 storageFree(const QString& dir)
        {
            QStorageInfo si(dir);
            if (!si.isValid()) return -1;
            return si.bytesAvailable();
        }

        // A part file is adopted only when it is a regular file of its own -- never a link that would make the
        // appends land somewhere else.
        bool plainFile(const QString& path)
        {
            const QFileInfo fi(path);
            return fi.exists() && fi.isFile() && !fi.isSymLink();
        }

        // Whether ANY directory entry sits at `path`, a dangling link included (QFileInfo::exists follows
        // links and would call a dangling one absent).
        bool entryExists(const QString& path)
        {
            const QFileInfo fi(path);
            return fi.exists() || fi.isSymLink();
        }
    }

    // ---------------------------------------------------------------- destinations ---------------------------

    QString destinationId(const QString& kind, const QString& key, const QString& dir)
    {
        return hashHex(QByteArray("eb-drop-dest/1\n") + kind.toUtf8() + '\n' + key.toUtf8() + '\n'
                       + QDir::cleanPath(dir).toUtf8());
    }

    QList<Destination> buildDestinations(const QList<Root>& roots)
    {
        QList<Destination> out;
        QSet<QString> seen;
        for (const Root& r : roots)
        {
            if (r.dir.trimmed().isEmpty() || r.kind.isEmpty()) continue;
            const QString dir = QDir::cleanPath(QFileInfo(r.dir).absoluteFilePath());
            if (!QFileInfo(dir).isDir()) continue;
            Destination d;
            d.id = destinationId(r.kind, r.key, dir);
            if (seen.contains(d.id)) continue;
            seen.insert(d.id);
            d.kind = r.kind;
            d.key = r.key;
            d.label = r.label.isEmpty() ? r.key : r.label;
            d.dir = dir;
            out << d;
        }
        return out;
    }

    QByteArray destinationsJson(const QList<Destination>& dests)
    {
        QJsonArray arr;
        for (const Destination& d : dests)
        {
            QJsonObject o;
            o.insert(QStringLiteral("id"), d.id);
            o.insert(QStringLiteral("label"), d.label);
            o.insert(QStringLiteral("kind"), d.kind);
            arr.append(o);
        }
        QJsonObject root;
        root.insert(QStringLiteral("ok"), true);
        root.insert(QStringLiteral("destinations"), arr);
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    const Destination* findDestination(const QList<Destination>& dests, const QString& id)
    {
        if (id.isEmpty()) return nullptr;
        for (const Destination& d : dests)
            if (d.id == id) return &d;
        return nullptr;
    }

    // ---------------------------------------------------------------- names ----------------------------------

    bool safeName(const QString& name)
    {
        return LibraryBundle::safePathSegment(name);
    }

    QString uploadIdFor(const QString& destId, const QString& name, qint64 size)
    {
        return hashHex(QByteArray("eb-drop-upload/1\n") + destId.toUtf8() + '\n' + name.toUtf8() + '\n'
                       + QByteArray::number(size));
    }

    bool validUploadId(const QString& id)
    {
        static const QRegularExpression re(QStringLiteral("^[0-9a-f]{32}$"));
        return re.match(id).hasMatch();
    }

    QString partFileName(const QString& uploadId)
    {
        return QLatin1String(kPartPrefix) + uploadId + QLatin1String(kPartSuffix);
    }

    bool isPartFileName(const QString& fileName)
    {
        static const QRegularExpression re(QStringLiteral("^\\.eb-drop-[0-9a-f]{32}\\.part$"));
        return re.match(fileName).hasMatch();
    }

    bool shouldSweep(const QString& fileName, const QDateTime& lastModified, const QDateTime& now)
    {
        if (!isPartFileName(fileName)) return false;
        if (!lastModified.isValid() || !now.isValid()) return false;
        return lastModified.secsTo(now) >= kStaleAfterSecs;
    }

    // ---------------------------------------------------------------- landing primitives ---------------------

    bool syncFile(QFile& f)
    {
        if (!f.isOpen()) return false;
        if (!f.flush()) return false;
#ifdef Q_OS_WIN
        const intptr_t h = _get_osfhandle(f.handle());
        if (h == -1) return false;
        return FlushFileBuffers(reinterpret_cast<HANDLE>(h)) != 0;
#else
        return ::fsync(f.handle()) == 0;
#endif
    }

    RenameResult renameNoReplace(const QString& from, const QString& to)
    {
        if (entryExists(to)) return RenameResult::Exists;
#ifdef Q_OS_WIN
        // No MOVEFILE_REPLACE_EXISTING: an entry at `to` -- file, directory or link -- fails the move.
        if (MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(from).utf16()),
                        reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(to).utf16()),
                        MOVEFILE_WRITE_THROUGH))
            return RenameResult::Renamed;
        const DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) return RenameResult::Exists;
        return RenameResult::Failed;
#else
        const QByteArray f = QFile::encodeName(from);
        const QByteArray t = QFile::encodeName(to);
        // link() refuses an existing entry (EEXIST), a dangling symlink included, with no window between the
        // check and the act. Filesystems without hard links (FAT, exFAT) say so, and only then do we fall back
        // to rename() after an lstat -- the one place a race window remains, on those filesystems alone.
        if (::link(f.constData(), t.constData()) == 0)
        {
            ::unlink(f.constData());
            return RenameResult::Renamed;
        }
        if (errno == EEXIST) return RenameResult::Exists;
        struct stat st;
        if (::lstat(t.constData(), &st) == 0) return RenameResult::Exists;
        if (::rename(f.constData(), t.constData()) == 0) return RenameResult::Renamed;
        return RenameResult::Failed;
#endif
    }

    // ---------------------------------------------------------------- answers --------------------------------

    QByteArray answerJson(const Answer& a)
    {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), a.ok());
        o.insert(QStringLiteral("code"), a.code);
        if (!a.reason.isEmpty())   o.insert(QStringLiteral("reason"), a.reason);
        if (!a.uploadId.isEmpty()) o.insert(QStringLiteral("uploadId"), a.uploadId);
        if (!a.name.isEmpty())     o.insert(QStringLiteral("name"), a.name);
        if (a.size >= 0)           o.insert(QStringLiteral("size"), double(a.size));
        if (a.received >= 0)       o.insert(QStringLiteral("received"), double(a.received));
        if (a.landed)              o.insert(QStringLiteral("landed"), true);
        return QJsonDocument(o).toJson(QJsonDocument::Compact);
    }

    // ---------------------------------------------------------------- uploads --------------------------------

    struct Uploads::Upload
    {
        QString id;
        QString destId;
        QString dir;
        QString name;
        qint64  size = 0;
        qint64  received = 0;
        QFile*  chunk = nullptr;          // open while a chunk is in flight
        qint64  chunkRemaining = 0;
        QString partPath() const { return dir + QLatin1Char('/') + partFileName(id); }
    };

    Uploads::Uploads(std::function<qint64(const QString&)> freeBytes) : freeBytes_(std::move(freeBytes))
    {
        if (!freeBytes_) freeBytes_ = storageFree;
    }

    Uploads::~Uploads()
    {
        for (const auto& u : uploads_)
            if (u->chunk) { u->chunk->flush(); u->chunk->close(); delete u->chunk; u->chunk = nullptr; }
    }

    qint64 Uploads::pendingBytesExcept(const QString& id) const
    {
        qint64 sum = 0;
        for (auto it = uploads_.cbegin(); it != uploads_.cend(); ++it)
            if (it.key() != id) sum += qMax<qint64>(0, it.value()->size - it.value()->received);
        return sum;
    }

    QString Uploads::partPathFor(const QString& id) const
    {
        const auto u = uploads_.value(id);
        return u ? u->partPath() : QString();
    }

    QSet<QString> Uploads::busyIds() const
    {
        QSet<QString> s;
        for (auto it = uploads_.cbegin(); it != uploads_.cend(); ++it)
            if (it.value()->chunk) s.insert(it.key());
        return s;
    }

    Answer Uploads::start(const QList<Destination>& dests, const QString& destId, const QString& name, qint64 size)
    {
        const Destination* d = findDestination(dests, destId);
        if (!d) return refuse(400, "nodest", QStringLiteral("that destination is not offered by this device"));
        if (!safeName(name))
            return refuse(400, "badname", QStringLiteral("that file name cannot be used on this device"));
        if (size < 0) return refuse(400, "badrequest", QStringLiteral("the file size is missing"));
        if (size > kMaxFileBytes)
            return refuse(413, "toolarge", QStringLiteral("that file is larger than the 64 GiB file drop limit"));

        const QString dir = d->dir;
        const QString target = dir + QLatin1Char('/') + name;
        if (entryExists(target))
        {
            Answer a = refuse(409, "exists", QStringLiteral("a file with that name is already there"));
            a.name = name;
            return a;
        }

        const QString id = uploadIdFor(d->id, name, size);
        std::shared_ptr<Upload> u = uploads_.value(id);
        if (!u)
        {
            if (uploads_.size() >= kMaxUploads)
                return refuse(503, "toomany", QStringLiteral("too many uploads are in progress; finish some first"));
            u = std::make_shared<Upload>();
            u->id = id;
            u->destId = d->id;
            u->dir = dir;
            u->name = name;
            u->size = size;
            const QString part = u->partPath();
            // An app restart forgets the registry, not the disk: a part file of this exact triple is picked up
            // where it stopped. One larger than the declared size cannot be this upload and is started over.
            if (entryExists(part))
            {
                if (!plainFile(part))
                    return refuse(500, "writefailed", QStringLiteral("this device could not reserve space for that file"));
                const qint64 have = QFileInfo(part).size();
                if (have <= size) u->received = have;
                else
                {
                    QFile::remove(part);
                    u->received = 0;
                }
            }
        }
        else if (!plainFile(u->partPath()))
        {
            // The part file went (a sweep, a user) while the registry still held it: start that triple over.
            if (u->chunk) return refuse(409, "busy", QStringLiteral("a piece of that file is still arriving"));
            u->received = 0;
        }

        // Free space: this file's outstanding bytes, plus every other upload's, must leave the reserve free.
        const qint64 need = (size - u->received) + pendingBytesExcept(id);
        const qint64 avail = freeBytes_(dir);
        if (avail < 0 || need > avail - kFreeSpaceReserve)
        {
            Answer a = refuse(507, "nospace", QStringLiteral("there is not enough free space on this device for that file"));
            a.name = name;
            a.size = size;
            return a;
        }

        if (!entryExists(u->partPath()))
        {
            QFile f(u->partPath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return refuse(500, "writefailed", QStringLiteral("this device could not reserve space for that file"));
            f.close();
            u->received = 0;
        }
        uploads_.insert(id, u);

        Answer a;
        a.uploadId = id;
        a.name = name;
        a.size = size;
        a.received = u->received;
        return a;
    }

    Answer Uploads::status(const QString& id) const
    {
        const auto u = uploads_.value(id);
        if (!u) return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        Answer a;
        a.uploadId = id;
        a.name = u->name;
        a.size = u->size;
        a.received = u->chunk ? u->received : (plainFile(u->partPath()) ? QFileInfo(u->partPath()).size() : 0);
        return a;
    }

    Answer Uploads::beginChunk(const QString& id, qint64 offset, qint64 length)
    {
        const auto u = uploads_.value(id);
        if (!u) return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        if (u->chunk) return refuse(409, "busy", QStringLiteral("a piece of that file is still arriving"));
        if (!plainFile(u->partPath()))
            return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        // What is on disk is the truth; the registry follows it.
        u->received = QFileInfo(u->partPath()).size();

        auto withReceived = [&](Answer a) { a.uploadId = id; a.size = u->size; a.received = u->received; return a; };
        if (length <= 0) return withReceived(refuse(400, "badrequest", QStringLiteral("an empty piece")));
        if (length > kMaxChunkBytes)
            return withReceived(refuse(413, "toolarge", QStringLiteral("that piece is larger than 8 MiB")));
        if (offset != u->received)
            return withReceived(refuse(409, "offset", QStringLiteral("that piece does not continue where the file stopped")));
        if (offset + length > u->size)
            return withReceived(refuse(400, "badrequest", QStringLiteral("that piece runs past the end of the file")));

        auto* f = new QFile(u->partPath());
        if (!f->open(QIODevice::WriteOnly | QIODevice::Append))
        {
            delete f;
            return withReceived(refuse(500, "writefailed", QStringLiteral("this device could not write that file")));
        }
        u->chunk = f;
        u->chunkRemaining = length;
        return withReceived(Answer());
    }

    bool Uploads::writeChunk(const QString& id, const char* data, qint64 n)
    {
        const auto u = uploads_.value(id);
        if (!u || !u->chunk || n < 0 || n > u->chunkRemaining) return false;
        if (u->chunk->write(data, n) != n) return false;
        u->chunkRemaining -= n;
        u->received += n;
        return true;
    }

    Answer Uploads::endChunk(const QString& id)
    {
        const auto u = uploads_.value(id);
        if (!u) return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        bool ok = true;
        if (u->chunk)
        {
            ok = u->chunk->flush();
            u->chunk->close();
            delete u->chunk;
            u->chunk = nullptr;
        }
        u->received = plainFile(u->partPath()) ? QFileInfo(u->partPath()).size() : 0;
        Answer a = ok && u->chunkRemaining == 0
                       ? Answer()
                       : refuse(500, "writefailed", QStringLiteral("this device could not write that piece"));
        u->chunkRemaining = 0;
        a.uploadId = id;
        a.size = u->size;
        a.received = u->received;
        return a;
    }

    void Uploads::abortChunk(const QString& id)
    {
        const auto u = uploads_.value(id);
        if (!u || !u->chunk) return;
        u->chunk->flush();
        u->chunk->close();
        delete u->chunk;
        u->chunk = nullptr;
        u->chunkRemaining = 0;
        u->received = plainFile(u->partPath()) ? QFileInfo(u->partPath()).size() : 0;
    }

    Answer Uploads::finish(const QString& id, const QString& newName)
    {
        const auto u = uploads_.value(id);
        if (!u) return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        if (u->chunk) return refuse(409, "busy", QStringLiteral("a piece of that file is still arriving"));
        const QString name = newName.isEmpty() ? u->name : newName;
        auto base = [&](Answer a) { a.uploadId = id; a.name = name; a.size = u->size; a.received = u->received; return a; };
        if (!safeName(name))
            return base(refuse(400, "badname", QStringLiteral("that file name cannot be used on this device")));
        if (!plainFile(u->partPath()))
        {
            uploads_.remove(id);
            return refuse(404, "unknown", QStringLiteral("that upload is not in progress; start it again"));
        }
        u->received = QFileInfo(u->partPath()).size();
        if (u->received != u->size)
            return base(refuse(409, "incomplete", QStringLiteral("not all of that file has arrived yet")));

        // DURABLE FIRST: the bytes are on the disk before the name that makes them visible is.
        {
            QFile f(u->partPath());
            if (!f.open(QIODevice::ReadWrite) || !syncFile(f))
                return base(refuse(500, "writefailed", QStringLiteral("this device could not write that file")));
            f.close();
        }
        const QString target = u->dir + QLatin1Char('/') + name;
        switch (renameNoReplace(u->partPath(), target))
        {
            case RenameResult::Exists:
                // The part file stays: the page offers a rename and a retry, which costs no re-upload.
                return base(refuse(409, "exists", QStringLiteral("a file with that name is already there")));
            case RenameResult::Failed:
                return base(refuse(500, "writefailed", QStringLiteral("this device could not put that file in place")));
            case RenameResult::Renamed:
                break;
        }
        Answer a = base(Answer());
        a.landed = true;
        a.landedPath = target;
        a.destinationId = u->destId;
        uploads_.remove(id);
        return a;
    }

    int sweepDirs(const QStringList& dirs, const QDateTime& now, const QSet<QString>& skipIds)
    {
        int removed = 0;
        QSet<QString> done;
        for (const QString& dir : dirs)
        {
            const QString clean = QDir::cleanPath(dir);
            if (done.contains(clean)) continue;
            done.insert(clean);
            QDir d(clean);
            if (!d.exists()) continue;
            // Name-filtered and hidden-inclusive (a dot file is hidden on Unix); never recursive.
            const QFileInfoList parts = d.entryInfoList({ QStringLiteral(".eb-drop-*.part") },
                                                        QDir::Files | QDir::Hidden | QDir::System);
            for (const QFileInfo& fi : parts)
            {
                if (fi.isSymLink()) continue;
                const QString name = fi.fileName();
                if (!shouldSweep(name, fi.lastModified(), now)) continue;
                const QString id = name.mid(int(qstrlen(kPartPrefix)), 32);
                if (skipIds.contains(id)) continue;
                if (QFile::remove(fi.absoluteFilePath())) ++removed;
            }
        }
        return removed;
    }

    int Uploads::sweep(const QStringList& dirs, const QDateTime& now)
    {
        const int n = sweepDirs(dirs, now, busyIds());
        // Forget anything whose part file is gone, so a later chunk is told to start again.
        for (auto it = uploads_.begin(); it != uploads_.end();)
        {
            if (!it.value()->chunk && !plainFile(it.value()->partPath())) it = uploads_.erase(it);
            else ++it;
        }
        return n;
    }

    QByteArray pageHtml()
    {
        QFile f(QString::fromLatin1(kPageResource));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }
}
