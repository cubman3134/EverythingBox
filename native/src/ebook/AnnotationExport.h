// AnnotationExport — "export my notes from this book" (issue #136): one book's bookmarks and highlights, with
// their notes, as a Markdown document (or the same content as plain text), in DOCUMENT order. Pure and
// header-only apart from writeExport(), so every byte of the format is pinned by probe_highlights rather than
// by opening a file and looking at it.
//
// THE ORDER is the panel's: ReaderAnnotations::merged, the one document-order rule both panels already draw
// from. An export that listed things in a different order from the list the reader has just been looking at
// would be a second answer to a settled question.
//
// THE FORMAT (Markdown):
//
//     # <title>
//
//     by <author>                       (only when the book names one)
//
//     ## Yellow highlight — <chapter or page>
//
//     > <the highlighted words, every line quoted>
//
//     **Note:** <the note, when there is one>
//
//     ## Bookmark — <chapter or page>
//
// One "##" section per annotation, so any Markdown viewer gives each its own heading and a table of contents
// for free. A book with none says "No notes yet." under its header rather than exporting an empty file.
// Plain text is the SAME content, line for line, with no Markdown syntax: the heading lines lose their '#',
// the quote is in quotation marks, and nothing is escaped.
//
// ESCAPING. The title, the author, the chapter names, the quoted words and the note are all TEXT, and a note
// that contains '#' or '*' must read as those characters, not become a heading or emphasis. escapeMarkdown()
// backslash-escapes every character that can start inline formatting ANYWHERE it appears — \ ` * _ [ ] < > #
// | ~ & — and, at the start of a line only, the markers that are only markers there: '-', '+', '=' and the '.'
// or ')' after an ordered-list number. Leading indentation is dropped per line, because four spaces make a code
// block. CommonMark lets any ASCII punctuation be backslash-escaped, so the escaped text renders as exactly the
// characters the reader typed. Punctuation that is harmless where it stands ('-' mid-word, "1.5") is left
// alone, so the file still reads as prose in a plain editor.
//
// THE FILE: "<title> — notes.md" in <data dir>/exports — a fixed, visible folder beside the app's own data,
// created on first use. The title goes through the app's existing file-name rule
// (RomhackInstall::sanitizeHackTitle: every character a file system reserves becomes a space, runs collapse,
// trailing dots go); a title that sanitises to nothing exports as "Untitled". Re-exporting the same book
// REPLACES its file (it is the same book's notes, newer), written atomically through QSaveFile.
#pragma once
#include <QDir>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ReaderAnchor.h"
#include "ReaderAnnotations.h"
#include "../core/BookmarkStore.h"
#include "../core/HighlightStore.h"
#include "../core/RomhackInstall.h"

