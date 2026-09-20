// Multi-disc grouping (issue #49). A user who drops "Final Fantasy VII (Disc 1).chd" … "(Disc 3).chd" into a
// system folder should see ONE library entry, not three, and it should launch through the existing .m3u disc-
// swap path (StreamResolver::looksLikeDiscPlaylist / probe_m3u) without hand-writing a playlist.
//
// This header is the PURE, testable heart: given a flat list of file names/paths it decides which ones are
// disc members of a common title, orders them, and produces the .m3u body — no disk I/O, no clock, no ini,
// QtCore only. The library-scan glue (RomLibrary::scan) does the I/O: it writes the generated .m3u into a
// cache dir (the user's ROM folder stays read-only), collapses each multi-disc set into one Rom, and hides
// the individual discs. Archived discs (inside a .zip/.7z) are deferred to a follow-up — loose files only.
//
// Normalisation mirrors GamelistStore's cleanTitle (drop every ()/[] tag) so a disc tag AND a region/rev tag
// are both stripped: "Final Fantasy VII (Disc 1) (USA)" and "… (Disc 2) (USA)" collapse to one key. Only
// files that actually CARRY a disc tag are ever grouped — a lone "Sonic (USA)" and "Sonic (Europe)" would
// normalise to the same key but must stay two separate games, so disc-number 0 never merges with anything.
#pragma once
#include <QChar>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <algorithm>

namespace DiscGroup
{
    // WHICH naming dialect the set is read in (issue #190, item 4).
    //
    //   Console  — exactly the forms #49 shipped: "(Disc 1)", "(Disk 2)", "(CD 3)", "[CD 4]". This is the
    //              default on every overload, so a console system's naming is untouched by construction.
    //   Computer — those PLUS the forms TOSEC uses for computer sets, which is how Amiga / C64 / Apple II
    //              / Atari ST / Amstrad CPC collections are actually named in the wild:
    //                  "(Disk 1 of 3)"  — the count comes after the number, so #49's regex (which wants the
    //                                     closing bracket straight after the digits) never matched it;
    //                  "(Side A)"       — a 5.25" floppy's two sides are two files of ONE disk.
    //
    // The dialect is opted into PER SYSTEM, by that system's launch recipe ("tosecDiskTags": true), and never
    // guessed from the file names. That is the gate #190 asks for: a console set that happens to be named
    // "Game (Disk 1 of 2).bin" groups exactly as it does today, because no console ships a recipe that turns
    // this on — and a user who wants it for their own system can turn it on in their own recipe override.
    enum class Dialect { Console, Computer };

    // A grouped set of disc members sharing one normalised title. A single-member set (a lone game, or a
    // disc set with only one disc present) has isMultiDisc == false and is passed through unchanged by the
    // glue; only isMultiDisc == true sets are collapsed into one .m3u library entry.
    struct DiscSet
    {
        QString          cleanTitle;  // readable title with the disc tag AND ()/[] region tags removed
        QVector<QString> members;     // member paths, ordered by disc number (then path, for determinism)
        bool             isMultiDisc = false;
    };

