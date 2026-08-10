// Preferred-region selection + 1G1R-style duplicate collapsing (issue #50). A folder holding
// "Game (USA).sfc", "Game (Europe).sfc" and "Game (Japan).sfc" is three near-identical library entries; a
// user who follows a No-Intro naming scheme wants ONE entry — the best variant by a region preference — with
// the others still reachable, nothing deleted. This header is the PURE, testable heart: given a flat list of
// file names/paths and an ordered region priority it groups same-title variants and picks the winner. No disk
// I/O, no clock, no ini — QtCore only. The library-scan glue (RomLibrary::scan) does the I/O and the hiding.
//
// It runs as a SECOND pass AFTER #49's multi-disc collapse, over the same file set, and REUSES DiscGroup's
// normalisation (normalizedKey / titleWithoutDiscTag) so region grouping keys IDENTICALLY to disc grouping
// and to GamelistStore::cleanTitle. A multi-disc set's collapsed entry has a region-less title, so it forms
// its own single-member group here and is passed through untouched — the two passes compose.
//
// ---- Region vocabulary (documented) ----------------------------------------------------------------------
// A region is read from a name's parenthesised/bracketed tags. Both No-Intro long names and the widely-used
// GoodTools single-letter codes are recognised; a tag may carry several comma/plus/ampersand/slash-separated
// tokens ("(USA, Europe)") and each is mapped. The canonical regions and their accepted synonyms are:
//   USA        <- usa, us, u, america, ntsc-u, ntscu
//   Europe     <- europe, eur, eu, e, pal
//   Japan      <- japan, jpn, jp, j, ntsc-j, ntscj
//   World      <- world, w
//   Korea      <- korea, kor, k
//   Asia       <- asia
//   Australia  <- australia, aus
//   Brazil     <- brazil, bra
//   China      <- china, chn
//   France     <- france, fra
//   Germany    <- germany, ger
//   Italy      <- italy, ita
//   Spain      <- spain, spa
//   Netherlands<- netherlands
//   Canada     <- canada
//   Sweden     <- sweden
// Single letters are deliberately limited to U/E/J/W/K — the established GoodTools primary-region codes.
// Ambiguous single letters (A/B/F/G/I/S) are NOT taken as regions: they collide with revision letters
// ("(Rev A)", "(Rev B)") and with language codes, and a false region would silently change which variant
// wins. Those regions are still recognised through their multi-letter forms. Revision/version/beta/proto/
// disc tags are skipped entirely when scanning for regions, so "(Rev A)" never reads as a region.
#pragma once
#include "DiscGroup.h"

#include <QChar>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>

namespace RegionCollapse
{
    // token (lowercased) -> canonical region name. See the header comment for the full documented vocabulary.
    inline const QHash<QString, QString>& regionSynonyms()
    {
        static const QHash<QString, QString> m = {
            { QStringLiteral("usa"), QStringLiteral("USA") },
            { QStringLiteral("us"), QStringLiteral("USA") },
            { QStringLiteral("u"), QStringLiteral("USA") },
            { QStringLiteral("america"), QStringLiteral("USA") },
            { QStringLiteral("ntsc-u"), QStringLiteral("USA") },
            { QStringLiteral("ntscu"), QStringLiteral("USA") },
            { QStringLiteral("europe"), QStringLiteral("Europe") },
            { QStringLiteral("eur"), QStringLiteral("Europe") },
            { QStringLiteral("eu"), QStringLiteral("Europe") },
            { QStringLiteral("e"), QStringLiteral("Europe") },
            { QStringLiteral("pal"), QStringLiteral("Europe") },
            { QStringLiteral("japan"), QStringLiteral("Japan") },
            { QStringLiteral("jpn"), QStringLiteral("Japan") },
            { QStringLiteral("jp"), QStringLiteral("Japan") },
            { QStringLiteral("j"), QStringLiteral("Japan") },
            { QStringLiteral("ntsc-j"), QStringLiteral("Japan") },
            { QStringLiteral("ntscj"), QStringLiteral("Japan") },
            { QStringLiteral("world"), QStringLiteral("World") },
            { QStringLiteral("w"), QStringLiteral("World") },
            { QStringLiteral("korea"), QStringLiteral("Korea") },
            { QStringLiteral("kor"), QStringLiteral("Korea") },
            { QStringLiteral("k"), QStringLiteral("Korea") },
            { QStringLiteral("asia"), QStringLiteral("Asia") },
            { QStringLiteral("australia"), QStringLiteral("Australia") },
            { QStringLiteral("aus"), QStringLiteral("Australia") },
            { QStringLiteral("brazil"), QStringLiteral("Brazil") },
            { QStringLiteral("bra"), QStringLiteral("Brazil") },
            { QStringLiteral("china"), QStringLiteral("China") },
            { QStringLiteral("chn"), QStringLiteral("China") },
            { QStringLiteral("france"), QStringLiteral("France") },
            { QStringLiteral("fra"), QStringLiteral("France") },
            { QStringLiteral("germany"), QStringLiteral("Germany") },
            { QStringLiteral("ger"), QStringLiteral("Germany") },
            { QStringLiteral("italy"), QStringLiteral("Italy") },
            { QStringLiteral("ita"), QStringLiteral("Italy") },
            { QStringLiteral("spain"), QStringLiteral("Spain") },
            { QStringLiteral("spa"), QStringLiteral("Spain") },
            { QStringLiteral("netherlands"), QStringLiteral("Netherlands") },
            { QStringLiteral("canada"), QStringLiteral("Canada") },
            { QStringLiteral("sweden"), QStringLiteral("Sweden") },
        };
        return m;
    }

