// THE NATURAL-ORDER COLLATOR, in one place, because building it wrong is silent (issue #205).
//
// Four unrelated features order names the same way — comic pages (page1, page2, …, page10), photos in a
// folder (img1, img2, img10), the tracks of an untagged album (1 rip, 02 rip, 10 rip) and a merged music
// level — and all four build the same QCollator: numeric-aware, case-insensitive. This header exists so
// they build it through ONE constructor, because of the trap below.
//
// ---- THE TRAP: QCollator IS INERT UNDER THE C LOCALE ---------------------------------------------------
//
// A default-constructed QCollator collates in the DEFAULT locale, which is the system's. When that locale
// is the C/POSIX one, Qt opens no ICU collator at all — QCollatorPrivate::init() returns early for the C
// locale on every backend — and QCollator::compare() quietly degrades to QString::compare(). setNumericMode
// (true) is then accepted, still reads back as true, and does NOTHING: "page10" sorts before "page2", and
// nothing anywhere says so. There is no warning, no error and no failed call to notice.
//
// A machine has that locale whenever LANG / LC_ALL are unset — a kiosk or set-top session started by a
// display manager that scrubs the environment, a systemd unit with no locale, a bare console, and every
// GitHub Actions runner. That last one is how this was finally caught: probe_tar, probe_photos and
// probe_musiclibrary had been red on Linux CI since the day each was written, asserting an order the code
// could not produce there, while passing on every developer's Windows box (which has a real system locale).
//
// So: when the ambient locale is C, name a definite one instead. Any real locale agrees about the ordering
// of digits, which is the whole of what numeric mode needs, and a machine that HAS a locale keeps using its
// own — an accented title still sorts by its own language's rules, exactly as before.
#pragma once
#include <QCollator>
#include <QLocale>

namespace NaturalOrder
{
    // The locale to collate in: the ambient one, unless that is C (see the header note), in which case a
    // definite one. Separated from collator() so the substitution itself can be asserted.
    inline QLocale collationLocale()
    {
        const QLocale ambient;      // == QLocale::system() unless a caller set a default
        return ambient.language() == QLocale::C ? QLocale(QLocale::English, QLocale::UnitedStates)
                                                : ambient;
    }

    // Numeric-aware, case-insensitive — "page2" before "page10", "Page2" and "page2" folded together.
    inline QCollator collator()
    {
        QCollator c(collationLocale());
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }
}