    // The disc number carried by a name's tag: "(Disc N)", "(Disk N)", "(CD N)", "[CD N]" — case-insensitive,
    // either bracket kind, N a positive integer. Returns that N, or 0 when the name carries no disc tag.
    // Operates on a file name, a base name, or a full path (the tag is matched wherever it appears).
    inline int discNumber(const QString& name, Dialect dialect = Dialect::Console)
    {
        // Open bracket, optional space, the keyword, at least one space, the digits, optional space, close
        // bracket. \b after the keyword would not fire before a space, so an explicit space class is used.
        static const QRegularExpression console(
            QStringLiteral("[\\(\\[]\\s*(?:disc|disk|cd)\\s+(\\d+)\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        // The same, plus TOSEC's "of M" count and the side that some collections fold into the SAME tag
        // ("(Disk 1 of 2 Side A)"). Both trailers are optional, so every console form still matches here.
        static const QRegularExpression computer(
            QStringLiteral("[\\(\\[]\\s*(?:disc|disk|cd)\\s+(\\d+)(?:\\s+of\\s+\\d+)?"
                           "(?:\\s+side\\s+[A-Za-z0-9]+)?\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m =
            (dialect == Dialect::Computer ? computer : console).match(name);
        if (!m.hasMatch()) return 0;
        bool ok = false;
        const int n = m.captured(1).toInt(&ok);
        return ok ? n : 0;
    }

    // The SIDE a name declares — "(Side A)" -> 1, "(Side B)" -> 2, "(Side 2)" -> 2 — or 0 when it declares
    // none. A 5.25" floppy carries two sides and TOSEC dumps them as two files, so the two sides of one disk
    // are two MEMBERS of one entry, not two entries. Also matches the folded spelling "(Disk 1 of 2 Side A)".
    // Zero in the Console dialect always: no console names a cartridge or a CD by side, and reading a side
    // where #49 read nothing is exactly the behaviour change the per-system gate exists to prevent.
    inline int sideOrdinal(const QString& name, Dialect dialect = Dialect::Console)
    {
        if (dialect != Dialect::Computer) return 0;
        static const QRegularExpression re(
            QStringLiteral("[\\(\\[](?:[^\\)\\]]*?\\s)?side\\s+([A-Za-z]|\\d+)\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(name);
        if (!m.hasMatch()) return 0;
        const QString cap = m.captured(1);
        bool ok = false;
        const int n = cap.toInt(&ok);
        if (ok) return n;                                  // "(Side 2)" -> 2
        return cap.at(0).toLower().unicode() - 'a' + 1;    // "(Side A)" -> 1, "(Side B)" -> 2
    }

    // The readable title with the disc tag AND every other ()/[] tag (region, revision, hack) removed, runs
    // of whitespace collapsed and trimmed. Any file extension on a name/path is dropped first. This mirrors
    // GamelistStore's cleanTitle stripping so grouping matches the same region/rev tags the gamelist match
    // does — e.g. "Final Fantasy VII (Disc 1) (USA).chd" -> "Final Fantasy VII".
    inline QString titleWithoutDiscTag(const QString& nameOrPath)
    {
        // Take just the base name so a directory named "(CD stuff)" upstream can't leak into the title.
        QString t = QFileInfo(nameOrPath).completeBaseName();
        static const QRegularExpression tags(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
        t.remove(tags);
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        t.replace(ws, QStringLiteral(" "));
        return t.trimmed();
    }

    // The grouping key: titleWithoutDiscTag folded to lowercase alphanumerics only, so punctuation/spacing
    // differences between disc names ("Final Fantasy VII" vs "final  fantasy vii") still collapse together.
    // Empty only if the title was all punctuation. This is the exact fold GamelistStore::cleanTitle uses.
    inline QString normalizedKey(const QString& nameOrPath)
    {
        const QString t = titleWithoutDiscTag(nameOrPath);
        QString out;
        out.reserve(t.size());
        for (const QChar c : t)
            if (c.isLetterOrNumber()) out += c.toLower();
        return out;
    }

    // Group a flat list of file names/paths into DiscSets. Files carrying a disc tag (discNumber >= 1) are
    // grouped by normalisedKey and their members ordered by disc number; every other file becomes its own
    // single-member set (isMultiDisc == false) so nothing without a disc tag is ever merged. Deterministic:
    // the returned vector is ordered by cleanTitle then first member path, and members within a set by disc
    // number then path — the same input always yields the same output (so a re-scan does not churn).
    // `dialect` decides WHICH tags count as a disc tag (see Dialect): the default is #49's console forms, and
    // a system whose recipe opts in also groups TOSEC's "(Disk N of M)" and "(Side A/B)".
    inline QVector<DiscSet> groupDiscs(const QVector<QString>& fileNamesOrPaths,
                                       Dialect dialect = Dialect::Console)
    {
        struct Member { int disc; int side; QString path; };
        QHash<QString, QVector<Member>> byKey;   // normalised key -> disc members (only for tagged files)
        QVector<Member>                 singles; // untagged files: each is its own set, never grouped

        for (const QString& path : fileNamesOrPaths)
        {
            const int n = discNumber(path, dialect);
            // A side tag alone makes a member too (a one-disk, two-sided title is named only "(Side A)" /
            // "(Side B)"), and such a file belongs to disk 1. In the Console dialect sideOrdinal is always 0,
            // so this reads exactly as `n >= 1` there.
            const int s = sideOrdinal(path, dialect);
            if (n >= 1 || s >= 1)
            {
                const QString key = normalizedKey(path);
                // A disc tag on a name that normalises to nothing (all-punctuation title) can't be grouped
                // meaningfully — treat it as a lone file rather than merging every such oddity together.
                if (key.isEmpty()) singles.push_back({ qMax(n, 1), s, path });
                else               byKey[key].push_back({ qMax(n, 1), s, path });
            }
            else
            {
                singles.push_back({ 0, 0, path });
            }
        }

        QVector<DiscSet> out;
        out.reserve(byKey.size() + singles.size());

        for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it)
        {
            QVector<Member> mem = it.value();
            std::sort(mem.begin(), mem.end(), [](const Member& a, const Member& b) {
                if (a.disc != b.disc) return a.disc < b.disc;
                // Then the SIDE, so a set with both reads disk 1 side A, disk 1 side B, disk 2 side A…
                // (zero for every console member, which leaves #49's ordering exactly as it was).
                if (a.side != b.side) return a.side < b.side;
                return a.path < b.path; // stable tie-break (two files claiming the same disc number)
            });
            DiscSet s;
            s.isMultiDisc = mem.size() >= 2;
            for (const Member& m : mem) s.members.push_back(m.path);
            s.cleanTitle = titleWithoutDiscTag(mem.front().path);
            out.push_back(std::move(s));
        }

        for (const Member& m : singles)
        {
            DiscSet s;
            s.isMultiDisc = false;
            s.members.push_back(m.path);
            s.cleanTitle = titleWithoutDiscTag(m.path);
            out.push_back(std::move(s));
        }

        std::sort(out.begin(), out.end(), [](const DiscSet& a, const DiscSet& b) {
            const int c = a.cleanTitle.compare(b.cleanTitle, Qt::CaseInsensitive);
            if (c != 0) return c < 0;
            const QString ap = a.members.isEmpty() ? QString() : a.members.front();
            const QString bp = b.members.isEmpty() ? QString() : b.members.front();
            return ap < bp;
        });
        return out;
    }

    // The .m3u body for a set: its member paths in disc order, one per line, trailing newline. This is the
    // shape StreamResolver::parseM3u accepts and looksLikeDiscPlaylist recognises as a disc set (a bare list
    // of disc-image paths, no #EXTM3U needed). The glue passes ABSOLUTE member paths so the cached playlist
    // resolves regardless of where it is written. Deterministic — identical members yield identical bytes.
    inline QString m3uContentFor(const DiscSet& set)
    {
        QString out;
        for (const QString& m : set.members)
        {
            out += m;
            out += QLatin1Char('\n');
        }
        return out;
    }
}
