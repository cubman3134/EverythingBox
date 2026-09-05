// ReaderSelection — the reader's D-PAD-FIRST text selection, as a pure decision table (issue #136). A caret
// offset, a text buffer and the current page's line starts go in; a new caret, a selection range and what the
// host should do next come out. No QWidget, no QTextDocument, no Settings — which is what lets
// probe_highlights drive the whole key map headlessly, including the cases a finger can never reach.
//
// WHY A CURSOR MODE. This app is read on a television with a pad, and there is no pointer to sweep across a
// paragraph with. The issue asks for exactly this: "D-pad-friendly selection needs a cursor mode, Osk-style
// deliberateness rather than touch-only". So selection is a MODE you enter on purpose, not a drag you can
// start by accident: the caret is visible, it moves in units you can see (a word, a line), and the selection
// only begins when you say so.
//
// THE KEY MAP, in full — one press, one meaning, and the same four keys do the moving and the extending:
//
//   Left  / Right   caret to the previous / next WORD start (the unit a reader thinks in; a character step on
//                   a pad is a hundred presses across a paragraph)
//   Up    / Down    caret to the same column on the previous / next LINE (clamped inside that line)
//   Enter           1st press: drop the selection anchor HERE (select-from-here). 2nd press: COMMIT the
//                   selection — the host then offers the colour menu. So the whole gesture is
//                   enter-mode → move → Enter → move → Enter, every step visible and undoable.
//   Escape / Back   cancel: with a selection in flight it drops the selection and keeps the caret; with no
//                   selection it leaves cursor mode entirely (so Back never strands you inside the mode).
//
// THE RANGE THE COMMIT YIELDS IS TRIMMED. Moving right by word leaves the caret on the FIRST character of the
// next word, so the raw span carries the space (and any punctuation) between the two — a highlight that draws
// a trailing gap and stores an excerpt with a hanging space. trimmedRange() shrinks the span to the text
// inside it, which is what a reader pointed at.
//
// A committed range becomes a ReaderAnchor with its RESERVED endOffset filled in (ReaderAnchor.h: "a highlight
// is simply an anchor with endOffset >= 0"). There is no second position model.
#pragma once
#include <QChar>
#include <QString>
#include <QVector>
#include <Qt>

#include "ReaderAnchor.h"

namespace ReaderSelection
{

// What counts as being INSIDE a word. Letters and digits obviously; an apostrophe (both the typewriter one and
// the typographic one real books use) so "don't" and "O'Brien" are one word rather than three.
inline bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('\'') || c == QChar(0x2019);
}

// The start of the word AFTER `pos`: step off the word we are in, then over whatever separates it from the
// next one. At (or past) the end of the text the answer is the end of the text — a caret may sit there.
inline int nextWordStart(const QString& t, int pos)
{
    const int n = t.size();
    int i = qBound(0, pos, n);
    while (i < n && isWordChar(t.at(i))) ++i;
    while (i < n && !isWordChar(t.at(i))) ++i;
    return i;
}

// The start of the word BEFORE `pos` (or of the word `pos` sits inside, when it is not already at its start).
inline int prevWordStart(const QString& t, int pos)
{
    int i = qBound(0, pos, t.size());
    if (i <= 0) return 0;
    --i;
    while (i > 0 && !isWordChar(t.at(i))) --i;
    while (i > 0 && isWordChar(t.at(i - 1))) --i;
    return i;
}

// One caret step by word. dir > 0 forward, dir < 0 back, 0 is a no-op.
inline int moveWord(const QString& t, int caret, int dir)
{
    if (dir > 0) return nextWordStart(t, caret);
    if (dir < 0) return prevWordStart(t, caret);
    return qBound(0, caret, t.size());
}

// One caret step by line, keeping the COLUMN (the offset within the line) — the movement a reader expects from
// up/down anywhere else. `lineStarts` is the ascending list of line-start offsets the layout produced; at the
// first/last line the caret does not move, so the mode never falls off the ends of the chapter.
inline int moveLine(const QVector<int>& lineStarts, int textLen, int caret, int dir)
{
    const int n = lineStarts.size();
    if (n == 0 || dir == 0) return qBound(0, caret, textLen);
    const int c = qBound(0, caret, textLen);

    int idx = 0;                                  // the last line start at or before the caret
    for (int i = 0; i < n; ++i)
    {
        if (lineStarts.at(i) <= c) idx = i;
        else break;
    }
    const int j = idx + (dir > 0 ? 1 : -1);
    if (j < 0 || j >= n) return c;                // clamped at the ends, never wrapped

    const int col = c - lineStarts.at(idx);
    // Where this line ends. A line that is followed by another ends one before that one starts, so a long
    // column lands on the line's LAST character rather than on the first character of the line below it.
    const int lineEnd = (j + 1 < n) ? qMax(lineStarts.at(j), lineStarts.at(j + 1) - 1)
                                    : qMax(lineStarts.at(j), textLen);
    return qBound(lineStarts.at(j), lineStarts.at(j) + col, lineEnd);
}

