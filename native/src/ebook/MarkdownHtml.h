// A DELIBERATELY SMALL Markdown-to-HTML pass (issue #144), header-only and QtCore-only so the whole of it is
// unit-testable with no widget, no file and no reader.
//
// WHAT IT DOES, AND WHY THAT IS THE WHOLE LIST. Headings, emphasis, lists, links, inline and fenced code,
// block quotes, images by relative path, horizontal rules. Nothing else: no tables, no footnotes, no
// definition lists, no HTML passthrough, no reference links. The output is read by QTextBrowser, whose HTML
// subset does not render a table the way a Markdown table is meant to look anyway, and every construct past
// this line is a plugin's worth of parser for a document type people mostly write as prose.
//
// WHY NOT QTextDocument::setMarkdown. Two reasons, and the first is decisive: the reader's model is a LIST OF
// CHAPTER FILES (EbookSource), so the document has to be SPLIT at its top-level headings before anything
// renders it — and a QTextDocument gives back one document, not a split. The second is that the reader
// already renders (X)HTML for EPUB, MOBI and FB2, so HTML is the one representation this whole subsystem
// speaks; adding a second document pipeline for one format would make "the reader" two readers.
//
// HTML IS ESCAPED, ALWAYS. A .md is a file off somebody's disk, and `<script>` in one is text, not markup.
// Escaping happens FIRST, before any construct is recognised, so there is no ordering by which a tag can
// survive into the output.
#pragma once
#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace MarkdownHtml
{
    // One chapter: the heading that opened it (empty for a document with no top-level heading, or for the
    // text before the first one) and its rendered HTML.
    struct Section
    {
        QString title;
        QString html;
    };

    namespace detail
    {
        // The two sentinels code spans are parked under while the emphasis rules run. Stripped from the
        // input first, so a document containing them cannot forge one.
        inline QChar codeOpen()  { return QChar(0x0001); }
        inline QChar codeClose() { return QChar(0x0002); }

        inline QString escapeHtml(QString s)
        {
            s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
            s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
            s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
            return s;
        }
    }

    // One line of Markdown's inline layer. Order is the whole correctness argument:
    //   escape -> code spans OUT -> images -> links -> strong -> emphasis -> code spans BACK IN.
    // Code goes out before emphasis so `a * b * c` in backticks keeps its asterisks, and comes back after so
    // its own content is never re-scanned. Images go before links because `![a](b)` also matches `[a](b)`.
    inline QString inlineToHtml(const QString& lineIn)
    {
        QString s = lineIn;
        s.remove(detail::codeOpen());
        s.remove(detail::codeClose());
        s = detail::escapeHtml(s);

        // Code spans: `x` and ``x`` (the double form so a span can contain a backtick).
        QStringList codes;
        static const QRegularExpression reCode(QStringLiteral("``(.+?)``|`([^`]+)`"));
        {
            QString out;
            int last = 0;
            auto it = reCode.globalMatch(s);
            while (it.hasNext())
            {
                const auto m = it.next();
                out += s.mid(last, m.capturedStart() - last);
                const QString body = m.captured(1).isNull() ? m.captured(2) : m.captured(1);
                out += detail::codeOpen() + QString::number(codes.size()) + detail::codeClose();
                codes << body;
                last = m.capturedEnd();
            }
            out += s.mid(last);
            s = out;
        }

        // Images BEFORE links (an image is a link with a bang in front of it). Relative paths only in
        // practice, but the href is written out verbatim either way — resolution is the renderer's.
        static const QRegularExpression reImg(QStringLiteral("!\\[([^\\]]*)\\]\\(([^)\\s]+)[^)]*\\)"));
        s.replace(reImg, QStringLiteral("<img src=\"\\2\" alt=\"\\1\">"));
        static const QRegularExpression reLink(QStringLiteral("\\[([^\\]]+)\\]\\(([^)\\s]+)[^)]*\\)"));
        s.replace(reLink, QStringLiteral("<a href=\"\\2\">\\1</a>"));

        // Strong before emphasis, or "**bold**" would be read as an empty italic wrapping "*bold*".
        static const QRegularExpression reStrongA(QStringLiteral("\\*\\*(\\S(?:.*?\\S)?)\\*\\*"));
        static const QRegularExpression reStrongB(QStringLiteral("__(\\S(?:.*?\\S)?)__"));
        s.replace(reStrongA, QStringLiteral("<b>\\1</b>"));
        s.replace(reStrongB, QStringLiteral("<b>\\1</b>"));
        static const QRegularExpression reEmA(QStringLiteral("\\*(\\S(?:[^*]*?\\S)?)\\*"));
        static const QRegularExpression reEmB(QStringLiteral("(?<![A-Za-z0-9_])_(\\S(?:[^_]*?\\S)?)_(?![A-Za-z0-9_])"));
        s.replace(reEmA, QStringLiteral("<i>\\1</i>"));
        s.replace(reEmB, QStringLiteral("<i>\\1</i>"));

        for (int i = codes.size() - 1; i >= 0; --i)
            s.replace(detail::codeOpen() + QString::number(i) + detail::codeClose(),
                      QStringLiteral("<code>") + codes.at(i) + QStringLiteral("</code>"));
        return s;
    }

    // The TOP-LEVEL headings, in order, without rendering anything. The library scan (#134) wants a chapter
    // count and a title out of a .md and has no use for its HTML, and render() would build the whole document
    // to answer that. Same two rules as render(): a '#' heading only, and never one inside a fenced block.
    inline QStringList topLevelHeadings(const QString& markdown)
    {
        static const QRegularExpression reH1(QStringLiteral("^#\\s+(.*?)\\s*#*\\s*$"));
        QStringList out;
        bool inFence = false;
        const QStringList lines = markdown.split(QLatin1Char('\n'));
        for (QString line : lines)
        {
            if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
            const QString t = line.trimmed();
            if (t.startsWith(QLatin1String("```")) || t.startsWith(QLatin1String("~~~")))
            { inFence = !inFence; continue; }
            if (inFence) continue;
            const auto m = reH1.match(line);
            if (m.hasMatch()) out << m.captured(1);
        }
        return out;
    }

    // The block layer. Line-driven, single pass, no lookahead beyond "is this line blank".
    inline QVector<Section> render(const QString& markdown)
    {
        QVector<Section> out;
        Section cur;
        QStringList para;            // the paragraph being accumulated
        bool inList = false, listOrdered = false, inQuote = false, inFence = false;
        QString fence;               // the fenced block's accumulated (escaped) text

        auto closePara = [&]() {
            if (para.isEmpty()) return;
            cur.html += QStringLiteral("<p>") + inlineToHtml(para.join(QLatin1Char(' ')))
                        + QStringLiteral("</p>");
            para.clear();
        };
        auto closeList = [&]() {
            if (!inList) return;
            cur.html += listOrdered ? QStringLiteral("</ol>") : QStringLiteral("</ul>");
            inList = false;
        };
        auto closeQuote = [&]() {
            if (!inQuote) return;
            cur.html += QStringLiteral("</blockquote>");
            inQuote = false;
        };
        auto closeBlocks = [&]() { closePara(); closeList(); closeQuote(); };
        auto flushSection = [&]() {
            closeBlocks();
            if (!cur.html.trimmed().isEmpty() || !cur.title.isEmpty()) out.append(cur);
            cur = Section();
        };

        static const QRegularExpression reHeading(QStringLiteral("^(#{1,6})\\s+(.*?)\\s*#*\\s*$"));
        static const QRegularExpression reBullet(QStringLiteral("^\\s{0,3}[-*+]\\s+(.*)$"));
        static const QRegularExpression reNumber(QStringLiteral("^\\s{0,3}\\d+[.)]\\s+(.*)$"));
        static const QRegularExpression reRule(QStringLiteral("^\\s{0,3}([-*_])(\\s*\\1){2,}\\s*$"));
        static const QRegularExpression reQuote(QStringLiteral("^\\s{0,3}>\\s?(.*)$"));

        const QStringList lines = markdown.split(QLatin1Char('\n'));
        for (QString line : lines)
        {
            if (line.endsWith(QLatin1Char('\r'))) line.chop(1);

            // A fenced block swallows everything, headings included: inside ``` a '#' is a comment, not a
            // chapter, and treating it as one would split a shell script across two pages of the reader.
            if (line.trimmed().startsWith(QLatin1String("```"))
                || line.trimmed().startsWith(QLatin1String("~~~")))
            {
                if (inFence)
                {
                    cur.html += QStringLiteral("<pre><code>") + fence + QStringLiteral("</code></pre>");
                    fence.clear();
                    inFence = false;
                }
                else { closeBlocks(); inFence = true; }
                continue;
            }
            if (inFence) { fence += detail::escapeHtml(line) + QLatin1Char('\n'); continue; }

            const auto head = reHeading.match(line);
            if (head.hasMatch())
            {
                const int level = int(head.captured(1).size());
                const QString text = head.captured(2);
                // A TOP-LEVEL heading is a chapter boundary; every deeper one is a heading inside the
                // chapter it falls in. That is the whole chaptering rule, and it is why a document with no
                // '#' at all comes back as exactly one section.
                if (level == 1)
                {
                    flushSection();
                    cur.title = text;
                    cur.html += QStringLiteral("<h1>") + inlineToHtml(text) + QStringLiteral("</h1>");
                }
                else
                {
                    closeBlocks();
                    cur.html += QStringLiteral("<h%1>").arg(level) + inlineToHtml(text)
                                + QStringLiteral("</h%1>").arg(level);
                }
                continue;
            }

            if (reRule.match(line).hasMatch()) { closeBlocks(); cur.html += QStringLiteral("<hr>"); continue; }

            const auto quote = reQuote.match(line);
            if (quote.hasMatch())
            {
                closePara();
                closeList();
                if (!inQuote) { cur.html += QStringLiteral("<blockquote>"); inQuote = true; }
                cur.html += QStringLiteral("<p>") + inlineToHtml(quote.captured(1)) + QStringLiteral("</p>");
                continue;
            }

            const auto bullet = reBullet.match(line);
            const auto number = bullet.hasMatch() ? QRegularExpressionMatch() : reNumber.match(line);
            if (bullet.hasMatch() || number.hasMatch())
            {
                closePara();
                closeQuote();
                const bool ordered = number.hasMatch();
                if (inList && ordered != listOrdered) closeList();
                if (!inList)
                {
                    cur.html += ordered ? QStringLiteral("<ol>") : QStringLiteral("<ul>");
                    inList = true;
                    listOrdered = ordered;
                }
                cur.html += QStringLiteral("<li>")
                            + inlineToHtml(ordered ? number.captured(1) : bullet.captured(1))
                            + QStringLiteral("</li>");
                continue;
            }

            if (line.trimmed().isEmpty()) { closeBlocks(); continue; }

            closeList();
            closeQuote();
            para << line.trimmed();
        }

        if (inFence && !fence.isEmpty())
            cur.html += QStringLiteral("<pre><code>") + fence + QStringLiteral("</code></pre>");
        flushSection();
        return out;
    }
}
