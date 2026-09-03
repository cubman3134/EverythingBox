#pragma once
// ReadAloudTarget — the narrow seam ReadAloudController drives, implemented by the reader (EbookView).
//
// It exists so the controller never reaches into a widget: everything narration needs from the reader is nine
// calls, and everything the reader needs from narration is that those nine mean what they say. A plain
// (non-QObject) interface, exactly like HostedReader next door — EbookView is already a QWidget and a second
// QObject base would be illegal.
//
// THE POSITION CONTRACT, which is the whole of issue #145's third decision. raShowSpoken() is not "scroll a
// bit": it is the reader ARRIVING at that paragraph — page turned if the paragraph is not on the current page,
// paragraph highlighted, and the position persisted through the reader's own store, the same store a page turn
// writes. So the spoken paragraph IS the reading position; there is no second bookmark system and no new mark
// kind, and raClearSpoken() drops only the highlight — never the place.
#include <QString>

class ReadAloudTarget
{
public:
    virtual ~ReadAloudTarget() = default;

    // The current chapter's text in DOCUMENT coordinates — QTextDocument::toPlainText(), whose offsets are the
    // offsets everything else in the reader positions with (topTextPosition, gotoSpineOffset, ReaderAnchor).
    virtual QString raChapterText() const = 0;
    virtual int     raChapterIndex() const = 0;
    virtual int     raChapterCount() const = 0;
    virtual bool    raGotoChapter(int index) = 0;   // load a chapter at its start; false when out of range

    // Where the reader is right now — the offset narration starts from when it is asked to begin "here".
    virtual int     raCurrentOffset() const = 0;

    // Arrive at [start, end): page to it, highlight it, persist it. See the position contract above.
    virtual void    raShowSpoken(int start, int end) = 0;
    // Narration stopped. Drop the highlight and KEEP the position — you are left on the page it reached.
    virtual void    raClearSpoken() = 0;

    // The book's stable natural key — the same one resume and bookmarks use, so #140's per-book speed memory
    // names one identity across a narrated book and its audiobook.
    virtual QString raBookKey() const = 0;
    // The language to prefer when offering voices (BCP-47 or a bare 2-letter code); empty means "no preference,
    // offer everything".
    virtual QString raPreferredLanguage() const = 0;
    // Narration's own state moved (started, stopped, paused, paragraph changed, speed/voice changed) — the
    // reader's chrome should re-read it. Not a signal: this interface is not a QObject.
    virtual void    raNarrationChanged() = 0;
};
