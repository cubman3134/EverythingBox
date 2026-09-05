#include "AudiobookMeta.h"
#include "AppBrand.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

// A fact's value by LABEL, case-insensitive, first match wins. Returns "" when the reply carries no such
// label — which is the ONLY way this file learns a provider did not say (never a default, never a guess).
QString factValue(const MediaDetail& d, const QStringList& labels)
{
    for (const QString& want : labels)
        for (const MediaFact& f : d.facts)
            if (f.label.compare(want, Qt::CaseInsensitive) == 0 && !f.value.trimmed().isEmpty())
                return f.value.trimmed();
    return QString();
}

// The same read over MediaArt::meta, which is where an addon that publishes structured extras rather than
// display facts puts them. Facts win: they are what a provider means to SHOW, and a provider that shows a
// narrator is more sure of it than one that filed it under extras.
QString metaValue(const MediaDetail& d, const QStringList& labels)
{
    for (const QString& want : labels)
        for (auto it = d.art.meta.constBegin(); it != d.art.meta.constEnd(); ++it)
            if (it.key().compare(want, Qt::CaseInsensitive) == 0)
            {
                const QString v = it.value().toString().trimmed();
                if (!v.isEmpty()) return v;
            }
    return QString();
}

QString firstOf(const MediaDetail& d, const QStringList& labels)
{
    const QString f = factValue(d, labels);
    return f.isEmpty() ? metaValue(d, labels) : f;
}

} // namespace

// ---- the record ---------------------------------------------------------------------------------------

bool AudiobookMeta::Match::isEmpty() const
{
    return matchId.isEmpty() && matchTitle.isEmpty() && !hasFields();
}

bool AudiobookMeta::Match::hasFields() const
{
    return !narrator.isEmpty() || !series.isEmpty() || seriesIndex > 0 || !description.isEmpty()
           || !coverUrl.isEmpty() || year > 0 || runtimeSec > 0;
}

// ---- canonical JSON -----------------------------------------------------------------------------------

AudiobookMeta::Match AudiobookMeta::fromJson(const QJsonObject& o)
{
    Match m;
    m.provider    = o.value(QStringLiteral("provider")).toString();
    m.matchId     = o.value(QStringLiteral("matchId")).toString();
    m.matchTitle  = o.value(QStringLiteral("matchTitle")).toString();
    m.matchAuthor = o.value(QStringLiteral("matchAuthor")).toString();
    m.narrator    = o.value(QStringLiteral("narrator")).toString();
    m.series      = o.value(QStringLiteral("series")).toString();
    m.seriesIndex = o.value(QStringLiteral("seriesIndex")).toInt();
    m.description = o.value(QStringLiteral("description")).toString();
    m.coverUrl    = o.value(QStringLiteral("coverUrl")).toString();
    m.year        = o.value(QStringLiteral("year")).toInt();
    m.runtimeSec  = o.value(QStringLiteral("runtimeSec")).toInt();
    m.confidence  = o.value(QStringLiteral("confidence")).toInt();
    m.rejected    = o.value(QStringLiteral("rejected")).toBool();
    m.updatedAt   = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    return m;
}

QJsonObject AudiobookMeta::toJson(const Match& m)
{
    QJsonObject o;
    const auto putS = [&o](const char* k, const QString& v)
    { const QString t = v.trimmed(); if (!t.isEmpty()) o.insert(QLatin1String(k), t); };
    const auto putI = [&o](const char* k, int v) { if (v > 0) o.insert(QLatin1String(k), v); };
    putS("provider", m.provider);
    putS("matchId", m.matchId);
    putS("matchTitle", m.matchTitle);
    putS("matchAuthor", m.matchAuthor);
    putS("narrator", m.narrator);
    putS("series", m.series);
    putI("seriesIndex", m.seriesIndex);
    putS("description", m.description);
    putS("coverUrl", m.coverUrl);
    putI("year", m.year);
    putI("runtimeSec", m.runtimeSec);
    putI("confidence", m.confidence);
    if (m.rejected) o.insert(QStringLiteral("rejected"), true);
    if (m.updatedAt > 0) o.insert(QStringLiteral("updatedAt"), double(m.updatedAt));
    return o;
}

// ---- reading a provider's reply -----------------------------------------------------------------------