namespace AnnotationExport
{

enum class Format { Markdown, PlainText };

// What the header and the location labels need to know about the book.
struct BookInfo
{
    QString     title;
    QString     author;
    QStringList chapterTitles;   // indexed by spine; empty for a pdf / comic (their anchors are pages)
};

// One line of the user's text, escaped (see ESCAPING above).
inline QString escapeMarkdownLine(const QString& raw)
{
    int lead = 0;
    while (lead < raw.size() && (raw.at(lead) == QLatin1Char(' ') || raw.at(lead) == QLatin1Char('\t'))) ++lead;
    const QString line = raw.mid(lead);

    static const QString kInline = QStringLiteral("\\`*_[]<>#|~&");
    QString out;
    out.reserve(line.size() + 8);
    for (const QChar c : line)
    {
        if (kInline.contains(c)) out += QLatin1Char('\\');
        out += c;
    }
    if (out.isEmpty()) return out;

    // Line-start markers. Checked on the ESCAPED line: nothing above inserts before a '-', '+', '=' or digit,
    // so its first character is still the line's first character.
    const QChar first = out.at(0);
    if (first == QLatin1Char('-') || first == QLatin1Char('+') || first == QLatin1Char('='))
        out.prepend(QLatin1Char('\\'));
    else if (first >= QLatin1Char('0') && first <= QLatin1Char('9'))
    {
        int j = 0;
        while (j < out.size() && out.at(j) >= QLatin1Char('0') && out.at(j) <= QLatin1Char('9')) ++j;
        if (j < out.size() && (out.at(j) == QLatin1Char('.') || out.at(j) == QLatin1Char(')')))
            out.insert(j, QLatin1Char('\\'));
    }
    return out;
}

// The user's text, escaped line by line. Line breaks are kept (a note may have several lines).
inline QString escapeMarkdown(const QString& text)
{
    QString norm = text;
    norm.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    norm.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = norm.split(QLatin1Char('\n'));
    for (QString& l : lines) l = escapeMarkdownLine(l);
    return lines.join(QLatin1Char('\n'));
}

// A title, author or chapter name on ONE line: a heading cannot span two.
inline QString oneLine(const QString& s)
{
    return s.simplified();
}

// Where an annotation is, in words: the chapter's own title when the book has one for that spine, else
// "Chapter N"; a pdf or comic page is "Page N". 1-based, as a reader counts.
inline QString locationLabel(const ReaderAnchor& a, const QStringList& chapterTitles)
{
    if (a.kind == ReaderAnchor::Book)
    {
        if (a.spine >= 0 && a.spine < chapterTitles.size())
        {
            const QString t = oneLine(chapterTitles.at(a.spine));
            if (!t.isEmpty()) return t;
        }
        return QStringLiteral("Chapter %1").arg(a.spine + 1);
    }
    return QStringLiteral("Page %1").arg(a.page + 1);
}

// The whole document. See THE FORMAT above; the probe pins it byte for byte.
inline QString render(const BookInfo& info, const QVector<BookmarkStore::Bookmark>& bookmarks,
                      const QVector<HighlightStore::Highlight>& highlights, Format format)
{
    const bool md = (format == Format::Markdown);
    const QString dash = QStringLiteral(" ") + QChar(0x2014) + QStringLiteral(" ");
    const QString title  = oneLine(info.title).isEmpty() ? QStringLiteral("Untitled") : oneLine(info.title);
    const QString author = oneLine(info.author);

    QString out;
    if (md)
    {
        out += QStringLiteral("# ") + escapeMarkdown(title) + QStringLiteral("\n\n");
        if (!author.isEmpty()) out += QStringLiteral("by ") + escapeMarkdown(author) + QStringLiteral("\n\n");
    }
    else
    {
        out += title + QLatin1Char('\n');
        if (!author.isEmpty()) out += QStringLiteral("by ") + author + QLatin1Char('\n');
        out += QLatin1Char('\n');
    }

    const QVector<ReaderAnnotations::Entry> entries = ReaderAnnotations::merged(bookmarks, highlights);
    if (entries.isEmpty()) return out + QStringLiteral("No notes yet.\n");

    QStringList blocks;
    for (const ReaderAnnotations::Entry& e : entries)
    {
        const QString where = locationLabel(e.anchor, info.chapterTitles);
        QString block;
        if (!e.isHighlight())
        {
            block = md ? QStringLiteral("## Bookmark") + dash + escapeMarkdown(where) + QLatin1Char('\n')
                       : QStringLiteral("Bookmark") + dash + where + QLatin1Char('\n');
            blocks << block;
            continue;
        }
        const QString what = HighlightStore::colorName(e.color) + QStringLiteral(" highlight");
        if (md)
        {
            block = QStringLiteral("## ") + what + dash + escapeMarkdown(where) + QStringLiteral("\n\n");
            QStringList quoted = escapeMarkdown(e.excerpt).split(QLatin1Char('\n'));
            for (QString& q : quoted) q = q.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + q;
            block += quoted.join(QLatin1Char('\n')) + QLatin1Char('\n');
            if (e.hasNote())
                block += QStringLiteral("\n**Note:** ") + escapeMarkdown(e.note) + QLatin1Char('\n');
        }
        else
        {
            block = what + dash + where + QLatin1Char('\n');
            block += QChar(0x201C) + e.excerpt + QChar(0x201D) + QLatin1Char('\n');
            if (e.hasNote()) block += QStringLiteral("Note: ") + e.note + QLatin1Char('\n');
        }
        blocks << block;
    }
    return out + blocks.join(QLatin1Char('\n'));
}

// "<title> — notes.md", the title made safe by the app's existing file-name rule.
inline QString fileNameFor(const QString& title)
{
    QString safe = RomhackInstall::sanitizeHackTitle(title);
    if (safe.isEmpty()) safe = QStringLiteral("Untitled");
    return safe + QStringLiteral(" ") + QChar(0x2014) + QStringLiteral(" notes.md");
}

// The fixed, visible export folder: <data dir>/exports.
inline QString exportDir(const QString& dataDir)
{
    return dataDir + QStringLiteral("/exports");
}

// Write `content` (UTF-8) to dir/fileName, creating the folder and REPLACING any existing file of that name,
// atomically. Returns the file's absolute path, or an empty string with `error` set.
inline QString writeExport(const QString& dir, const QString& fileName, const QString& content,
                           QString* error = nullptr)
{
    if (!QDir().mkpath(dir))
    {
        if (error) *error = QStringLiteral("could not create the folder %1").arg(QDir::toNativeSeparators(dir));
        return QString();
    }
    const QString path = QDir(dir).absoluteFilePath(fileName);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
    {
        if (error) *error = f.errorString();
        return QString();
    }
    f.write(content.toUtf8());
    if (!f.commit())
    {
        if (error) *error = f.errorString();
        return QString();
    }
    return path;
}

} // namespace AnnotationExport