    // True when a tag's content is a revision / version / beta / proto / disc marker rather than a region —
    // those are skipped when scanning for regions so "(Rev A)" is never mistaken for the Asia/Australia code.
    inline bool looksLikeNonRegionTag(const QString& tagContent)
    {
        static const QRegularExpression re(
            QStringLiteral("^\\s*(?:rev\\b|v\\d|beta\\b|proto\\b|sample\\b|demo\\b|disc\\b|disk\\b|cd\\b)"),
            QRegularExpression::CaseInsensitiveOption);
        return re.match(tagContent).hasMatch();
    }

    // The set of canonical regions a name declares across all its ()/[] tags (empty if none recognised).
    inline QSet<QString> regionsIn(const QString& nameOrPath)
    {
        QSet<QString> out;
        // Match each ()/[] tag body. Content is split on the separators No-Intro/GoodTools use between
        // multiple regions/languages in one tag.
        static const QRegularExpression tag(QStringLiteral("[\\(\\[]([^\\)\\]]*)[\\)\\]]"));
        static const QRegularExpression sep(QStringLiteral("[,+&/]"));
        auto it = tag.globalMatch(nameOrPath);
        while (it.hasNext())
        {
            const QString body = it.next().captured(1);
            if (looksLikeNonRegionTag(body)) continue; // a revision/version/etc. tag carries no region
            for (QString tok : body.split(sep, Qt::SkipEmptyParts))
            {
                tok = tok.trimmed().toLower();
                const QString canon = regionSynonyms().value(tok);
                if (!canon.isEmpty()) out.insert(canon);
            }
        }
        return out;
    }

    // The region rank of a name against an ordered priority list: the index (0-based, lower = more preferred)
    // of the BEST-ranked region the name declares that appears in `priority`. A name that declares no region
    // in the priority list — whether it declares none at all, or only regions the user did not list — ranks
    // AFTER every listed region (returns priority.size()), but is still a candidate (never dropped for it).
    inline int regionRank(const QString& nameOrPath, const QStringList& priority)
    {
        const QSet<QString> regions = regionsIn(nameOrPath);
        int best = priority.size(); // "after all listed" until a listed region is found
        for (int i = 0; i < priority.size(); ++i)
            if (regions.contains(priority[i])) { best = i; break; } // priority is ordered, first hit is best
        return best;
    }