QStringList AudiobookMeta::labelsFor(const QString& field)
{
    // Every spelling an addon in the wild actually publishes. Order is preference: the first label a reply
    // carries is the one taken, so the specific spellings lead and the generic ones trail.
    if (field == QLatin1String("narrator"))
        return { QStringLiteral("Narrator"), QStringLiteral("Narrators"), QStringLiteral("Narrated by"),
                 QStringLiteral("Read by"), QStringLiteral("Reader") };
    if (field == QLatin1String("series"))
        return { QStringLiteral("Series") };
    if (field == QLatin1String("position"))
        // "Book" is the label a series' own position rides for an audiobook, and it is NOT the book's title:
        // the title has its own field on MediaDetail and this reader never touches it (see the header).
        return { QStringLiteral("Series position"), QStringLiteral("Book number"), QStringLiteral("Book"),
                 QStringLiteral("Volume") };
    if (field == QLatin1String("year"))
        return { QStringLiteral("Year"), QStringLiteral("Published"), QStringLiteral("Publication year"),
                 QStringLiteral("Release date") };
    if (field == QLatin1String("runtime"))
        return { QStringLiteral("Runtime"), QStringLiteral("Length"), QStringLiteral("Duration") };
    if (field == QLatin1String("author"))
        return { QStringLiteral("Author"), QStringLiteral("Authors"), QStringLiteral("Written by") };
    return {};
}

AudiobookMeta::Match AudiobookMeta::fromDetail(const MediaDetail& d, const QString& providerId)
{
    Match m;
    m.provider   = providerId;
    m.matchTitle = d.title.trimmed();
    // THE PROVIDER'S OWN ID FOR THE EDITION, out of the reply's extra-metadata bag ("matchId"/"editionId"),
    // which is the only free-form place a getMeta reply has for one. `imdbStreamId` is read as a last resort
    // only because an addon written for the film path may already fill it; nothing here means an IMDB id.
    m.matchId    = firstOf(d, { QStringLiteral("matchId"), QStringLiteral("editionId") });
    if (m.matchId.isEmpty()) m.matchId = d.imdbStreamId.trimmed();
    m.matchAuthor  = firstOf(d, labelsFor(QStringLiteral("author")));
    if (m.matchAuthor.isEmpty()) m.matchAuthor = d.subtitle.trimmed();
    m.description  = d.overview.trimmed();
    m.coverUrl     = d.imageUrl.trimmed();
    if (m.coverUrl.isEmpty()) m.coverUrl = d.art.image(QStringLiteral("poster"));
    if (m.coverUrl.isEmpty()) m.coverUrl = d.art.image(QStringLiteral("thumb"));
    m.narrator     = firstOf(d, labelsFor(QStringLiteral("narrator")));
    m.series       = firstOf(d, labelsFor(QStringLiteral("series")));
    m.seriesIndex  = parseSeriesIndex(firstOf(d, labelsFor(QStringLiteral("position"))));
    m.year         = parseYear(firstOf(d, labelsFor(QStringLiteral("year"))));
    m.runtimeSec   = parseRuntimeSec(firstOf(d, labelsFor(QStringLiteral("runtime"))));
    return m;
}

int AudiobookMeta::parseYear(const QString& v)
{
    static const QRegularExpression re(QStringLiteral("(1[0-9]{3}|20[0-9]{2}|21[0-9]{2})"));
    const QRegularExpressionMatch mm = re.match(v);
    return mm.hasMatch() ? mm.captured(1).toInt() : 0;
}

int AudiobookMeta::parseSeriesIndex(const QString& v)
{
    // The FIRST integer anywhere in the provider's own position field. "3", "#3", "Book 3", "3.5" (the
    // library's index is an int, so a half-book files with its whole one, which is where a shelf puts it).
    static const QRegularExpression re(QStringLiteral("([0-9]+)"));
    const QRegularExpressionMatch mm = re.match(v);
    if (!mm.hasMatch()) return 0;
    const int n = mm.captured(1).toInt();
    return n > 0 ? n : 0;
}

