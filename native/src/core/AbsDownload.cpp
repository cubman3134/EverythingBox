#include "AbsDownload.h"

#include "AbsProgressQueue.h"
#include "PreferLocal.h"   // the one prefer-local rule (#417) — asked, never restated

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace {

// Everything a filesystem (or a person reading a folder) would rather not see. Its own copy for the reason
// SubsonicDownload.cpp and JellyfinDownload.cpp give for theirs: this file is QtCore-only and probe-linked.
QString sanitize(const QString& in, int maxLen = 80)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
    {
        const ushort u = c.unicode();
        if (u < 0x20) continue;
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c == QLatin1Char('*')
            || c == QLatin1Char('?') || c == QLatin1Char('"') || c == QLatin1Char('<') || c == QLatin1Char('>')
            || c == QLatin1Char('|'))
        { out += QLatin1Char('_'); continue; }
        out += c;
    }
    out = out.simplified();
    // A trailing dot or space is legal to WRITE on Windows and then unopenable.
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    return out.left(maxLen);
}

QString two(int n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); }

// The inode at the end of a track's contentUrl ("/api/items/li_x/file/<ino>", perhaps under a base path), for
// the fallback when a reply carries no `audioFiles`. Empty when the url is not that shape — the other shape
// Audiobookshelf has used ("/s/item/...") names a file by its PATH, which the download route cannot take.
QString inoFromContentUrl(const QString& contentUrl)
{
    const int at = contentUrl.lastIndexOf(QStringLiteral("/file/"));
    if (at < 0) return QString();
    QString ino = contentUrl.mid(at + 6);
    const int cut = ino.indexOf(QRegularExpression(QStringLiteral("[/?#]")));
    if (cut >= 0) ino = ino.left(cut);
    return ino;
}

bool usableId(const QString& s)
{
    return !s.isEmpty() && !s.contains(QLatin1Char(':')) && !s.contains(QLatin1Char('#'))
           && !s.contains(QLatin1Char('/')) && !s.contains(QLatin1Char('\\'));
}

// The extension a local copy is written with: the server's own (off the file name or the metadata), else one
// guessed from the mime type, else ".mp3" (mpv opens by content either way; the extension is for the
// listener's file manager). Always WITH its dot.
QString extensionFor(const QString& ext, const QString& mime)
{
    QString e = ext.trimmed();
    if (e.isEmpty())
    {
        if (mime.contains(QLatin1String("mp4")) || mime.contains(QLatin1String("m4b"))) e = QStringLiteral(".m4b");
        else if (mime.contains(QLatin1String("ogg")))  e = QStringLiteral(".ogg");
        else if (mime.contains(QLatin1String("flac"))) e = QStringLiteral(".flac");
        else if (mime.contains(QLatin1String("wav")))  e = QStringLiteral(".wav");
        else e = QStringLiteral(".mp3");
    }
    e = sanitize(e, 10);
    if (!e.startsWith(QLatin1Char('.'))) e.prepend(QLatin1Char('.'));
    return e;
}

bool strictlyInside(const QString& path, const QString& dir)
{
    const QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    if (d.isEmpty() || p.size() <= d.size() + 1) return false;
    return p.startsWith(d + QLatin1Char('/'), Qt::CaseInsensitive);
}

} // namespace

// ---- The file ref ------------------------------------------------------------------------------------

QString AbsDownload::fileRef(const QString& qualifiedBookId, const QString& ino)
{
    const Abs::Ref r = Abs::parse(qualifiedBookId);
    if (!r.ok || r.isEpisode() || !usableId(ino)) return QString();
    return QString::fromLatin1(kFileScheme) + r.serverId + QLatin1Char(':') + r.itemId + QLatin1Char(':') + ino;
}

AbsDownload::FileRef AbsDownload::parseFileRef(const QString& ref)
{
    FileRef out;
    const QString prefix = QString::fromLatin1(kFileScheme);
    if (!ref.startsWith(prefix)) return out;
    const QStringList parts = ref.mid(prefix.size()).split(QLatin1Char(':'));
    if (parts.size() != 3) return out;
    const QString book = Abs::qualify(parts.at(0), parts.at(1));
    if (book.isEmpty() || !usableId(parts.at(2))) return out;
    out.ok = true;
    out.qualifiedBookId = book;
    out.itemId = parts.at(1);
    out.ino = parts.at(2);
    return out;
}

