#include "MediaSegments.h"
#include "LocalLibrary.h"

#include <QLatin1Char>
#include <QRegularExpression>
#include <QStringList>
#include <initializer_list>

namespace {

// One EDL time token: plain seconds, HH:MM:SS.sss, or #frames. Returns false when unusable — including a
// frame token with no frame rate to convert it, which is why fps is threaded all the way down here.
bool parseTime(const QString& tok, double fps, double* out)
{
    if (tok.isEmpty()) return false;
    if (tok.startsWith(QLatin1Char('#')))
    {
        if (fps <= 0.0) return false;
        bool ok = false;
        const double frames = tok.mid(1).toDouble(&ok);
        if (!ok || frames < 0.0) return false;
        *out = frames / fps;
        return true;
    }
    if (tok.contains(QLatin1Char(':')))
    {
        const QStringList p = tok.split(QLatin1Char(':'));
        if (p.size() != 3) return false;
        bool h = false, m = false, s = false;
        const double hh = p[0].toDouble(&h), mm = p[1].toDouble(&m), ss = p[2].toDouble(&s);
        if (!h || !m || !s) return false;
        *out = hh * 3600.0 + mm * 60.0 + ss;
        return *out >= 0.0;
    }
    bool ok = false;
    *out = tok.toDouble(&ok);
    return ok && *out >= 0.0;
}

// The chapter title, lowercased with every non-alphanumeric run collapsed to a single space. "[OP]" -> "op",
// "Opening Credits!" -> "opening credits".
QString normalizeTitle(const QString& t)
{
    QString s;
    s.reserve(t.size());
    for (const QChar c : t) s += c.isLetterOrNumber() ? c.toLower() : QLatin1Char(' ');
    return s.simplified();
}

// WORD-BOUNDARY containment on a space-normalized string. Substring matching would make "Introduction" an
// intro and would match "op"/"ed" inside ordinary words.
bool hasPhrase(const QString& norm, const char* phrase)
{
    const QString padded = QLatin1Char(' ') + norm + QLatin1Char(' ');
    return padded.contains(QLatin1Char(' ') + QLatin1String(phrase) + QLatin1Char(' '));
}

bool matchAny(const QString& norm, std::initializer_list<const char*> phrases)
{
    for (const char* p : phrases) if (hasPhrase(norm, p)) return true;
    return false;
}

// Five stages, and the order is load-bearing.
//
// Intro must be tested before the GENERIC "credits", or "Opening Credits" — which contains "credits" — would
// type every anime and drama opening as end credits. But the intro stage owns the bare segment nouns
// "titles"/"title"/"theme", and "End Titles" / "Closing Titles" (the conventional BBC/ITV chapter names)
// contain "titles". So the end cues are tested first of all: letting one fall through to the intro stage
// would hand a caller an "Intro" at 2700s of a 2760s episode — an offer to skip the rest of the episode, the
// same failure parseEdl's credits-first rule exists to prevent.
//
// Stage 1 is COMPOSITIONAL — any END QUALIFIER next to any SEGMENT NOUN — rather than a flat list of the
// spelled-out pairs. A flat list is unfixable by construction: it enumerates a cross product, so every pair
// nobody thought to write down is a live bug. "Closing Theme" was exactly that — no "closing theme" entry,
// so it fell to the intro stage, matched the bare "theme", and typed as an Intro; so did "End Theme",
// "Final Titles" and the singular "End Title". The product closes the whole class at once.
//
// Matching stays WORD-BOUNDARY (hasPhrase) throughout: on substrings "Introduction" would be an intro and
// "op"/"ed" would fire inside ordinary words.
//
// Phrases whose match is already implied by a shorter phrase in the SAME stage are omitted, not listed for
// documentation: "previously on" can never out-match "previously", so listing it would read as a live rule
// that can never decide anything.
std::optional<MediaSegments::SegmentType> typeForTitle(const QString& title)
{
    using T = MediaSegments::SegmentType;
    const QString n = normalizeTitle(title);
    if (n.isEmpty()) return std::nullopt;
    // 1. An END QUALIFIER × a SEGMENT NOUN: "End Titles", "Closing Theme", "Final Titles", "End Credits"…
    //    Note "opening" is deliberately NOT a qualifier here, so "Opening Credits" misses this stage and is
    //    caught as an Intro at stage 3.
    if (matchAny(n, { "end", "ending", "closing", "final" }) &&
        matchAny(n, { "credits", "credit", "titles", "title", "theme" }))
        return T::Credits;
    // 2. Standalone end markers, which name the end with no segment noun at all.
    if (matchAny(n, { "outro", "ed", "ending" }))
        return T::Credits;
    // 3. Intro, before the generic "credits" below can claim "Opening Credits".
    if (matchAny(n, { "intro", "opening", "titles", "title", "theme", "op" }))
        return T::Intro;
    if (matchAny(n, { "recap", "previously" }))
        return T::Recap;
    // 5. Generic: a bare "Credits" with no opening/closing qualifier is the end credits.
    if (matchAny(n, { "credits", "credit" }))
        return T::Credits;
    return std::nullopt;
}

} // namespace