int AudiobookMeta::parseRuntimeSec(const QString& v)
{
    const QString s = v.trimmed();
    if (s.isEmpty()) return 0;

    // "14:20:00" / "51:30" — a clock, which is unambiguous and so is read first.
    static const QRegularExpression clock(QStringLiteral("^([0-9]+):([0-5][0-9])(?::([0-5][0-9]))?$"));
    const QRegularExpressionMatch cm = clock.match(s);
    if (cm.hasMatch())
    {
        const int a = cm.captured(1).toInt(), b = cm.captured(2).toInt();
        return cm.captured(3).isEmpty() ? a * 3600 + b * 60           // h:mm
                                        : a * 3600 + b * 60 + cm.captured(3).toInt();
    }

    // "14h 20m", "14 hours 20 minutes", "20m", "860 min".
    static const QRegularExpression hRe(QStringLiteral("([0-9]+)\\s*(?:h|hr|hrs|hour|hours)\\b"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mRe(QStringLiteral("([0-9]+)\\s*(?:m|min|mins|minute|minutes)\\b"),
                                        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch hm = hRe.match(s);
    const QRegularExpressionMatch mm = mRe.match(s);
    if (hm.hasMatch() || mm.hasMatch())
        return (hm.hasMatch() ? hm.captured(1).toInt() * 3600 : 0)
               + (mm.hasMatch() ? mm.captured(1).toInt() * 60 : 0);

    // A bare number is SECONDS, which is what a structured field carries. A bare number is never read as
    // minutes: guessing the unit of an unlabelled number is how a twelve-hour book becomes twelve minutes.
    bool ok = false;
    const qint64 n = s.toLongLong(&ok);
    return (ok && n > 0 && n < 3600 * 1000) ? int(n) : 0;
}

// ---- two providers answered ---------------------------------------------------------------------------

int AudiobookMeta::providerPriority(const QString& id)
{
    const QString p = QString::fromLatin1(AppBrand::kAddonPrefix);
    if (id == p + QLatin1String("openlibrary")) return 0;  // edition contributions -> narrator, series
    if (id == p + QLatin1String("googlebooks")) return 1;  // the better descriptions and covers
    return 100;                                            // anything the user installed: after ours, but in
}

AudiobookMeta::Match AudiobookMeta::mergeLowerPriority(const Match& hi, const Match& lo)
{
    Match out = hi;
    // The IDENTITY follows whoever named one — if the leading provider matched nothing, the record is the
    // other's match and must say so, or "reject this match" would name a provider that supplied no fields.
    if (out.matchTitle.isEmpty() && out.matchId.isEmpty())
    {
        out.provider   = lo.provider;
        out.matchId    = lo.matchId;
        out.matchTitle = lo.matchTitle;
    }
    if (out.matchAuthor.isEmpty()) out.matchAuthor = lo.matchAuthor;
    if (out.narrator.isEmpty())    out.narrator    = lo.narrator;
    if (out.series.isEmpty())      out.series      = lo.series;
    if (out.seriesIndex <= 0)      out.seriesIndex = lo.seriesIndex;
    if (out.description.isEmpty()) out.description = lo.description;
    if (out.coverUrl.isEmpty())    out.coverUrl    = lo.coverUrl;
    if (out.year <= 0)             out.year        = lo.year;
    if (out.runtimeSec <= 0)       out.runtimeSec  = lo.runtimeSec;
    return out;
}

// ---- is this the same book? ---------------------------------------------------------------------------

QString AudiobookMeta::normalizedName(const QString& s)
{
    QString t = s.toLower();
    // "hobbit, the" -> "the hobbit" before the article is dropped, so both spellings converge.
    static const QRegularExpression trailingArticle(QStringLiteral(",\\s*(the|a|an)\\s*$"));
    t.replace(trailingArticle, QString());
    static const QRegularExpression nonWord(QStringLiteral("[^a-z0-9 ]+"));
    t.replace(nonWord, QStringLiteral(" "));
    static const QRegularExpression leadingArticle(QStringLiteral("^(the|a|an) "));
    t.replace(leadingArticle, QString());
    return t.simplified();
}

namespace {

// Word-overlap agreement, 0..100: how much of the SHORTER name the two share. Shorter-side, because a
// provider's title is routinely the tagged one plus a subtitle ("Dune: Book One of the Dune Chronicles"),
// and scoring against the longer side would reject the right edition for being more complete than the tag.
int nameAgreement(const QString& a, const QString& b)
{
    const QStringList wa = AudiobookMeta::normalizedName(a).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList wb = AudiobookMeta::normalizedName(b).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (wa.isEmpty() || wb.isEmpty()) return 0;
    const QSet<QString> sa(wa.constBegin(), wa.constEnd());
    const QSet<QString> sb(wb.constBegin(), wb.constEnd());
    int shared = 0;
    for (const QString& w : sa) if (sb.contains(w)) ++shared;
    const int denom = std::min(sa.size(), sb.size());
    return denom > 0 ? (shared * 100) / denom : 0;
}

} // namespace

int AudiobookMeta::confidenceFor(const AudiobookLibrary::Book& scanned, const Match& m)
{
    if (m.matchTitle.trimmed().isEmpty()) return 0;   // named nothing -> found nothing

    // The book's own title, which for an untagged book is its FOLDER name — the honest thing to match on,
    // and the case the feature exists for.
    const QString mine = scanned.title;
    if (mine.trimmed().isEmpty()) return 0;           // nothing to compare: never a silent match

    const int title = nameAgreement(mine, m.matchTitle);   // 0..100, the dominant term
    int score = (title * 80) / 100;                        // at most 80 from the title alone

    // The author either agrees or it does not. A book with no author tag says nothing here rather than
    // counting against the match — an untagged library is the population this feature serves.
    if (tagCarries(scanned.author) && !m.matchAuthor.trimmed().isEmpty())
        score += nameAgreement(scanned.author, m.matchAuthor) >= 50 ? 15 : -25;

    // A narrator we did not have is what makes the match worth applying; it is evidence of an AUDIOBOOK
    // edition rather than of the right book, so it is a nudge and not a term.
    if (!m.narrator.trimmed().isEmpty() && !tagCarries(scanned.narrator)) score += 5;

    return std::max(0, std::min(100, score));
}

// ---- THE PRECEDENCE RULE ------------------------------------------------------------------------------

bool AudiobookMeta::tagCarries(const QString& tagged)
{
    return !tagged.trimmed().isEmpty();
}

QString AudiobookMeta::fill(const QString& tagged, const QString& enriched)
{
    return tagCarries(tagged) ? tagged : enriched.trimmed();
}

int AudiobookMeta::fillInt(int tagged, int enriched)
{
    return tagged > 0 ? tagged : std::max(0, enriched);
}

int AudiobookMeta::applyToEntries(QVector<AudiobookLibrary::FileEntry>& entries,
                                  const QHash<QString, Match>& byBookKey)
{
    if (byBookKey.isEmpty()) return 0;
    int touched = 0;
    for (AudiobookLibrary::FileEntry& e : entries)
    {
        const auto it = byBookKey.constFind(AudiobookLibrary::bookKeyFor(e));
        if (it == byBookKey.constEnd()) continue;
        const Match& m = it.value();
        if (m.rejected || m.confidence < kAcceptThreshold || !m.hasFields()) continue;

        bool changed = false;
        // NARRATOR lands on the EXPLICIT narrator field and never on `composer`: composer is a tag the file
        // carried and this file does not write tags. The blank being filled is effectiveNarrator() — the
        // library's own reading — so a file whose COMPOSER holds the narrator (the m4b convention) is
        // already narrated and is left completely alone.
        const QString narr = fill(e.effectiveNarrator(), m.narrator);
        if (narr != e.effectiveNarrator()) { e.narrator = narr; changed = true; }
        const QString ser = fill(e.series, m.series);
        if (ser != e.series) { e.series = ser; changed = true; }
        const int idx = fillInt(e.seriesIndex, m.seriesIndex);
        if (idx != e.seriesIndex) { e.seriesIndex = idx; changed = true; }
        const int yr = fillInt(e.year, m.year);
        if (yr != e.year) { e.year = yr; changed = true; }
        // DURATION IS NEVER FILLED FROM A MATCH. The scan's per-file seconds are what progressFor divides by
        // and what the "9h 14m" on the shelf sums; a provider's runtime is for the WHOLE book and for THEIR
        // edition, so writing it onto a part would make a progress bar that is wrong about a twelve-hour
        // book — the exact thing AudiobookLibrary::Progress::known exists to refuse. The matched runtime is
        // kept on the record and shown as a fact; it never becomes a number the player divides by.
        if (changed) ++touched;
    }
    return touched;
}

QString AudiobookMeta::matchSummary(const Match& m)
{
    QStringList parts;
    if (!m.matchTitle.isEmpty()) parts << m.matchTitle;
    if (!m.matchAuthor.isEmpty()) parts << m.matchAuthor;
    if (!m.narrator.isEmpty()) parts << m.narrator;
    if (m.confidence > 0) parts << (QString::number(m.confidence) + QLatin1Char('%'));
    return parts.join(QStringLiteral(" · "));
}

bool AudiobookMeta::wantsEnrichment(const AudiobookLibrary::Book& b)
{
    return !tagCarries(b.narrator) || !tagCarries(b.series) || b.year <= 0
           || b.coverSourcePath.isEmpty();
}