// ---- The plan ----------------------------------------------------------------------------------------

AbsDownload::Plan AbsDownload::planFor(const QString& qualifiedId, const Abs::ItemDetail& d)
{
    Plan p;
    const Abs::Ref ref = Abs::parse(qualifiedId);
    if (!ref.ok || ref.isEpisode() || !d.ok || d.item.isPodcast) return p;
    p.qualifiedId = qualifiedId;
    p.title    = d.item.title.trimmed().isEmpty() ? QStringLiteral("Audiobook") : d.item.title.trimmed();
    p.author   = d.item.author;
    p.narrator = d.item.narrator;
    p.chapters = d.chapters;

    // THE FILE LIST — the server's audioFiles, already in `index` order with the excluded ones dropped
    // (Abs::readItem). Every file, in order: a book with part four missing is not a book.
    struct Src { QString ino, name, ext, mime; double duration; };
    QVector<Src> src;
    for (const Abs::AudioFile& f : d.files)
        src.push_back({ f.ino, f.fileName, f.ext, f.mimeType, f.duration });
    if (src.isEmpty())
    {
        // No file list in the reply: the tracks, whose contentUrl ends in the same inode.
        for (const Abs::Track& t : d.tracks)
        {
            const QString ino = inoFromContentUrl(t.contentUrl);
            if (ino.isEmpty()) { src.clear(); break; }   // one unnameable file makes the whole plan unusable
            src.push_back({ ino, t.title, QString(), t.mimeType, t.duration });
        }
    }
    if (src.isEmpty()) return p;

    QSet<QString> usedNames;
    double at = 0.0;
    for (int i = 0; i < src.size(); ++i)
    {
        const Src& s = src.at(i);
        if (!usableId(s.ino)) return Plan{};
        PlannedFile f;
        f.ino = s.ino;
        f.title = s.name.trimmed().isEmpty() ? QStringLiteral("Part %1").arg(i + 1) : s.name.trimmed();
        // "NN - name.ext": the ordinal FIRST, so the folder sorts in the book's order in any file manager, and
        // unique within the folder by construction (two files the server calls the same get two ordinals).
        // The server's extension where its file name carries one (".mp3"), else the metadata's; the stem is
        // the name without it, so "01 - One.mp3" is not written as "01 - One.mp3.mp3".
        const QString suffix = QFileInfo(f.title).suffix();
        const bool nameHasExt = !suffix.isEmpty() && suffix.size() <= 5 && !suffix.contains(QLatin1Char(' '));
        QString stem = sanitize(nameHasExt ? QFileInfo(f.title).completeBaseName() : f.title, 70);
        if (stem.isEmpty()) stem = QStringLiteral("Part %1").arg(i + 1);
        // A server name that already LEADS with this ordinal ("01 - Low") keeps it as it is, rather than
        // becoming "01 - 01 - Low": the order is the same and the name is the one the listener recognises.
        const QString ordinal = two(i + 1);
        const bool leadsWithIt = stem.startsWith(ordinal)
                                 && (stem.size() == ordinal.size() || !stem.at(ordinal.size()).isDigit());
        QString name = (leadsWithIt ? stem : ordinal + QStringLiteral(" - ") + stem)
                       + extensionFor(nameHasExt ? QLatin1Char('.') + suffix : s.ext, s.mime);
        while (usedNames.contains(name.toLower())) name.prepend(QLatin1Char('_'));
        usedNames.insert(name.toLower());
        f.localName = name;
        f.duration = s.duration > 0.0 ? s.duration : 0.0;
        f.startOffset = at;
        at += f.duration;
        p.files.push_back(f);
    }
    p.duration = d.item.duration > 0.0 ? d.item.duration : at;

    const QString server8 = sanitize(ref.serverId, 8);
    p.folderName = sanitize(p.title, 60) + QStringLiteral(" [abs-") + server8 + QLatin1Char('-')
                   + sanitize(ref.itemId, 40) + QLatin1Char(']');
    p.ok = true;
    return p;
}

QString AbsDownload::folderFor(const Plan& plan, const QString& downloadsDir)
{
    if (!plan.ok || downloadsDir.isEmpty()) return QString();
    return downloadsDir + QStringLiteral("/audiobooks/") + plan.folderName;
}

