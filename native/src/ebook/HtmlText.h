// A SINGLE-FILE HTML DOCUMENT AS A BOOK (issue #259). The HTML half of TextBook, the way MarkdownHtml is the
// Markdown half: header-only and QtCore-only, so every decision below is testable with no widget and no reader.
// TextBook owns the file, the encoding ladder and the staging; this owns what an .html's markup is allowed to
// become.
//
// ---- THE PRIVACY LINE, and why this is an allowlist -------------------------------------------------------
//
// Opening a local file must never make the app fetch anything over the network. A remote <img> in an HTML
// file is a tracking pixel the moment it loads; a stylesheet can name a URL; a UNC path is a network fetch that
// on Windows also hands over the user's credentials. So the output is REBUILT from the input, element by
// element, and only what is on the lists below is written back out. Nothing is passed through because it
// "looked harmless" - a new tag or attribute nobody thought of is dropped by default, not kept by default.
//
//   KEPT:     text; headings; paragraphs, line breaks and rules; inline formatting (b/i/u/s/em/strong/code/
//             sub/sup/small/...); lists; block quotes and <pre>; tables; plain block wrappers (div/section/
//             article/...); <a> for its text and an IN-DOCUMENT (#fragment) href only; <img> whose source is
//             local (below). Attributes: id, title, lang, dir, align, and the handful a table or list needs.
//   DROPPED WITH THEIR CONTENT: <script>, <style>, <iframe>, <object>/<applet>, <svg>/<math>, <template>,
//             <video>/<audio>/<canvas>, <select>, <textarea>, <noembed>/<noframes>/<frameset>, and <title>
//             (read as metadata, never shown as body text).
//   DROPPED, CONTENT KEPT: every other element - <embed>, <frame>, <link>, <base>, <meta>, <form>, <font>,
//             <picture>, <source>, ... - and EVERY attribute not on the list: all event handlers (on*), style
//             (CSS can carry url()), srcset, background, poster, data, action, cite.
//   IMAGE SOURCES: kept only when the path is RELATIVE and resolves to an existing file INSIDE the HTML
//             file's own folder (subfolders allowed). Refused: http(s):, ftp:, file:, javascript: and any other
//             scheme (a drive letter "C:" parses as one); protocol-relative "//host"; a UNC "\\host"; an
//             absolute "/path"; any path whose ".." climbs out of the folder (checked after percent-decoding,
//             and again on the CANONICAL path, so a link inside the folder that points out of it is refused
//             too); a file that does not exist. A kept image is written as the absolute file: URL of the
//             canonical path, so it loads from the book's folder and not from wherever the chapter is staged.
//             The one other source is an image carried INSIDE the file (data:image/png|jpeg|gif|webp|bmp;
//             base64), which is not a fetch: it is staged beside the chapters, exactly as an FB2 <binary> is.
//
// ---- WHAT BECOMES A CHAPTER -------------------------------------------------------------------------------
//
// The document splits at its TOP-LEVEL heading level, which is the SHALLOWEST heading level present (an HTML
// file that starts at <h2> has <h2> chapters). If that level occurs EXACTLY ONCE it is the document's title,
// not a chapter break, and the split moves to the next level down that is present (one <h1> over <h2>s splits
// at <h2>; one <h1> over only <h3>s splits at <h3>). Content before the first split heading - in the title
// case, the title heading and any preface - is a chapter of its own when it holds any text or image, and in
// the title case the contents panel names it by that heading. No headings at all, or one heading with nothing
// below it: ONE chapter, and no contents panel. A split inside a wrapper (<div>, <section>, ...) closes the
// wrapper at the end of one chapter and re-opens it at the start of the next, so every chapter is balanced.
//
// ---- TITLE AND AUTHOR -------------------------------------------------------------------------------------
//
// Title: <title> if it is present and not blank, else the first heading's text, else nothing (and the caller
// uses the filename). Author: a <meta name="author" content="..."> only. Never guessed.
//
// ---- ENCODING ---------------------------------------------------------------------------------------------
//
// declaredCharset() finds what the document says about itself - <meta charset> or the http-equiv
// Content-Type - in its first 4 KiB, comments excluded. TextBook::decode() takes it as the rung between the
// BOM and the rest of the ladder: a BOM outranks a claim written inside the bytes, and the claim outranks a
// guess.
#pragma once
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLatin1String>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace HtmlText
{
    // One chapter: the heading that opened it (empty for untitled front matter and for a one-chapter document)
    // and its sanitised HTML.
    struct Chapter
    {
        QString title;
        QString html;
    };

    // An image the document carried inside itself (a data: URI), to be written beside the chapters under
    // `name` - the name the chapter HTML already refers to it by.
    struct StagedImage
    {
        QString    name;
        QByteArray bytes;
    };

    struct Document
    {
        QString title;               // <title>, else the first heading, else empty (the caller's filename)
        QString author;              // <meta name="author">, else empty
        QVector<Chapter> chapters;   // never empty
        QVector<StagedImage> images; // data: images to stage (only when parse() was given a folder)
        int splitLevel = 0;          // the heading level chapters were split at, 1..6; 0 == one chapter
    };

    namespace detail
    {
        inline QString escapeAttr(QString s)
        {
            s.replace(QLatin1Char('&'), QLatin1String("&amp;"));
            s.replace(QLatin1Char('<'), QLatin1String("&lt;"));
            s.replace(QLatin1Char('>'), QLatin1String("&gt;"));
            s.replace(QLatin1Char('"'), QLatin1String("&quot;"));
            return s;
        }

        inline bool oneOf(const QString& s, std::initializer_list<const char*> list)
        {
            for (const char* x : list) if (s == QLatin1String(x)) return true;
            return false;
        }

        // Elements with no content and no end tag.
        inline bool isVoid(const QString& n)
        {
            return oneOf(n, { "br", "hr", "img", "wbr", "col", "area", "base", "embed", "input", "link", "meta",
                              "param", "source", "track", "frame", "keygen" });
        }

        // THE ELEMENT ALLOWLIST. Anything not here is dropped (its content still read, unless it is below).
        inline bool keepsTag(const QString& n)
        {
            return oneOf(n, { "p", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6",
                              "b", "strong", "i", "em", "u", "s", "strike", "del", "ins", "mark", "small", "big",
                              "sub", "sup", "code", "kbd", "samp", "tt", "var", "cite", "dfn", "abbr", "acronym",
                              "q", "span", "bdi", "bdo", "time", "ruby", "rt", "rp", "wbr",
                              "blockquote", "pre", "ul", "ol", "li", "dl", "dt", "dd",
                              "div", "section", "article", "main", "header", "footer", "aside", "nav", "address",
                              "center", "figure", "figcaption",
                              "table", "caption", "thead", "tbody", "tfoot", "tr", "th", "td", "col", "colgroup",
                              "a", "img" });
        }

        // THE ATTRIBUTE ALLOWLIST. No on*, no style, no srcset/background/poster/data/action/cite: nothing that
        // runs and nothing that names a location, except the two sources sanitised in parse().
        inline bool keepsAttr(const QString& tag, const QString& a)
        {
            if (oneOf(a, { "id", "title", "lang", "dir", "align" })) return true;
            if (tag == QLatin1String("a"))   return oneOf(a, { "href", "name" });
            if (tag == QLatin1String("img")) return oneOf(a, { "src", "alt", "width", "height" });
            if (tag == QLatin1String("td") || tag == QLatin1String("th"))
                return oneOf(a, { "colspan", "rowspan", "valign", "width" });
            if (tag == QLatin1String("ol"))  return oneOf(a, { "start", "type" });
            if (tag == QLatin1String("ul"))  return a == QLatin1String("type");
            if (tag == QLatin1String("li"))  return a == QLatin1String("value");
            if (tag == QLatin1String("table")) return oneOf(a, { "border", "cellpadding", "cellspacing", "width" });
            if (tag == QLatin1String("col") || tag == QLatin1String("colgroup")) return oneOf(a, { "span", "width" });
            return false;
        }

        // Raw-text elements: their content is NOT markup (a '<' in a script is a less-than sign), so it is
        // skipped to the matching end tag without being tokenised. All of them are dropped with their content;
        // <title>'s is read as the document title.
        inline bool isRawText(const QString& n)
        {
            return oneOf(n, { "script", "style", "textarea", "title", "xmp", "iframe", "noembed", "noframes",
                              "plaintext" });
        }

        // Elements whose content is tokenised but dropped: active or embedded content, and the fallback text
        // that stands in for it ("your browser cannot play video" is not a line of the book).
        inline bool dropsContent(const QString& n)
        {
            return oneOf(n, { "object", "applet", "svg", "math", "template", "select", "video", "audio", "canvas",
                              "frameset" });
        }

        // Wrappers re-opened at the top of the next chapter when a split falls inside them.
        inline bool isWrapper(const QString& n)
        {
            return oneOf(n, { "div", "section", "article", "main", "header", "footer", "aside", "nav", "address",
                              "blockquote", "center", "figure" });
        }

        // Block starts that close an open <p> (the one implicit end tag the reader's own parser handles worst).
        inline bool closesP(const QString& n)
        {
            return oneOf(n, { "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl", "pre", "blockquote",
                              "table", "hr", "section", "article", "main", "header", "footer", "aside", "nav",
                              "address", "center", "figure", "figcaption" });
        }

        inline int headingLevel(const QString& n)
        {
            if (n.size() == 2 && n.at(0) == QLatin1Char('h') && n.at(1) >= QLatin1Char('1')
                && n.at(1) <= QLatin1Char('6'))
                return n.at(1).unicode() - '0';
            return 0;
        }
    }

    // Character references, for the text this code READS (a title, a heading for the contents panel, an
    // attribute value it re-writes). Body text is passed to the reader verbatim, whose own parser knows every
    // entity. Numeric references in full; named ones for the Latin-1 range (in code-point order, so the table
    // is its own index) and the typographic set real documents use. An unknown name is left as written.
    inline QString decodeEntities(const QString& s)
    {
        if (!s.contains(QLatin1Char('&'))) return s;
        static const QHash<QString, char32_t> named = [] {
            QHash<QString, char32_t> h;
            const char* latin1 =
                "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr "
                "deg plusmn sup2 sup3 acute micro para middot cedil sup1 ordm raquo frac14 frac12 frac34 iquest "
                "Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc Iuml "
                "ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig "
                "agrave aacute acirc atilde auml aring aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml "
                "eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml";
            char32_t cp = 0xA0;
            for (const QString& n : QString::fromLatin1(latin1).split(QLatin1Char(' ')))
                h.insert(n, cp++);
            const struct { const char* n; char32_t c; } more[] = {
                { "amp", U'&' }, { "lt", U'<' }, { "gt", U'>' }, { "quot", U'"' }, { "apos", U'\'' },
                { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
                { "sbquo", 0x201A }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E },
                { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 }, { "hellip", 0x2026 },
                { "permil", 0x2030 }, { "prime", 0x2032 }, { "Prime", 0x2033 }, { "lsaquo", 0x2039 },
                { "rsaquo", 0x203A }, { "euro", 0x20AC }, { "trade", 0x2122 }, { "OElig", 0x0152 },
                { "oelig", 0x0153 }, { "Scaron", 0x0160 }, { "scaron", 0x0161 }, { "Yuml", 0x0178 },
                { "fnof", 0x0192 }, { "circ", 0x02C6 }, { "tilde", 0x02DC }, { "ensp", 0x2002 },
                { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200C }, { "zwj", 0x200D },
                { "lrm", 0x200E }, { "rlm", 0x200F } };
            for (const auto& m : more) h.insert(QString::fromLatin1(m.n), m.c);
            return h;
        }();

        QString out;
        out.reserve(s.size());
        for (qsizetype i = 0; i < s.size(); ++i)
        {
            if (s.at(i) != QLatin1Char('&')) { out += s.at(i); continue; }
            const qsizetype semi = s.indexOf(QLatin1Char(';'), i + 1);
            if (semi < 0 || semi - i > 32) { out += s.at(i); continue; }
            const QString ent = s.mid(i + 1, semi - i - 1);
            char32_t cp = 0;
            bool ok = false;
            if (ent.startsWith(QLatin1Char('#')))
            {
                const bool hex = ent.size() > 1 && (ent.at(1) == QLatin1Char('x') || ent.at(1) == QLatin1Char('X'));
                cp = char32_t(hex ? ent.mid(2).toUInt(&ok, 16) : ent.mid(1).toUInt(&ok, 10));
                if (ok && (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) cp = 0xFFFD;
            }
            else
            {
                const auto it = named.constFind(ent);
                if (it != named.constEnd()) { cp = *it; ok = true; }
            }
            if (!ok) { out += s.at(i); continue; }
            out += QString::fromUcs4(&cp, 1);
            i = semi;
        }
        return out;
    }

    // What the document says its encoding is, lower-cased, or empty. Read from the first 4 KiB as ASCII (every
    // label and tag in question is ASCII; a UTF-16 document has a BOM, which outranks this anyway), with
    // comments removed first so a commented-out declaration is not a declaration.
    inline QByteArray declaredCharset(const QByteArray& bytes)
    {
        QByteArray head = bytes.left(4096);
        for (qsizetype a = head.indexOf("<!--"); a >= 0; a = head.indexOf("<!--", a))
        {
            const qsizetype e = head.indexOf("-->", a + 4);
            if (e < 0) { head.truncate(a); break; }
            head.remove(a, e + 3 - a);
        }
        static const QRegularExpression reMeta(QStringLiteral("<meta\\b[^>]*>"),
                                               QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression reCharset(QStringLiteral("charset\\s*=\\s*[\"']?\\s*([A-Za-z0-9_.:+\\-]+)"),
                                                  QRegularExpression::CaseInsensitiveOption);
        auto it = reMeta.globalMatch(QString::fromLatin1(head));
        while (it.hasNext())
        {
            const auto c = reCharset.match(it.next().captured(0));
            if (c.hasMatch()) return c.captured(1).toLatin1().toLower();
        }
        return QByteArray();
    }

    // An <img src> the reader may load: the absolute file: URL of an existing file inside `fileDir`, the
    // staged name of an image the document carries inline, or EMPTY (drop the image). See the header for the
    // list; each refusal below is one line of it. An empty `fileDir` refuses everything (a metadata-only pass).
    inline QString localImage(const QString& raw, const QString& fileDir, Document* doc, int* staged)
    {
        if (fileDir.isEmpty()) return QString();
        QString v = raw.trimmed();
        if (v.isEmpty()) return QString();

        // An image carried inside the file. Not a fetch: staged beside the chapters like an FB2 <binary>, under
        // a name this code chose (never one the document chose).
        static const QRegularExpression reData(QStringLiteral("^data:image/(png|jpe?g|gif|webp|bmp);base64,"),
                                               QRegularExpression::CaseInsensitiveOption);
        const auto dm = reData.match(v);
        if (dm.hasMatch())
        {
            QString body = v.mid(dm.capturedEnd());
            body.remove(QRegularExpression(QStringLiteral("\\s")));
            const QByteArray bytes = QByteArray::fromBase64(body.toLatin1());
            if (bytes.isEmpty()) return QString();
            QString ext = dm.captured(1).toLower();
            if (ext == QLatin1String("jpeg")) ext = QStringLiteral("jpg");
            const QString name = QStringLiteral("htmlimg%1.%2").arg(++*staged).arg(ext);
            doc->images.append({ name, bytes });
            return name;
        }

        // Anything that names a place of its own: an absolute "/path", protocol-relative "//host", a UNC
        // "\\host", and any scheme at all - http:, https:, ftp:, file:, javascript:, a data: that is not an
        // inline image, and a drive letter ("C:" parses as a one-letter scheme).
        static const QRegularExpression reScheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.\\-]*:"));
        auto namesAPlace = [&](const QString& s) {
            return s.startsWith(QLatin1Char('/')) || s.startsWith(QLatin1Char('\\')) || reScheme.match(s).hasMatch();
        };
        if (namesAPlace(v)) return QString();

        // A URL's query and fragment are not part of a file name; its percent-escapes are. Checked AGAIN after
        // decoding, so "%2F%2Fhost" and "C%3A/x" are refused as what they decode to.
        static const QRegularExpression reQuery(QStringLiteral("[?#]"));
        const qsizetype cut = v.indexOf(reQuery);
        if (cut >= 0) v.truncate(cut);
        v = QUrl::fromPercentEncoding(v.toUtf8());
        v.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (v.isEmpty() || namesAPlace(v)) return QString();

        auto under = [](const QString& root) {
            return root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        };
        const QString root = QDir::cleanPath(QFileInfo(fileDir).absoluteFilePath());
        const QString full = QDir::cleanPath(root + QLatin1Char('/') + v);
        if (!full.startsWith(under(root))) return QString();          // ".." climbed out of the folder
        const QFileInfo fi(full);
        if (!fi.isFile()) return QString();                            // nothing there to show
        // Where it REALLY is: a link or junction inside the folder can point anywhere.
        const QString canonRoot = QFileInfo(root).canonicalFilePath();
        const QString canon = fi.canonicalFilePath();
        if (canonRoot.isEmpty() || canon.isEmpty() || !canon.startsWith(under(canonRoot))) return QString();
        return QUrl::fromLocalFile(canon).toString(QUrl::FullyEncoded);
    }

    // The whole pass: tokenise, sanitise against the lists above, then split into chapters by the rule above.
    // `fileDir` is the HTML file's folder, which relative images resolve against; empty resolves none.
    inline Document parse(const QString& src, const QString& fileDir)
    {
        using namespace detail;
        Document doc;

        struct Token
        {
            enum Kind { Text, Start, End } kind;
            QString name;   // element name, lower case (Start/End)
            QString raw;    // the text as written (Text) or the sanitised start tag (Start)
            int level;      // heading level 1..6, else 0
        };
        QVector<Token> toks;
        QString titleText;
        bool haveTitle = false;
        QString dropName;
        int dropDepth = 0;
        int staged = 0;

        const qsizetype n = src.size();
        qsizetype i = 0;
        while (i < n)
        {
            if (src.at(i) != QLatin1Char('<'))
            {
                qsizetype j = src.indexOf(QLatin1Char('<'), i);
                if (j < 0) j = n;
                if (dropDepth == 0) toks.append({ Token::Text, QString(), src.mid(i, j - i), 0 });
                i = j;
                continue;
            }
            // Comments, doctypes, processing instructions, CDATA: never content.
            if (src.mid(i, 4) == QLatin1String("<!--"))
            {
                const qsizetype e = src.indexOf(QLatin1String("-->"), i + 4);
                i = e < 0 ? n : e + 3;
                continue;
            }
            if (i + 1 < n && (src.at(i + 1) == QLatin1Char('!') || src.at(i + 1) == QLatin1Char('?')))
            {
                const qsizetype e = src.indexOf(QLatin1Char('>'), i);
                i = e < 0 ? n : e + 1;
                continue;
            }
            const bool isEnd = i + 1 < n && src.at(i + 1) == QLatin1Char('/');
            qsizetype p = i + (isEnd ? 2 : 1);
            if (p >= n || !src.at(p).isLetter())
            {
                if (isEnd)   // "</ >", "</3": not a tag, and not text either
                {
                    const qsizetype e = src.indexOf(QLatin1Char('>'), i);
                    i = e < 0 ? n : e + 1;
                }
                else         // a less-than sign in prose
                {
                    if (dropDepth == 0) toks.append({ Token::Text, QString(), QStringLiteral("&lt;"), 0 });
                    ++i;
                }
                continue;
            }
            qsizetype q = p;
            while (q < n && (src.at(q).isLetterOrNumber() || src.at(q) == QLatin1Char('-')
                             || src.at(q) == QLatin1Char(':') || src.at(q) == QLatin1Char('_')))
                ++q;
            const QString name = src.mid(p, q - p).toLower();

            // Attributes: name[=value], value quoted with " or ' (which may then hold a '>') or bare.
            QVector<QPair<QString, QString>> attrs;
            qsizetype k = q;
            bool closed = false;
            while (k < n)
            {
                while (k < n && (src.at(k).isSpace() || src.at(k) == QLatin1Char('/'))) ++k;
                if (k >= n) break;
                if (src.at(k) == QLatin1Char('>')) { ++k; closed = true; break; }
                const qsizetype a = k;
                while (k < n && !src.at(k).isSpace() && src.at(k) != QLatin1Char('=') && src.at(k) != QLatin1Char('>')
                       && src.at(k) != QLatin1Char('/'))
                    ++k;
                if (k == a) { ++k; continue; }   // a stray '=' where a name belongs
                const QString an = src.mid(a, k - a).toLower();
                while (k < n && src.at(k).isSpace()) ++k;
                QString av;
                if (k < n && src.at(k) == QLatin1Char('='))
                {
                    ++k;
                    while (k < n && src.at(k).isSpace()) ++k;
                    if (k < n && (src.at(k) == QLatin1Char('"') || src.at(k) == QLatin1Char('\'')))
                    {
                        const QChar quote = src.at(k);
                        qsizetype e = src.indexOf(quote, k + 1);
                        if (e < 0) e = n;
                        av = src.mid(k + 1, e - k - 1);
                        k = qMin(n, e + 1);
                    }
                    else
                    {
                        qsizetype e = k;
                        while (e < n && !src.at(e).isSpace() && src.at(e) != QLatin1Char('>')) ++e;
                        av = src.mid(k, e - k);
                        k = e;
                    }
                }
                attrs.append({ an, decodeEntities(av) });
            }
            const qsizetype tagEnd = k;
            const bool selfClosing = closed && tagEnd >= 2 && src.at(tagEnd - 2) == QLatin1Char('/');
            i = tagEnd;

            if (!isEnd && isRawText(name))
            {
                const qsizetype close = src.indexOf(QLatin1String("</") + name, tagEnd, Qt::CaseInsensitive);
                const qsizetype stop = close < 0 ? n : close;
                if (name == QLatin1String("title") && dropDepth == 0 && !haveTitle)
                {
                    titleText = decodeEntities(src.mid(tagEnd, stop - tagEnd)).simplified();
                    haveTitle = true;
                }
                if (close < 0) { i = n; continue; }
                const qsizetype gt = src.indexOf(QLatin1Char('>'), close);
                i = gt < 0 ? n : gt + 1;
                continue;
            }
            if (dropDepth > 0)
            {
                if (name == dropName)
                {
                    if (isEnd) --dropDepth;
                    else if (!selfClosing) ++dropDepth;
                }
                continue;
            }
            if (!isEnd && dropsContent(name))
            {
                if (!selfClosing) { dropName = name; dropDepth = 1; }
                continue;
            }
            if (!isEnd && name == QLatin1String("meta"))
            {
                QString metaName, content;
                for (const auto& at : attrs)
                {
                    if (at.first == QLatin1String("name"))    metaName = at.second.trimmed().toLower();
                    if (at.first == QLatin1String("content")) content  = at.second;
                }
                if (metaName == QLatin1String("author") && doc.author.isEmpty())
                    doc.author = content.simplified();
                continue;
            }
            if (!keepsTag(name)) continue;

            const int level = headingLevel(name);
            if (isEnd)
            {
                toks.append({ Token::End, name, QString(), level });
                continue;
            }
            QString tag = QLatin1Char('<') + name;
            QSet<QString> seen;
            bool dropElement = false;
            for (const auto& at : attrs)
            {
                if (!keepsAttr(name, at.first) || seen.contains(at.first)) continue;
                seen.insert(at.first);
                QString v = at.second;
                if (name == QLatin1String("img") && at.first == QLatin1String("src"))
                {
                    v = localImage(v, fileDir, &doc, &staged);
                    if (v.isEmpty()) { dropElement = true; break; }
                }
                if (name == QLatin1String("a") && at.first == QLatin1String("href"))
                {
                    v = v.trimmed();
                    if (!v.startsWith(QLatin1Char('#'))) continue;   // in-document only; the reader navigates no further
                }
                tag += QLatin1Char(' ') + at.first + QStringLiteral("=\"") + escapeAttr(v) + QLatin1Char('"');
            }
            if (name == QLatin1String("img") && !seen.contains(QStringLiteral("src"))) dropElement = true;
            if (dropElement) continue;
            tag += QLatin1Char('>');
            toks.append({ Token::Start, name, tag, level });
        }

        // ---- The split level: the shallowest present, unless it occurs once (then it is the title) ----
        int counts[7] = { 0, 0, 0, 0, 0, 0, 0 };
        for (const Token& t : toks)
            if (t.kind == Token::Start && t.level > 0) ++counts[t.level];
        int split = 0;
        for (int lv = 1; lv <= 6 && !split; ++lv) if (counts[lv] > 0) split = lv;
        int titleLevel = 0;
        if (split && counts[split] == 1)
        {
            titleLevel = split;
            split = 0;
            for (int lv = titleLevel + 1; lv <= 6 && !split; ++lv) if (counts[lv] > 0) split = lv;
        }
        doc.splitLevel = split;

        // ---- Build the chapters ----
        struct Open { QString name; QString tag; };
        QVector<Open> stack;
        Chapter cur;
        bool curHasContent = false;
        bool inFront = true;              // before the first split heading
        int headingOpen = 0;              // level of the heading whose text is being collected
        bool headingNamesChapter = false; // that heading's text is the chapter's contents-panel title
        QString headingText, firstHeading;

        auto closeAll = [&]() {
            for (qsizetype s = stack.size() - 1; s >= 0; --s)
                cur.html += QStringLiteral("</") + stack.at(s).name + QLatin1Char('>');
        };
        auto popTo = [&](qsizetype s) {
            while (stack.size() > s)
            {
                cur.html += QStringLiteral("</") + stack.last().name + QLatin1Char('>');
                stack.removeLast();
            }
        };

        for (const Token& t : toks)
        {
            if (t.kind == Token::Text)
            {
                cur.html += t.raw;
                if (headingOpen) headingText += t.raw;
                if (!t.raw.trimmed().isEmpty()) curHasContent = true;
                continue;
            }
            if (t.kind == Token::End)
            {
                qsizetype s = stack.size() - 1;
                while (s >= 0 && stack.at(s).name != t.name) --s;
                if (s < 0) continue;      // an end tag for nothing open
                popTo(s);
                if (t.level > 0 && t.level == headingOpen)
                {
                    const QString text = decodeEntities(headingText).simplified();
                    if (firstHeading.isEmpty()) firstHeading = text;
                    if (headingNamesChapter) cur.title = text;
                    headingOpen = 0;
                    headingNamesChapter = false;
                }
                continue;
            }

            // A start tag. A split heading first closes the chapter before it and opens the next.
            if (split && t.level == split)
            {
                closeAll();
                if (curHasContent) doc.chapters.append(cur);
                cur = Chapter();
                QVector<Open> keep;
                for (const Open& o : stack)
                    if (isWrapper(o.name)) { keep << o; cur.html += o.tag; }
                stack = keep;
                curHasContent = true;
                inFront = false;
            }
            if (closesP(t.name) && !stack.isEmpty() && stack.last().name == QLatin1String("p")) popTo(stack.size() - 1);
            if ((t.name == QLatin1String("li") && !stack.isEmpty() && stack.last().name == QLatin1String("li"))
                || ((t.name == QLatin1String("dt") || t.name == QLatin1String("dd")) && !stack.isEmpty()
                    && (stack.last().name == QLatin1String("dt") || stack.last().name == QLatin1String("dd"))))
                popTo(stack.size() - 1);
            cur.html += t.raw;
            if (!isVoid(t.name)) stack.append({ t.name, t.raw });
            if (t.name == QLatin1String("img")) curHasContent = true;
            if (t.level > 0 && !headingOpen)
            {
                headingOpen = t.level;
                headingText.clear();
                headingNamesChapter = (split && t.level == split)
                                      || (split && inFront && t.level == titleLevel);
            }
        }
        closeAll();
        if (curHasContent || doc.chapters.isEmpty()) doc.chapters.append(cur);

        // One chapter is never titled: a contents panel with one row in it is furniture.
        if (doc.chapters.size() == 1) doc.chapters.first().title.clear();

        doc.title = !titleText.isEmpty() ? titleText : firstHeading;
        return doc;
    }
}
