// AnnotationExportAction — the reader's "Export notes" verb (issue #136), shared by every reader kind and both
// layouts: the classic bars of the book, pdf and comic readers, and the themed chrome's top row, all call
// run() on the HostedReader in front of them. The FORMAT is AnnotationExport.h (pure, probe-pinned); this is
// the thin impure half — read the stores, write the file, say where it went.
//
// Where it goes: <data dir>/exports/<title> — notes.md (AnnotationExport::exportDir / fileNameFor), replacing a
// previous export of the same book. No save dialog: the folder is fixed and visible, and the toast names the
// file. The toast rides a notice hook MainWindow registers once (the stores' change-hook pattern), so no
// reader has to know the window exists.
#pragma once
#include <QString>
#include <functional>

class HostedReader;

namespace AnnotationExportAction
{
    // MainWindow registers its toast here once. Unset (a probe, a reader built with no window) = silent.
    void setNoticeHook(std::function<void(const QString&)> hook);
    // Say something through that hook. Also used by the note prompt to refuse an over-long note.
    void notice(const QString& text);

    // Export `reader`'s bookmarks and highlights (with their notes) as Markdown. Returns the written file's
    // absolute path, or an empty string on failure; either way the outcome is told through notice().
    QString run(HostedReader* reader);
}