// Shrink [start,end) to the text inside it: a word-wise extension leaves the caret on the first character of
// the NEXT word, so the raw span carries the separator before it. Returns an EMPTY range (start == end) when
// there is nothing but whitespace in the span.
inline void trimRange(const QString& t, int& start, int& end)
{
    const int n = t.size();
    start = qBound(0, start, n);
    end   = qBound(0, end, n);
    if (end < start) qSwap(start, end);
    while (start < end && t.at(start).isSpace()) ++start;
    while (end > start && t.at(end - 1).isSpace()) --end;
}

// What a key press did, so the host can react without re-deriving the state machine.
enum class Result
{
    Ignored,    // not one of ours — the host may still use the key (paging, chrome, …)
    Moved,      // the caret (and, while selecting, the selection) moved
    Anchored,   // select-from-here: the selection just began at the caret
    Committed,  // the selection is finished — the host offers the colour menu
    Cleared,    // a selection in flight was dropped; the mode is still on, the caret is still there
    Exited,     // cursor mode is over
};

// The cursor-mode state machine itself. `active` is the mode; `anchor` is where select-from-here happened
// (-1 = no selection in flight). Everything below is a pure function of those three ints plus the key.
struct Model
{
    bool active = false;
    int  caret  = 0;
    int  anchor = -1;

    bool hasSelection() const { return active && anchor >= 0 && anchor != caret; }
    int  selStart() const { return hasSelection() ? qMin(anchor, caret) : caret; }
    int  selEnd()   const { return hasSelection() ? qMax(anchor, caret) : caret; }
    bool selecting() const { return active && anchor >= 0; }

    void enter(int at) { active = true; caret = qMax(0, at); anchor = -1; }
    void leave()       { active = false; anchor = -1; }

    // The whole key map (see the header comment). `text` is the chapter's plain text in the SAME coordinates
    // the anchor's offsets use, and `lineStarts` the current layout's line starts.
    Result key(int qtKey, const QString& text, const QVector<int>& lineStarts)
    {
        if (!active) return Result::Ignored;
        switch (qtKey)
        {
        case Qt::Key_Left:  caret = moveWord(text, caret, -1); return Result::Moved;
        case Qt::Key_Right: caret = moveWord(text, caret, +1); return Result::Moved;
        case Qt::Key_Up:    caret = moveLine(lineStarts, text.size(), caret, -1); return Result::Moved;
        case Qt::Key_Down:  caret = moveLine(lineStarts, text.size(), caret, +1); return Result::Moved;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Select:
            if (anchor < 0) { anchor = caret; return Result::Anchored; }
            return Result::Committed;
        case Qt::Key_Escape:
        case Qt::Key_Back:
        case Qt::Key_Backspace:
            if (anchor >= 0) { anchor = -1; return Result::Cleared; }
            leave();
            return Result::Exited;
        default:
            return Result::Ignored;
        }
    }

    // The committed selection as a book range anchor — the SAME ReaderAnchor a bookmark uses, with its
    // reserved endOffset filled in. Returns a POINT anchor (endOffset -1) when the trimmed span is empty, so a
    // caller that stores it unconditionally stores a bookmark-shaped anchor rather than a zero-width highlight.
    ReaderAnchor toAnchor(int spine, const QString& text) const
    {
        ReaderAnchor a;
        a.kind  = ReaderAnchor::Book;
        a.spine = spine;
        int s = selStart(), e = selEnd();
        trimRange(text, s, e);
        a.offset = s;
        a.endOffset = (e > s) ? e : -1;
        return a;
    }
};

// The trimmed text a range covers — the excerpt the annotation panel lists a highlight by, taken from the
// chapter text rather than stored twice. Whitespace is SIMPLIFIED, because a passage that runs across a
// paragraph (or takes a heading with it) carries the newlines between them, and a panel row is one line: left
// alone they render as a row that is mostly empty space with the words pushed off the end of it.
inline QString textOf(const QString& text, const ReaderAnchor& a)
{
    if (!a.isRange()) return QString();
    int s = a.offset, e = a.endOffset;
    trimRange(text, s, e);
    return (e > s) ? text.mid(s, e - s).simplified() : QString();
}

} // namespace ReaderSelection
