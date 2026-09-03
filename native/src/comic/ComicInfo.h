// COMICINFO.XML — the comic world's metadata standard, read from inside the archive (issue #152).
//
// A CBZ is a bag of images and says nothing about itself, so #134 took a comic's series and issue number
// from its FILENAME (comic/ComicName), folder-corroborated and deliberately timid. ComicRack, Mylar, Komga,
// Kavita and every other tagger write a small XML document called ComicInfo.xml into the archive ROOT that
// says all of it outright. Filename parsing guesses; this tells. So when the document is there it WINS, and
// when it is not, nothing about the old behaviour changes — which is the compatibility rule the whole
// reading library is built on and the one this file is most easily wrong about.
//
// ---- WHERE IT IS READ, AND WHY IT COSTS NOTHING ----------------------------------------------------------
//
// In the SAME PASS the #134 scan already makes to count a comic's pages (core/BookMeta::readComic /
// readCbr). The archive is already open; ComicInfo.xml is one more member off a central directory that has
// already been walked. There is no second pass, no second open, and no per-file cost for an archive that
// carries no ComicInfo at all — the name is looked for in the member list that was being built anyway.
//
// ---- ONE PARSER, FOUR CONTAINERS -------------------------------------------------------------------------
//
// The DOCUMENT is container-agnostic: parse() takes bytes. The only per-format code is "hand me the bytes of
// the member called ComicInfo.xml", which is xmlFromArchive() — CBZ through miniz, CBR through the unarr
// reader (RarComic), CB7 through the vendored LZMA SDK (SevenZip) and CBT through the in-tree tar reader
// (Tar.h). Adding a fifth container would add one branch there and nothing else.
//
// NOTE WHAT THAT DOES NOT CHANGE: which files the LIBRARY SCAN claims. BookLibrary.h refuses .cb7 and .cbt
// because reaching page one costs a whole-archive extraction, and that cost has not moved. This seam reads
// all four so the READER (ComicView, which already opens all four) and any later caller get the same answer
// from the same code; the scan's extension gate is untouched, and probe_books still pins it.
//
// ---- THE NAME MATCH IS EXACT, CASE-INSENSITIVE, AND ROOT-ONLY ----------------------------------------------
//
// `ComicInfo.xml`, `comicinfo.xml` and `COMICINFO.XML` are the same member — taggers disagree about the case
// and a zip is case-sensitive. But a member called `extras/ComicInfo.xml` or `__MACOSX/ComicInfo.xml` is NOT
// one: the standard puts it at the archive root, a nested copy belongs to something else in the archive, and
// the __MACOSX shadow every Mac-built archive carries would otherwise be read as the comic's own metadata
// (the same trap ComicPageOrder.h documents for the COVER, arriving through a second door). First match at
// the root wins; the order is the archive's own.
//
// ---- A DOCUMENT THAT WILL NOT PARSE IS NOT A FAILURE -------------------------------------------------------
//
// It is logged ONCE and ignored, and the comic keeps everything the filename told #134 about it. A shelf
// that dropped a comic because its metadata was truncated would be losing exactly the files most likely to
// have been hand-edited. Same for a field this file does not know, a field with junk in it, and the per-page
// <Pages> block, which is read past without a word.
//
// WORKER-THREAD SAFE: file I/O and QXmlStreamReader, no Settings, no widgets, no static state. The #134 scan
// calls straight in from its worker thread.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ComicInfo
{
    // ---- THE AGE LADDER ------------------------------------------------------------------------------------
    // ComicRack's AgeRating is FREE TEXT with a conventional vocabulary of fifteen values, three national
    // rating systems deep and not ordered by anything in the format itself. The app has no use for fifteen
    // rungs, so they are collapsed onto the five below plus "we were not told" — and the collapse is the
    // documented table in ratingFor(), not a prefix match.
    //
    // Unrated IS ITS OWN RUNG AND IS NOT "Everyone". An AgeRating this file does not recognise, an empty one,
    // "Unknown" and "Rating Pending" all land here — never on a rung that would let a parental gate certify a
    // comic nobody rated. What a gate then DOES with Unrated is the gate's decision (see
    // hiddenWhenRestricted), not the parser's.
    enum class Rating
    {
        Unrated = 0,   // absent, "Unknown", "Rating Pending", or a value not in the table
        Everyone,      // Early Childhood, Everyone, G, Kids to Adults
        Everyone10,    // Everyone 10+, PG
        Teen,          // Teen
        Mature,        // MA15+, Mature 17+, M
        Adults         // R18+, Adults Only 18+, X18+
    };

    // The <Manga> field's reading direction. Unspecified is the honest answer for a comic that does not say —
    // it is NOT LeftToRight, because "the file did not say" and "the file said left to right" have to be
    // distinguishable for a user override to be able to lose to the second and win over the first.
    enum class Direction { Unspecified = 0, LeftToRight, RightToLeft };

    // ---- WHAT ONE ComicInfo.xml SAID -------------------------------------------------------------------------
    // Every field is empty / zero / Unrated / Unspecified when the document did not carry it. Nothing here is
    // ever inferred from a filename: that fallback belongs to BookLibrary, which is the layer that knows what
    // the file is called and can mark the result as having come from the name.
    struct Info
    {
        QString series;      // <Series>
        QString number;      // <Number>, VERBATIM — "1", "1.5", "Annual 1", "½". Never parsed into a double
                             // and thrown away: numberAsIndex() derives the sortable half beside it.
        int     volume = 0;  // <Volume> — the collected-edition volume, not the issue
        QString title;       // <Title> — the STORY's title; most issues carry none
        QString summary;     // <Summary>
        int     year = 0;    // <Year> / <Month> / <Day>: the cover date, in three fields, any of which can
        int     month = 0;   // stand alone. A year with no month is normal and is not a broken date.
        int     day = 0;

        // THE CREATORS, COLLAPSED. Six role fields (<Writer>, <Penciller>, <Inker>, <Colorist>, <Letterer>,
        // <CoverArtist>), each a comma-separated list of names, folded into ONE ordered list with duplicates
        // removed — a comic whose writer also drew it names that person once. Role order is the order above,
        // which is why `author` (the first writer) is the primary credit a shelf shows.
        QStringList creators;
        QString     author;  // the FIRST <Writer>. Empty when the document named no writer.

        QString publisher;   // <Publisher>
        QString genre;       // <Genre>
        QString language;    // <LanguageISO>
        QString web;         // <Web>
        int     pageCount = 0;  // <PageCount> — what the document CLAIMS. The scan prefers what it counted.

        Rating    rating    = Rating::Unrated;      // <AgeRating>, through ratingFor()
        Direction direction = Direction::Unspecified;   // <Manga>, through directionFor()

        // "The document said nothing a shelf could use." Deliberately not about pageCount, rating or
        // direction: a ComicInfo carrying only <PageCount>21</PageCount> still leaves the shelf showing the
        // filename, and this flag is what tells BookLibrary to keep doing that.
        bool isEmpty() const
        {
            return series.isEmpty() && number.isEmpty() && title.isEmpty() && author.isEmpty()
                && creators.isEmpty() && summary.isEmpty() && publisher.isEmpty() && genre.isEmpty()
                && volume == 0 && year == 0;
        }
    };

    // ---- The document --------------------------------------------------------------------------------------

    // Parse one ComicInfo.xml. `wellFormed`, when given, reports whether the XML parsed to the end without an
    // error — a truncated or malformed document yields whatever was read before the damage AND false, so a
    // caller can log it once and go on using the filename. A document whose root element is not <ComicInfo>
    // is not this format: it parses well-formed and yields an EMPTY Info rather than fields scraped out of
    // somebody else's XML.
    Info parse(const QByteArray& xml, bool* wellFormed = nullptr);

    // "Is this archive member the comic's own ComicInfo.xml": case-insensitive, ROOT ONLY (see the header).
    // A leading "./" is tolerated because some writers emit one; anything with a further separator is not it.
    bool isComicInfoName(const QString& memberName);

    // THE ARCHIVE SEAM. The bytes of the archive's root ComicInfo.xml, or empty when it has none / cannot be
    // read. Dispatches on the EXTENSION (.cbz/.zip, .cbr, .cb7, .cbt) exactly as ComicView::openComic does,
    // and never sniffs contents.
    //
    // COST, PER CONTAINER, BECAUSE IT IS NOT THE SAME COST: a zip is random access and this is one member; a
    // tar is a header walk of a buffer already in memory; a RAR needs ONE sequential decompression pass (its
    // solid mode makes an out-of-order read a restart, RarComic.h says why), and only when the header walk
    // has already established the member is there; a 7z is a whole-archive decode. The two expensive ones are
    // exactly the two containers the library scan does not claim.
    QByteArray xmlFromArchive(const QString& archivePath);

    // The same hunt over a directory that ALREADY holds an archive's extracted contents — root only, same
    // case-insensitive name rule. It exists for the .cb7 reader, which unpacks the archive to a temp dir to
    // reach its pages at all: reading the document out of that extraction costs nothing, where coming back
    // through xmlFromArchive() would decode the whole archive a second time.
    QByteArray xmlFromDirectory(const QString& dir);

    // Read + parse in one call, for a caller holding only a path (the reader). Empty Info when there is none.
    Info readArchive(const QString& archivePath);

    // ---- The tables ------------------------------------------------------------------------------------------

    // <AgeRating> -> Rating. The documented collapse; see Rating above and the table in the .cpp. Matching is
    // case-insensitive and whitespace-insensitive and otherwise EXACT — never a prefix or a "contains", so a
    // rating this file has not heard of stays Unrated instead of being pattern-matched onto a rung.
    Rating ratingFor(const QString& ageRating);

    // <Manga> -> Direction. "YesAndRightToLeft" is right to left; "Yes" and "No" are manga-ness rather than
    // direction and leave it Unspecified (a left-to-right manga is a real thing and the format has a value
    // for it: "YesAndLeftToRight"). Anything else is Unspecified.
    Direction directionFor(const QString& manga);

    // WHAT A RESTRICTED (kids) PROFILE HIDES: Mature and Adults, and nothing else.
    //
    // UNRATED IS SHOWN, and that is a decision rather than an oversight. Every comic in every library that
    // exists today is Unrated — this field has only just started being read — so hiding it would empty the
    // shelf of a kids profile the first time this build ran, which is a far worse failure than showing an
    // untagged comic that was already showing yesterday. The parser's job is that Unrated is never CONFUSED
    // with Everyone (a comic nobody rated is never certified as rated for kids); the gate's job is to hide
    // what is positively known to be adult. MA15+ is collapsed into Mature rather than Teen for the same
    // reason the two halves point the same way: when a rating straddles a rung, a parental gate takes the
    // higher one.
    bool hiddenWhenRestricted(Rating r);

    // ---- Precedence ------------------------------------------------------------------------------------------

    // THE READING DIRECTION A COMIC ACTUALLY OPENS IN. The user's per-series override wins outright when it
    // is set; the document's <Manga> is the DEFAULT under it; and with neither, left to right — the direction
    // the reader has always used, so a library with no ComicInfo anywhere reads exactly as it did.
    Direction resolveDirection(Direction embedded, Direction userOverride);

    // The sortable half of <Number>. The leading decimal of "1", "1.5", "0.5" and "3a" (3), or 0 for a number
    // that starts with no digit at all ("Annual 1", "Special") — 0 being the same "unnumbered, sorts last"
    // value BookLibrary::Book::seriesIndex already uses. The RAW string is kept beside it and is what breaks
    // the tie between two zeroes, in natural order, so "Annual 1" precedes "Annual 2" and neither displaces
    // issue 1.
    double numberAsIndex(const QString& number);
}
