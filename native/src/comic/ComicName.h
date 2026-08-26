// WHAT A COMIC'S FILENAME IS ALLOWED TO MEAN (issue #134) — the series-grouping rule for files that carry
// no metadata at all, as pure functions.
//
// Comics in a folder have no standard metadata in scope here (ComicInfo.xml is #152 and explicitly not this
// increment's), so the only thing a scan can read is the name. That makes this a NORMALISER, and this
// repository has a specific and repeated history with those: a romhack search matched Final Fantasy IV and
// VI for a Final Fantasy V query; a release parser matched the word "Web" inside *Charlotte's Web* and
// misfiled it; a format rule nearly dropped a real audiobook because its title contained a format word.
// Every one of them was a pattern that matched MORE THAN IT MEANT, and every one of them failed silently.
//
// So the governing rule of this file is stated once, here, and everything below is an application of it:
//
//     A COMIC LEFT UNGROUPED IS A CHEAP FAILURE. A COMIC FILED UNDER THE WRONG SERIES IS AN EXPENSIVE ONE.
//
// An ungrouped comic still appears on the shelf, under its own name, and still opens. A misgrouped one is
// hidden inside somebody else's series, where the person looking for it will not think to look and where
// nothing on screen says what happened. Every judgement below is therefore taken in the direction of doing
// nothing.
//
// ---- THE TWO TIERS, AND WHY THERE ARE TWO -----------------------------------------------------------------
//
// parse() reads ONE name and grades what it found:
//
//   * MARKED — the name contains an explicit statement that a number is an issue or volume number: a `#`,
//     the word `Vol`/`Volume`, a `v01`-style volume token, or a number standing alone as its own
//     dash-delimited field ("Series - 012 - Title"). A marker is somebody writing down what they meant, so
//     it is believed on its own.
//   * BARE — a plain trailing number ("Saga 012"). This is the shape that is right most of the time and
//     catastrophically wrong the rest of it, because *Fahrenheit 451*, *Catch 22*, *Area 51* and *Apollo 11*
//     are titles whose last word is a number that is not an issue number. A bare number is NOT believed on
//     its own.
//   * NONE — no number this file is willing to read as one.
//
// group() then supplies the missing evidence for the BARE case from the one place it can honestly come
// from: THE FOLDER. A bare number becomes a series only when at least one OTHER file in the same folder
// shares the same series prefix. One `Fahrenheit 451.cbz` sitting alone stays ungrouped and shows up as
// itself; a run of `Saga 001.cbz`…`Saga 054.cbz` is fifty-four pieces of evidence that "Saga" is a series
// and is grouped. That is the whole trick, and it is the reason this header has two functions instead of one.
//
// ---- THE SMALLER REFUSALS, each of which exists because of a real filename -----------------------------
//
//   * A FOUR-DIGIT NUMBER IN 1900..2099 IS A YEAR, NOT AN ISSUE. `Watchmen 1986.cbz` is not issue 1986.
//     This costs almost nothing in the other direction, because the handful of comics that ever reached a
//     four-digit issue (Action Comics #1000, Detective Comics #1027) are numbered outside that window.
//   * A SERIES NAME MUST CONTAIN A LETTER. Without it `01 - 02.cbz` becomes the series "01".
//   * ONLY TRAILING BRACKETED GROUPS ARE STRIPPED. `(2013) (Digital) (Zone-Empire)` at the end of a name is
//     release furniture; `Batman (Earth-Two)` in the middle of one is part of the title. Stripping interior
//     brackets would merge two different Batmen.
//   * NOTHING IS STRIPPED IF IT WOULD EMPTY THE NAME. A file called `(2013).cbz` keeps its name.
//   * PUNCTUATION IS LEFT ALONE otherwise. Folding "Spider-Man" to "spiderman" would make it match
//     "Spider Man" — which is usually right and is exactly the kind of "usually" this file refuses. Only
//     case and whitespace are folded for the grouping key.
//
// ---- WHAT THIS FILE IS NOT ------------------------------------------------------------------------------
//
// It never reads a file, never opens an archive, and never decides which files are comics — BookLibrary
// does that from the extension. It is handed names and returns judgements, which is what lets probe_books
// pin every rule above and mutation-test the grouping in both directions (a threshold that groups too
// eagerly and one that stops grouping must both die).
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace ComicName
{
    // How much the name itself said. See the header — this grade is the whole reason group() exists.
    enum class Evidence
    {
        None,     // no number here is an issue number
        Bare,     // a trailing number with no marker: believed only with a corroborating sibling
        Marked,   // an explicit #, Vol/v01, or a dash-delimited numeric field: believed on its own
    };

    struct Parsed
    {
        QString  series;             // "" unless evidence != None
        double   number = 0.0;       // the issue/volume number; 0 == none found
        QString  title;              // the part after the number, when the shape carried one; often ""
        QString  cleaned;            // the name with trailing bracket groups removed — NEVER empty
        Evidence evidence = Evidence::None;
    };

    // One name, graded. `baseName` is the file name WITHOUT its extension.
    Parsed parse(const QString& baseName);

    // The grouping key for a series name: case- and whitespace-folded, and nothing else (see the header on
    // why punctuation is left alone). Exposed because the index keys on it and the probe asserts on it.
    QString seriesKey(const QString& series);

    // What the shelf actually does with each file. `baseNames` are the comic files of ONE FOLDER, and the
    // result is one entry per input IN THE SAME ORDER.
    struct Grouped
    {
        QString series;          // "" == ungrouped: this file stands on its own
        double  number = 0.0;    // its place in that series; 0 == unnumbered
        QString title;           // what to call this file on screen — NEVER empty
    };
    QVector<Grouped> group(const QStringList& baseNames);

    // How many files in a folder must agree before a BARE trailing number is read as an issue number. Two:
    // this file plus one corroborating sibling. Named rather than inlined because it is the single value
    // that decides how eager this whole feature is, and because the probe mutates it in both directions —
    // 1 (group everything, so a lone *Fahrenheit 451* becomes the series "Fahrenheit") and 3 (group almost
    // nothing, so a two-issue run stops grouping) must each fail the suite.
    inline constexpr int kBareCorroboration = 2;
}
