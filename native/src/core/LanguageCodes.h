#pragma once
#include <QString>
#include <QHash>
#include <QLatin1Char>
#include <QSettings>

// Canonical language handling for the "preferred content language" setting.
// Canonical form = ISO-639-1 two-letter, lowercased. Empty = "no preference".
namespace LanguageCodes {

// ISO-639-2 (B and T variants) -> ISO-639-1, for the languages the settings UI offers.
inline const QHash<QString, QString>& iso3to1()
{
    static const QHash<QString, QString> m = {
        {QStringLiteral("eng"),QStringLiteral("en")},{QStringLiteral("spa"),QStringLiteral("es")},
        {QStringLiteral("fra"),QStringLiteral("fr")},{QStringLiteral("fre"),QStringLiteral("fr")},
        {QStringLiteral("deu"),QStringLiteral("de")},{QStringLiteral("ger"),QStringLiteral("de")},
        {QStringLiteral("ita"),QStringLiteral("it")},{QStringLiteral("por"),QStringLiteral("pt")},
        {QStringLiteral("nld"),QStringLiteral("nl")},{QStringLiteral("dut"),QStringLiteral("nl")},
        {QStringLiteral("rus"),QStringLiteral("ru")},{QStringLiteral("jpn"),QStringLiteral("ja")},
        {QStringLiteral("kor"),QStringLiteral("ko")},{QStringLiteral("zho"),QStringLiteral("zh")},
        {QStringLiteral("chi"),QStringLiteral("zh")},{QStringLiteral("ara"),QStringLiteral("ar")},
    };
    return m;
}

// ISO-639-1 -> the ISO-639-2 tag mpv most often sees on a track, so slang/alang match either.
inline const QHash<QString, QString>& iso1to3()
{
    static const QHash<QString, QString> m = {
        {QStringLiteral("en"),QStringLiteral("eng")},{QStringLiteral("es"),QStringLiteral("spa")},
        {QStringLiteral("fr"),QStringLiteral("fra")},{QStringLiteral("de"),QStringLiteral("deu")},
        {QStringLiteral("it"),QStringLiteral("ita")},{QStringLiteral("pt"),QStringLiteral("por")},
        {QStringLiteral("nl"),QStringLiteral("nld")},{QStringLiteral("ru"),QStringLiteral("rus")},
        {QStringLiteral("ja"),QStringLiteral("jpn")},{QStringLiteral("ko"),QStringLiteral("kor")},
        {QStringLiteral("zh"),QStringLiteral("zho")},{QStringLiteral("ar"),QStringLiteral("ara")},
    };
    return m;
}

// Any code (2- or 3-letter, any case) -> canonical 2-letter. Empty stays empty.
inline QString toCanonical(const QString& code)
{
    const QString c = code.trimmed().toLower();
    if (c.isEmpty()) return QString();
    if (c.size() == 2) return c;
    const auto it = iso3to1().constFind(c);
    return it != iso3to1().constEnd() ? it.value() : c.left(2);
}

// Canonical 2-letter -> mpv slang/alang list, e.g. "en" -> "en,eng". Empty stays empty.
inline QString toMpvLangList(const QString& canonical)
{
    const QString c = canonical.trimmed().toLower();
    if (c.isEmpty()) return QString();
    const auto it = iso1to3().constFind(c);
    return it != iso1to3().constEnd() ? (c + QLatin1Char(',') + it.value()) : c;
}

// Reads the canonical preferred content language from a settings store. Header-only so any
// translation unit (including probes that don't link the heavy Settings.cpp) shares ONE
// implementation. Guards on key PRESENCE, not emptiness: once content/language is written —
// even to "" for an explicit "no preference" — the legacy subs/language must not re-surface.
inline QString readPreferred(QSettings& s)
{
    if (s.contains(QStringLiteral("content/language")))
        return s.value(QStringLiteral("content/language")).toString();
    // Not yet set: derive (migrate) from the legacy 3-letter subtitle key, without persisting here.
    return toCanonical(s.value(QStringLiteral("subs/language")).toString());
}

} // namespace LanguageCodes
