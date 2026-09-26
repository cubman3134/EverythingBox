#include "AnnotationExportAction.h"

#include "AnnotationExport.h"
#include "../core/AppPaths.h"
#include "../core/BookmarkStore.h"
#include "../core/HighlightStore.h"
#include "../theme2/HostedReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

static std::function<void(const QString&)> g_notice;

void AnnotationExportAction::setNoticeHook(std::function<void(const QString&)> hook) { g_notice = std::move(hook); }

void AnnotationExportAction::notice(const QString& text)
{
    if (g_notice) g_notice(text);
}

QString AnnotationExportAction::run(HostedReader* reader)
{
    if (!reader) return QString();
    const QString key = reader->itemKey();
    // No key, no annotations: a photo folder (the comic reader in photo mode) has nothing a note could be
    // attached to. Said, rather than writing an "Untitled" file with nothing in it.
    if (key.isEmpty())
    {
        notice(QCoreApplication::translate("AnnotationExport", "There are no notes to export here."));
        return QString();
    }

    AnnotationExport::BookInfo info;
    info.title  = reader->bookTitle();
    info.author = reader->bookAuthor();
    // A pdf or a comic has no title of its own here: it is called what its reader calls it, its file's name. An
    // addon id is not a file name, so it is only used when it looks like one.
    if (info.title.trimmed().isEmpty() && !key.isEmpty())
    {
        const QFileInfo fi(key);
        if (!fi.completeBaseName().isEmpty()) info.title = fi.completeBaseName();
    }
    info.chapterTitles = reader->tocTitles();

    // Every kind: its bookmarks, and its highlights when it has any. A comic or a raster pdf has none (no text
    // layer to make one in), so its export is its bookmarks - the "highlights if they exist, bookmarks
    // otherwise" rule is simply what the stores hold for it.
    const QVector<BookmarkStore::Bookmark>   marks = key.isEmpty() ? QVector<BookmarkStore::Bookmark>()
                                                                   : BookmarkStore::list(key);
    const QVector<HighlightStore::Highlight> hls   = key.isEmpty() ? QVector<HighlightStore::Highlight>()
                                                                   : HighlightStore::list(key);

    const QString content = AnnotationExport::render(info, marks, hls, AnnotationExport::Format::Markdown);
    const QString dir     = AnnotationExport::exportDir(AppPaths::dataDir());
    const QString name    = AnnotationExport::fileNameFor(info.title);
    QString err;
    const QString path = AnnotationExport::writeExport(dir, name, content, &err);
    if (path.isEmpty())
    {
        notice(QCoreApplication::translate("AnnotationExport", "Couldn't export notes: %1").arg(err));
        return QString();
    }
    notice(QCoreApplication::translate("AnnotationExport", "Notes exported to %1")
               .arg(QDir::toNativeSeparators(path)));
    return path;
}