QVector<DownloadJob> AbsDownload::jobsFor(const Plan& plan, const QString& downloadsDir, const QString& thumb)
{
    QVector<DownloadJob> out;
    const QString folder = folderFor(plan, downloadsDir);
    if (folder.isEmpty()) return out;
    const int n = plan.files.size();
    for (int i = 0; i < n; ++i)
    {
        const PlannedFile& f = plan.files.at(i);
        DownloadJob j;
        j.title = QStringLiteral("%1 (%2/%3)").arg(plan.title).arg(i + 1).arg(n);
        // NO URL. The link is minted per request, inside DownloadManager::start(), from this ref.
        j.sourceRef = fileRef(plan.qualifiedId, f.ino);
        if (j.sourceRef.isEmpty()) return {};
        j.key = j.sourceRef;
        j.dest = folder + QLatin1Char('/') + f.localName;
        j.kind = QStringLiteral("audio");
        j.thumb = thumb;
        // AN INTERMEDIATE: a file is a means, the BOOK is what was asked for — recorded once, by completedBook.
        j.record = false;
        out.push_back(j);
    }
    return out;
}

// ---- The manifest ------------------------------------------------------------------------------------

QByteArray AbsDownload::manifestJson(const Plan& plan, const QString& coverFile)
{
    QJsonArray files;
    for (const PlannedFile& f : plan.files)
        files.append(QJsonObject{ { QStringLiteral("ino"), f.ino }, { QStringLiteral("title"), f.title },
                                  { QStringLiteral("file"), f.localName },
                                  { QStringLiteral("duration"), f.duration },
                                  { QStringLiteral("offset"), f.startOffset } });
    QJsonArray chapters;
    for (const Abs::Chapter& c : plan.chapters)
        chapters.append(QJsonObject{ { QStringLiteral("start"), c.start }, { QStringLiteral("end"), c.end },
                                     { QStringLiteral("title"), c.title } });
    const QJsonObject o{
        { QStringLiteral("v"), 1 },
        { QStringLiteral("id"), plan.qualifiedId },
        { QStringLiteral("title"), plan.title },
        { QStringLiteral("author"), plan.author },
        { QStringLiteral("narrator"), plan.narrator },
        { QStringLiteral("duration"), plan.duration },
        { QStringLiteral("cover"), coverFile },
        { QStringLiteral("chapters"), chapters },
        { QStringLiteral("files"), files } };
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

AbsDownload::Manifest AbsDownload::readManifest(const QString& manifestPath)
{
    Manifest m;
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly)) return m;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    Plan& p = m.plan;
    p.qualifiedId = o.value(QStringLiteral("id")).toString();
    if (!Abs::isQualified(p.qualifiedId)) return m;
    p.title    = o.value(QStringLiteral("title")).toString();
    p.author   = o.value(QStringLiteral("author")).toString();
    p.narrator = o.value(QStringLiteral("narrator")).toString();
    p.duration = o.value(QStringLiteral("duration")).toDouble();
    for (const QJsonValue& v : o.value(QStringLiteral("chapters")).toArray())
    {
        const QJsonObject c = v.toObject();
        p.chapters.push_back({ c.value(QStringLiteral("start")).toDouble(), c.value(QStringLiteral("end")).toDouble(),
                               c.value(QStringLiteral("title")).toString() });
    }
    for (const QJsonValue& v : o.value(QStringLiteral("files")).toArray())
    {
        const QJsonObject c = v.toObject();
        PlannedFile pf;
        pf.ino = c.value(QStringLiteral("ino")).toString();
        pf.title = c.value(QStringLiteral("title")).toString();
        pf.localName = c.value(QStringLiteral("file")).toString();
        pf.duration = c.value(QStringLiteral("duration")).toDouble();
        pf.startOffset = c.value(QStringLiteral("offset")).toDouble();
        // A name that escapes the folder is not a file of this book, whoever wrote it.
        if (pf.localName.isEmpty() || pf.localName.contains(QLatin1Char('/'))
            || pf.localName.contains(QLatin1Char('\\')) || pf.localName.contains(QStringLiteral("..")))
            return Manifest{};
        p.files.push_back(pf);
    }
    if (p.files.isEmpty()) return Manifest{};
    m.folder = QFileInfo(manifestPath).absolutePath();
    p.folderName = QFileInfo(m.folder).fileName();
    const QString cover = o.value(QStringLiteral("cover")).toString();
    if (!cover.isEmpty() && !cover.contains(QLatin1Char('/')) && !cover.contains(QLatin1Char('\\')))
        m.coverFile = m.folder + QLatin1Char('/') + cover;
    p.ok = true;
    m.ok = true;
    return m;
}

