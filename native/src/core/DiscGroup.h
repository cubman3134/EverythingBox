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
    inline int discNumber(const QString& name)
    {
        // Open bracket, optional space, the keyword, at least one space, the digits, optional space, close
        // bracket. \b after the keyword would not fire before a space, so an explicit space class is used.
        static const QRegularExpression re(
            QStringLiteral("[\\(\\[]\\s*(?:disc|disk|cd)\\s+(\\d+)\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(name);
        if (!m.hasMatch()) return 0;
        bool ok = false;
        const int n = m.captured(1).toInt(&ok);
        return ok ? n : 0;
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
    inline QVector<DiscSet> groupDiscs(const QVector<QString>& fileNamesOrPaths)
    {
        struct Member { int disc; QString path; };
        QHash<QString, QVector<Member>> byKey;   // normalised key -> disc members (only for tagged files)
        QVector<Member>                 singles; // untagged files: each is its own set, never grouped

        for (const QString& path : fileNamesOrPaths)
        {
            const int n = discNumber(path);
            if (n >= 1)
            {
                const QString key = normalizedKey(path);
                // A disc tag on a name that normalises to nothing (all-punctuation title) can't be grouped
                // meaningfully — treat it as a lone file rather than merging every such oddity together.
                if (key.isEmpty()) singles.push_back({ n, path });
                else               byKey[key].push_back({ n, path });
            }
            else
            {
                singles.push_back({ 0, path });
            }
        }

        QVector<DiscSet> out;
        out.reserve(byKey.size() + singles.size());

        for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it)
        {
            QVector<Member> mem = it.value();
            std::sort(mem.begin(), mem.end(), [](const Member& a, const Member& b) {
                if (a.disc != b.disc) return a.disc < b.disc;
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