QVector<MediaSegments::Segment> MediaSegments::parseEdl(const QString& text, double duration, double fps)
{
    QVector<Segment> out;
    bool introTaken = false;
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]")), Qt::SkipEmptyParts);
    for (const QString& raw : lines)
    {
        const QStringList f = raw.trimmed().split(ws, Qt::SkipEmptyParts);
        if (f.size() != 3) continue;
        double start = 0.0, end = 0.0;
        if (!parseTime(f[0], fps, &start) || !parseTime(f[1], fps, &end)) continue;
        bool ok = false;
        const int action = f[2].toInt(&ok);
        if (!ok) continue;
        if (action != 0 && action != 3) continue;          // 1 mute / 2 scene marker are not skips
        if (duration > 0.0)
        {
            if (start >= duration) continue;
            end = qMin(end, duration);
        }
        // Also the inverted-range guard: kMinSegmentS is positive, so an end at or before start fails this.
        if (end - start < kMinSegmentS) continue;

        Segment s{ start, end, SegmentType::Commercial };
        // Credits FIRST: in a short file one range can satisfy both rules, and typing it as an Intro would
        // offer to skip the rest of the episode.
        if (duration > 0.0 && end >= duration - kCreditsTailS)
            s.type = SegmentType::Credits;
        else if (!introTaken && start < kIntroWindowS && (end - start) <= kIntroMaxLenS)
        {
            s.type = SegmentType::Intro;
            introTaken = true;
        }
        out.push_back(s);
    }
    return out;
}

QVector<MediaSegments::Segment> MediaSegments::fromChapters(const QVector<Chapter>& chapters, double duration)
{
    QVector<Segment> out;
    for (int i = 0; i < chapters.size(); ++i)
    {
        const std::optional<SegmentType> ty = typeForTitle(chapters[i].title);
        if (!ty) continue;
        const double start = chapters[i].time;
        const double end   = (i + 1 < chapters.size()) ? chapters[i + 1].time : duration;
        // Covers the last chapter with an unknown duration too: end = duration <= 0 <= start fails this.
        if (end - start < kMinSegmentS) continue;
        out.push_back(Segment{ start, end, *ty });
    }
    return out;
}

QVector<MediaSegments::Segment> MediaSegments::resolve(const QVector<Segment>& edl,
                                                       const QVector<Segment>& chapters,
                                                       const QVector<Segment>& learned)
{
    QVector<Segment> out;
    for (const SegmentType t : { SegmentType::Intro, SegmentType::Credits,
                                 SegmentType::Recap, SegmentType::Commercial })
    {
        for (const QVector<Segment>* tier : { &edl, &chapters, &learned })
        {
            bool found = false;
            for (const Segment& s : *tier) if (s.type == t) { out.push_back(s); found = true; }
            if (found) break;                              // this type is settled; lower tiers do not add
        }
    }
    return out;
}

MediaSegments::Key MediaSegments::keyFor(const QString& imdbStreamId, const QString& localPath)
{
    Key k;
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    // SHAPE, not just arity: "tt0903747:2:7" is the contract, but other 3-part tile ids exist —
    // "tmdb:tv:1396" would otherwise key as seriesKey="tmdb", season=0, collapsing every TMDB-catalogued
    // show into ONE learned bucket and offering one show's intro during another's.
    if (p.size() == 3 && p[0].startsWith(QLatin1String("tt")))
    {
        k.seriesKey = p[0];
        k.season    = p[1].toInt();
        return k;
    }
    if (!localPath.isEmpty())
    {
        const LocalLibrary::VideoEntry e = LocalLibrary::parseFile(localPath);
        if (e.kind == LocalLibrary::Kind::Episode && !e.show.isEmpty())
        {
            k.seriesKey = LocalLibrary::showKeyFor(e);
            k.season    = e.season;
        }
    }
    return k;
}

QString MediaSegments::typeToString(SegmentType t)
{
    switch (t)
    {
    case SegmentType::Intro:      return QStringLiteral("intro");
    case SegmentType::Credits:    return QStringLiteral("credits");
    case SegmentType::Recap:      return QStringLiteral("recap");
    case SegmentType::Commercial: return QStringLiteral("commercial");
    }
    return QStringLiteral("intro");
}

std::optional<MediaSegments::SegmentType> MediaSegments::typeFromString(const QString& s)
{
    if (s == QLatin1String("intro"))      return SegmentType::Intro;
    if (s == QLatin1String("credits"))    return SegmentType::Credits;
    if (s == QLatin1String("recap"))      return SegmentType::Recap;
    if (s == QLatin1String("commercial")) return SegmentType::Commercial;
    return std::nullopt;
}

void MediaSegments::Tracker::reset(QVector<Segment> segments)
{
    segs_ = std::move(segments);
    consumed_.assign(static_cast<size_t>(segs_.size()), false);
}

std::optional<MediaSegments::Segment> MediaSegments::Tracker::onPosition(double t)
{
    std::optional<Segment> hit;
    for (int i = 0; i < segs_.size(); ++i)
    {
        const Segment& s = segs_[i];
        const size_t ix = static_cast<size_t>(i);
        // Positional re-arm: being before a segment's start means it lies ahead again, however we got here.
        // No need to track the previous position — a backward seek is implied by t < start.
        if (consumed_[ix] && t < s.start) consumed_[ix] = false;
        if (!consumed_[ix] && t >= s.start && t < s.end && !hit)
        {
            consumed_[ix] = true;
            hit = s;
        }
    }
    return hit;
}