    // The revision ordinal a name carries, higher = newer, 0 when none. Precedence: a No-Intro "(Rev N)" /
    // "(Rev A)" tag first (numeric N as-is; a letter A/B/… as its 1-based ordinal, so Rev A == 1, Rev B == 2),
    // else a "(vMAJOR[.MINOR])" version tag (encoded MAJOR*1000+MINOR so v1.1 > v1.0 > v1). Absent => 0, which
    // sorts below any real revision — an untagged first print loses a tie to a "(Rev 1)" of the same region.
    inline int revisionOf(const QString& nameOrPath)
    {
        static const QRegularExpression rev(
            QStringLiteral("[\\(\\[]\\s*rev\\s*(\\d+|[A-Za-z])\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch rm = rev.match(nameOrPath);
        if (rm.hasMatch())
        {
            const QString cap = rm.captured(1);
            bool ok = false;
            const int n = cap.toInt(&ok);
            if (ok) return n;                                   // "(Rev 2)" -> 2
            return cap.at(0).toLower().unicode() - 'a' + 1;      // "(Rev A)" -> 1, "(Rev B)" -> 2
        }
        static const QRegularExpression ver(
            QStringLiteral("[\\(\\[]\\s*v\\s*(\\d+)(?:\\.(\\d+))?\\s*[\\)\\]]"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch vm = ver.match(nameOrPath);
        if (vm.hasMatch())
        {
            const int major = vm.captured(1).toInt();
            const int minor = vm.captured(2).isEmpty() ? 0 : vm.captured(2).toInt();
            return major * 1000 + minor;                         // "(v1.1)" -> 1001, "(v1.0)" -> 1000
        }
        return 0;
    }

    // A sensible default region priority for an app language, so the feature is useful WITHOUT any reorder UI
    // (the ordered-region editor is an explicit follow-up per the issue). `appLanguage` may be a bare code
    // ("en") or a full locale ("en_US" / "pt-BR"); only the leading language subtag is used. The documented
    // mapping — English and unknown default to a US-first order; Japanese/Korean/Chinese lead with their home
    // region; European languages lead with Europe (plus their own country) then World, US, Japan:
    inline QStringList defaultPriority(const QString& appLanguage)
    {
        QString lang = appLanguage.toLower();
        const int cut = lang.indexOf(QRegularExpression(QStringLiteral("[_-]")));
        if (cut >= 0) lang = lang.left(cut);

        const QStringList usaFirst  = { QStringLiteral("USA"), QStringLiteral("World"),
                                        QStringLiteral("Europe"), QStringLiteral("Japan") };
        if (lang == QStringLiteral("ja"))
            return { QStringLiteral("Japan"), QStringLiteral("World"), QStringLiteral("USA"), QStringLiteral("Europe") };
        if (lang == QStringLiteral("ko"))
            return { QStringLiteral("Korea"), QStringLiteral("Japan"), QStringLiteral("World"),
                     QStringLiteral("USA"), QStringLiteral("Europe") };
        if (lang == QStringLiteral("zh"))
            return { QStringLiteral("China"), QStringLiteral("Japan"), QStringLiteral("World"),
                     QStringLiteral("USA"), QStringLiteral("Europe") };

        // European languages: prefer the PAL/Europe release, then the speaker's own country, then World/US/JP.
        static const QHash<QString, QString> euCountry = {
            { QStringLiteral("fr"), QStringLiteral("France") },
            { QStringLiteral("de"), QStringLiteral("Germany") },
            { QStringLiteral("it"), QStringLiteral("Italy") },
            { QStringLiteral("es"), QStringLiteral("Spain") },
            { QStringLiteral("pt"), QStringLiteral("Brazil") },
            { QStringLiteral("nl"), QStringLiteral("Netherlands") },
            { QStringLiteral("sv"), QStringLiteral("Sweden") },
        };
        const auto c = euCountry.constFind(lang);
        if (c != euCountry.constEnd())
            return { QStringLiteral("Europe"), c.value(), QStringLiteral("World"),
                     QStringLiteral("USA"), QStringLiteral("Japan") };

        return usaFirst; // English and every unrecognised language
    }

    // One collapsed group: the chosen variant (winner), its region-less clean title, and the losers.
    struct RegionGroup
    {
        QString          chosenPath;    // the winning variant's path
        QString          chosenTitle;   // its readable, tag-stripped title (DiscGroup::titleWithoutDiscTag)
        QVector<QString> otherVersions; // the losing variants, in the same (rank,revision,path) order, minus the winner
    };

    // Group a flat list of paths by DiscGroup::normalizedKey (so only same-title variants group — two truly
    // different games never merge), and within each group pick the winner by (regionRank asc, then revisionOf
    // desc, then path asc for determinism). A single-file group is its own winner with empty otherVersions.
    // The returned vector is ordered by chosenTitle then chosenPath, so a re-scan yields identical output.
    inline QVector<RegionGroup> collapseByRegion(const QVector<QString>& paths, const QStringList& priority)
    {
        QHash<QString, QVector<QString>> byKey;   // normalised title key -> its variant paths
        QVector<QString>                 order;   // keys in first-seen order (stable before the final sort)
        for (const QString& p : paths)
        {
            const QString key = DiscGroup::normalizedKey(p);
            if (!byKey.contains(key)) order.push_back(key);
            byKey[key].push_back(p);
        }

        QVector<RegionGroup> out;
        out.reserve(order.size());
        for (const QString& key : order)
        {
            QVector<QString> variants = byKey.value(key);
            std::sort(variants.begin(), variants.end(), [&priority](const QString& a, const QString& b) {
                const int ra = regionRank(a, priority), rb = regionRank(b, priority);
                if (ra != rb) return ra < rb;                     // more-preferred region first
                const int va = revisionOf(a), vb = revisionOf(b);
                if (va != vb) return va > vb;                     // higher revision first
                return a < b;                                     // stable, deterministic tie-break
            });
            RegionGroup g;
            g.chosenPath  = variants.front();
            g.chosenTitle = DiscGroup::titleWithoutDiscTag(variants.front());
            for (int i = 1; i < variants.size(); ++i) g.otherVersions.push_back(variants[i]);
            out.push_back(std::move(g));
        }

        std::sort(out.begin(), out.end(), [](const RegionGroup& a, const RegionGroup& b) {
            const int c = a.chosenTitle.compare(b.chosenTitle, Qt::CaseInsensitive);
            if (c != 0) return c < 0;
            return a.chosenPath < b.chosenPath;
        });
        return out;
    }
}
