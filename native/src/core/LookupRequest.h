// LookupRequest — the in-book lookup's PURE half (issue #137): what the three verbs ask for, and what a
// reader is shown when the answer comes back or fails to. URLs, a request body, three readers and every
// failure sentence — no socket, no Settings, no QWidget — which is what lets probe_lookup drive the whole
// contract against a fixture server and against hand-written bodies, including the failures a real endpoint
// would only produce by being broken at the moment you looked.
//
// THE THREE SOURCES, and why these three. Wiktionary's REST definition endpoint and Wikipedia's summary
// endpoint are zero-config and need no key, which is the bar #81/#88 set for anything this app turns on by
// itself; translation has no such endpoint, so it is a LibreTranslate-class instance whose URL the user
// supplies. An unset instance URL means the Translate verb is ABSENT from the menu (translateConfigured()),
// not present-and-broken: an action that cannot work should not be offered.
//
// EVERY FAILURE IS A SENTENCE. read*() never returns "not ok" with nothing to show: offline, a 404, a 5xx and
// a body that is not what the endpoint documents each produce a line a reader can act on. The card renders
// Outcome::text whatever happened, so there is no state in which the card sits on "Looking up…" for ever —
// which is the specific way a lookup panel usually fails.
//
// NOTHING HERE IS LOGGED AND NOTHING HERE IS A CREDENTIAL. The only user text that leaves the process is the
// selected word, and that only after an explicit verb press (the privacy line in Settings ▸ Reading says so).
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace LookupRequest
{

// The three verbs the annotation action menu gains beside Highlight.
enum class Verb { Define = 0, Wikipedia = 1, Translate = 2 };

// What the card shows. `ok` is only for the caller that wants to know whether to record the word; `text` is
// ALWAYS populated, success or failure, because the card has to render something either way.
struct Outcome
{
    bool    ok = false;
    QString text;     // the definition / the summary / the translation, or the failure sentence
};

// A language TAG reduced to the code Wikimedia's subdomains and LibreTranslate both speak: "en_US" -> "en",
// "pt-BR" -> "pt", "" -> "en". Also the SANITISER: the code becomes part of a host name, so anything that is
// not 2..8 plain ASCII letters is refused outright and falls back to English rather than being pasted into a
// URL. A hand-edited ini cannot steer a request at another host through this.
inline QString normalizeLang(const QString& raw)
{
    QString s = raw.trimmed().toLower();
    const int cut = s.indexOf(QLatin1Char('-')) >= 0 ? s.indexOf(QLatin1Char('-'))
                                                     : s.indexOf(QLatin1Char('_'));
    if (cut > 0) s = s.left(cut);
    if (s.size() < 2 || s.size() > 8) return QStringLiteral("en");
    for (QChar c : s)
        if (c < QLatin1Char('a') || c > QLatin1Char('z')) return QStringLiteral("en");
    return s;
}

// The selected text reduced to something worth asking about: whitespace collapsed, and the punctuation a
// selection drags in from the page (quotes, a trailing comma, an em dash) taken off both ends. The INSIDE is
// left alone, so "don't" and "New York" survive intact.
inline QString normalizeTerm(const QString& raw)
{
    QString s = raw.simplified();
    auto edge = [](QChar c) {
        return !(c.isLetterOrNumber() || c == QLatin1Char('\'') || c == QChar(0x2019)
                 || c == QLatin1Char(' ') || c == QLatin1Char('-'));
    };
    int a = 0, b = s.size();
    while (a < b && edge(s.at(a))) ++a;
    while (b > a && edge(s.at(b - 1))) --b;
    return s.mid(a, b - a).trimmed();
}

// The vocabulary list's identity for a word: case-folded, so "Ineffable" at the start of a sentence and
// "ineffable" in the middle of one are ONE row that updates rather than two that accumulate.
inline QString vocabKey(const QString& term, const QString& lang)
{
    return normalizeTerm(term).toLower() + QLatin1Char('|') + normalizeLang(lang);
}

// A word is worth a network call; a paragraph is not. Four words is the "or short phrase" the issue scopes,
// and the ceiling is what stops a mis-committed selection from posting a page of a book to a translator.
inline bool isLookupable(const QString& term)
{
    const QString t = normalizeTerm(term);
    if (t.isEmpty() || t.size() > 120) return false;
    return t.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() <= 4;
}

// A page title in a Wikimedia REST path: spaces are underscores (the canonical title form), then percent
// encoded whole — a '/' or a '?' in a selection would otherwise reshape the request.
inline QString pathTitle(const QString& term)
{
    QString t = normalizeTerm(term);
    t.replace(QLatin1Char(' '), QLatin1Char('_'));
    return QString::fromLatin1(QUrl::toPercentEncoding(t));
}

// GET https://<lang>.wiktionary.org/api/rest_v1/page/definition/<term>
inline QUrl defineUrl(const QString& term, const QString& lang)
{
    return QUrl(QStringLiteral("https://%1.wiktionary.org/api/rest_v1/page/definition/%2")
                    .arg(normalizeLang(lang), pathTitle(term)));
}

// GET https://<lang>.wikipedia.org/api/rest_v1/page/summary/<term>
inline QUrl summaryUrl(const QString& term, const QString& lang)
{
    return QUrl(QStringLiteral("https://%1.wikipedia.org/api/rest_v1/page/summary/%2")
                    .arg(normalizeLang(lang), pathTitle(term)));
}

// Is a translation instance configured at all? An empty (or non-http) setting means the verb is not offered.
inline bool translateConfigured(const QString& endpoint)
{
    const QString e = endpoint.trimmed();
    if (e.isEmpty()) return false;
    const QUrl u(e);
    return u.isValid() && !u.host().isEmpty()
        && (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https"));
}

// POST <endpoint>/translate. The setting is written by a person, so both spellings are accepted: a bare
// instance root ("https://libretranslate.example") and one that already names the route.
inline QUrl translateUrl(const QString& endpoint)
{
    if (!translateConfigured(endpoint)) return QUrl();
    QString e = endpoint.trimmed();
    while (e.endsWith(QLatin1Char('/'))) e.chop(1);
    if (!e.endsWith(QLatin1String("/translate"))) e += QLatin1String("/translate");
    return QUrl(e);
}

// LibreTranslate's documented body. "auto" as the source is deliberate for a book that declared no language:
// the endpoint detects it, rather than this app guessing and translating French as though it were English.
inline QByteArray translateBody(const QString& term, const QString& from, const QString& to)
{
    QJsonObject o;
    o.insert(QStringLiteral("q"), term);
    o.insert(QStringLiteral("source"), from.isEmpty() ? QStringLiteral("auto") : normalizeLang(from));
    o.insert(QStringLiteral("target"), normalizeLang(to));
    o.insert(QStringLiteral("format"), QStringLiteral("text"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Wikimedia asks every client to identify itself; a request with no User-Agent is throttled or refused.
inline QByteArray userAgent()
{
    return QByteArrayLiteral("EverythingBox/1.0 (in-book lookup; https://github.com/cubman3134/EverythingBox)");
}

// ---- the readers ------------------------------------------------------------------------------------------
// Wiktionary's definitions arrive as HTML fragments (links, italics, a <span> of qualifiers). The card is a
// plain label, so the tags come off here rather than being rendered as angle brackets in the middle of a
// sentence. Entities that survive a tag strip are the four that actually appear.
inline QString stripHtml(const QString& in)
{
    QString out;
    out.reserve(in.size());
    bool inTag = false;
    for (QChar c : in)
    {
        if (c == QLatin1Char('<')) { inTag = true; continue; }
        if (c == QLatin1Char('>')) { inTag = false; continue; }
        if (!inTag) out.append(c);
    }
    out.replace(QLatin1String("&amp;"),  QLatin1String("&"));
    out.replace(QLatin1String("&lt;"),   QLatin1String("<"));
    out.replace(QLatin1String("&gt;"),   QLatin1String(">"));
    out.replace(QLatin1String("&quot;"), QLatin1String("\""));
    out.replace(QLatin1String("&#39;"),  QLatin1String("'"));
    out.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    return out.simplified();
}

// The failure sentences, in one place so the card cannot show a different wording than the probe pins. `what`
// is the source's name as a reader would say it.
inline QString offlineText(const QString& what)
{
    return QStringLiteral("Couldn't reach %1. Check your connection and try again.").arg(what);
}
inline QString serverErrorText(const QString& what, int status)
{
    return QStringLiteral("%1 answered with an error (%2). Try again in a moment.").arg(what).arg(status);
}
inline QString unreadableText(const QString& what)
{
    return QStringLiteral("%1 answered with something this app couldn't read.").arg(what);
}

// A Wiktionary definition page. Success = at least one sense; the senses are grouped by part of speech,
// which is what a dictionary card looks like. `lang` picks the section: the endpoint returns EVERY language
// that spells the word this way, and showing the Latin entry for an English word is not a lookup.
inline Outcome readDefinition(const QByteArray& body, int status, bool transportFailed,
                              const QString& term, const QString& lang)
{
    const QString what = QStringLiteral("Wiktionary");
    Outcome o;
    if (transportFailed) { o.text = offlineText(what); return o; }
    if (status == 404)
    {
        o.text = QStringLiteral("No dictionary entry for “%1”.").arg(normalizeTerm(term));
        return o;
    }
    if (status < 200 || status >= 300) { o.text = serverErrorText(what, status); return o; }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
    {
        o.text = unreadableText(what);
        return o;
    }
    const QJsonObject root = doc.object();
    const QString code = normalizeLang(lang);
    // The requested language first; failing that, whatever section the page does carry — a word that exists
    // only in the book's other language is still a better answer than "no entry".
    QJsonArray sections = root.value(code).toArray();
    if (sections.isEmpty())
        for (auto it = root.begin(); it != root.end() && sections.isEmpty(); ++it)
            sections = it.value().toArray();

    QStringList lines;
    for (const QJsonValue& sv : sections)
    {
        const QJsonObject sec = sv.toObject();
        const QString pos = stripHtml(sec.value(QStringLiteral("partOfSpeech")).toString());
        int n = 0;
        for (const QJsonValue& dv : sec.value(QStringLiteral("definitions")).toArray())
        {
            const QString d = stripHtml(dv.toObject().value(QStringLiteral("definition")).toString());
            if (d.isEmpty()) continue;
            // Three senses per part of speech: a card is read at a glance, and Wiktionary's long entries run
            // to dozens. The rest is one tap away in a browser, which this feature is deliberately not.
            if (++n > 3) break;
            lines << (pos.isEmpty() ? QStringLiteral("%1. %2").arg(n).arg(d)
                                    : QStringLiteral("%1 %2. %3").arg(pos).arg(n).arg(d));
        }
        if (lines.size() >= 6) break;
    }
    if (lines.isEmpty())
    {
        o.text = QStringLiteral("No dictionary entry for “%1”.").arg(normalizeTerm(term));
        return o;
    }
    o.ok = true;
    o.text = lines.join(QLatin1Char('\n'));
    return o;
}

// Wikipedia's summary endpoint. A DISAMBIGUATION page is not an answer and says so — its extract is a list of
// things the word could mean, which reads as nonsense in a card that claims to be about one of them.
inline Outcome readSummary(const QByteArray& body, int status, bool transportFailed, const QString& term)
{
    const QString what = QStringLiteral("Wikipedia");
    Outcome o;
    if (transportFailed) { o.text = offlineText(what); return o; }
    if (status == 404)
    {
        o.text = QStringLiteral("Wikipedia has no article for “%1”.").arg(normalizeTerm(term));
        return o;
    }
    if (status < 200 || status >= 300) { o.text = serverErrorText(what, status); return o; }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
    {
        o.text = unreadableText(what);
        return o;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("type")).toString() == QLatin1String("disambiguation"))
    {
        o.text = QStringLiteral("“%1” means several things on Wikipedia — there is no single article "
                                "to show.").arg(normalizeTerm(term));
        return o;
    }
    const QString extract = root.value(QStringLiteral("extract")).toString().simplified();
    if (extract.isEmpty())
    {
        o.text = QStringLiteral("Wikipedia has no article for “%1”.").arg(normalizeTerm(term));
        return o;
    }
    o.ok = true;
    o.text = extract;
    return o;
}

// A LibreTranslate-class reply. Its errors come back as a JSON `error` with a 4xx, and that message is the
// most useful thing there is to show (a wrong target language, a rate limit, an instance that wants a key),
// so it is passed through rather than flattened into a generic sentence.
inline Outcome readTranslation(const QByteArray& body, int status, bool transportFailed)
{
    const QString what = QStringLiteral("the translation service");
    Outcome o;
    if (transportFailed) { o.text = offlineText(what); return o; }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    const QJsonObject root = doc.isObject() ? doc.object() : QJsonObject();
    const QString err = root.value(QStringLiteral("error")).toString().simplified();

    if (status < 200 || status >= 300)
    {
        o.text = err.isEmpty() ? serverErrorText(what, status)
                               : QStringLiteral("The translation service refused: %1").arg(err);
        return o;
    }
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
    {
        o.text = unreadableText(what);
        return o;
    }
    const QString t = root.value(QStringLiteral("translatedText")).toString().simplified();
    if (t.isEmpty())
    {
        o.text = err.isEmpty() ? unreadableText(what)
                               : QStringLiteral("The translation service refused: %1").arg(err);
        return o;
    }
    o.ok = true;
    o.text = t;
    return o;
}

// The sentence the vocabulary list stores beside a word, so a review shows where it was met. Taken from the
// chapter text around the selection, simplified to one line and elided on both sides — a passage, not a page.
inline QString contextAround(const QString& text, int start, int end, int radius = 70)
{
    if (text.isEmpty() || start < 0 || end <= start || start >= text.size()) return QString();
    const int e = qMin(end, text.size());
    const int a = qMax(0, start - radius);
    const int b = qMin(text.size(), e + radius);
    QString s = text.mid(a, b - a).simplified();
    if (a > 0) s.prepend(QStringLiteral("…"));
    if (b < text.size()) s.append(QStringLiteral("…"));
    return s;
}

// What the card is titled, and what the vocabulary row records as its source.
inline QString verbName(Verb v)
{
    switch (v)
    {
    case Verb::Wikipedia: return QStringLiteral("Wikipedia");
    case Verb::Translate: return QStringLiteral("Translate");
    case Verb::Define:    break;
    }
    return QStringLiteral("Define");
}

} // namespace LookupRequest