bool AbsDownload::isComplete(const Manifest& m, const std::function<bool(const QString&)>& exists)
{
    if (!m.ok) return false;
    for (const PlannedFile& f : m.plan.files)
    {
        const QString path = m.folder + QLatin1Char('/') + f.localName;
        if (!(exists ? exists(path) : QFileInfo::exists(path))) return false;
    }
    return true;
}

DownloadedItem AbsDownload::recordFor(const Manifest& m, const QString& manifestPath)
{
    DownloadedItem d;
    d.path  = QFileInfo(manifestPath).absoluteFilePath();
    d.title = m.plan.title;
    d.kind  = QStringLiteral("audio");
    d.thumb = m.coverFile;
    d.key   = m.plan.qualifiedId;
    return d;
}

bool AbsDownload::completedBook(const DownloadJob& finished, DownloadedItem* out,
                                const std::function<bool(const QString&)>& exists)
{
    const FileRef ref = parseFileRef(finished.sourceRef);
    if (!ref.ok || finished.dest.isEmpty()) return false;
    const QString manifestPath = QFileInfo(finished.dest).absolutePath() + QLatin1Char('/') + manifestName();
    const Manifest m = readManifest(manifestPath);
    // The manifest has to name THIS book — a stray file dropped into another book's folder completes nothing.
    if (!m.ok || m.plan.qualifiedId != ref.qualifiedBookId) return false;
    if (!isComplete(m, exists)) return false;
    if (out) *out = recordFor(m, manifestPath);
    return true;
}

// ---- Playing it --------------------------------------------------------------------------------------

QString AbsDownload::localManifest(const QString& qualifiedId, const QVector<DownloadedItem>& downloads,
                                   const std::function<bool(const QString&)>& exists)
{
    // THE ONE RULE, behind this family's gate: only a qualified Audiobookshelf id is ever looked up.
    return PreferLocal::localCopy(Abs::isQualified(qualifiedId) ? qualifiedId : QString(), downloads, exists);
}

Abs::Session AbsDownload::localSession(const Manifest& m)
{
    Abs::Session s;
    if (!m.ok) return s;
    s.id = QStringLiteral("local");
    s.title = m.plan.title;
    s.duration = m.plan.duration;
    s.chapters = m.plan.chapters;
    int i = 0;
    for (const PlannedFile& f : m.plan.files)
    {
        Abs::Track t;
        t.index = ++i;
        t.title = f.title;
        t.contentUrl = m.folder + QLatin1Char('/') + f.localName;   // A FILE, not a route: nothing to sign
        t.startOffset = f.startOffset;
        t.duration = f.duration;
        s.tracks.push_back(t);
    }
    s.ok = !s.tracks.isEmpty();
    return s;
}

// ---- Removing it -------------------------------------------------------------------------------------

AbsDownload::Removal AbsDownload::removeBook(const QString& qualifiedId, const QString& manifestPath,
                                             const QString& downloadsDir)
{
    const QString folder = QFileInfo(manifestPath).absolutePath();
    // ONLY WHAT THIS FEATURE PUT ON DISK, decided before the filesystem is asked anything: a hand-edited store
    // row pointing at somebody's own library must not be deleted by a Remove pressed on a server book.
    if (!strictlyInside(folder, downloadsDir)) return Removal::RefusedOutsideDownloads;
    Removal out = Removal::Removed;
    QDir dir(folder);
    if (!dir.exists()) out = Removal::AlreadyGone;
    else if (!dir.removeRecursively()) return Removal::DeleteFailed;   // the entry stays: the files are still here
    // THE FILES ARE GONE, so everything that named them goes too: the Downloaded row, and the position this
    // device was holding for the book. The server's copy and the server's own position are not touched.
    DownloadsStore::remove(qualifiedId);
    AbsProgressQueue::remove(qualifiedId);
    return out;
}
