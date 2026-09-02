#include "LiveTvIdentity.h"

#include <QHash>
#include <QSet>

namespace {

const QLatin1String kPrefix("livetv:");
const QLatin1String kNamePrefix("livetv:name:");

// The closed quality-tag set. Lower-case, already normalised. See the header for why it is short.
bool isQualityTag(const QString& tok)
{
    static const QSet<QString> kTags = {
        QStringLiteral("hd"), QStringLiteral("fhd"), QStringLiteral("uhd"),
        QStringLiteral("sd"), QStringLiteral("hq"),
        QStringLiteral("4k"), QStringLiteral("8k"),
    };
    return kTags.contains(tok);
}

} // namespace

QString LiveTvIdentity::normalizedName(const QString& raw)
{
    // simplified() is exactly trim + collapse-internal-whitespace; toCaseFolded() is the case-insensitive
    // comparison Qt itself uses (toLower is locale-shaped and would fold a Turkish "I" differently on a
    // Turkish device — an identity that changes with the system locale is not an identity).
    return raw.simplified().toCaseFolded();
}

QString LiveTvIdentity::withoutQualityTag(const QString& normalized)
{
    const int sp = normalized.lastIndexOf(QLatin1Char(' '));
    if (sp <= 0) return normalized;                  // one token (or empty): there is no tag to take off
    QString tok = normalized.mid(sp + 1);
    // A tag is often bracketed ("cnn (hd)", "cnn [fhd]"). Peel one matching pair, and only a matching one.
    if ((tok.startsWith(QLatin1Char('(')) && tok.endsWith(QLatin1Char(')')))
        || (tok.startsWith(QLatin1Char('[')) && tok.endsWith(QLatin1Char(']'))))
        tok = tok.mid(1, tok.size() - 2);
    if (!isQualityTag(tok)) return normalized;
    return normalized.left(sp);                      // already trimmed: simplified() left no double spaces
}

bool LiveTvIdentity::isLiveTvId(const QString& id) { return id.startsWith(kPrefix); }

bool LiveTvIdentity::isCredentialShaped(const QString& id)
{
    if (!isLiveTvId(id)) return false;
    // The payload, whichever spelling it is in. A name-spelled id is derived from an M3U title, which is user
    // content and could in principle BE a url — so the same test is applied to it rather than trusting the
    // prefix. Costs nothing and closes the only way a credential could reach the wire through the fallback.
    const QString rest = id.startsWith(kNamePrefix) ? id.mid(QString(kNamePrefix).size())
                                                    : id.mid(QString(kPrefix).size());
    return rest.contains(QStringLiteral("://"));
}

QString LiveTvIdentity::idForTvgId(const QString& tvgId)
{
    const QString t = tvgId.trimmed();
    return t.isEmpty() ? QString() : kPrefix + t;
}

QString LiveTvIdentity::idForName(const QString& channelName)
{
    const QString n = normalizedName(channelName);
    return n.isEmpty() ? QString() : kNamePrefix + n;
}

QVector<QString> LiveTvIdentity::idsFor(const QVector<Channel>& channels, QStringList* collisionsOut)
{
    // Pass 1: every entry's normalised name, and the SET of them — the quality-tag rule asks "does the bare
    // name exist in this list", and it asks it of the whole list rather than only of the tagless entries,
    // because a tagged entry's twin may itself have a tvg-id (in which case the two are simply two identities,
    // which is right: they are two streams the provider chose to distinguish).
    QVector<QString> norms;
    norms.reserve(channels.size());
    QSet<QString> present;
    for (const Channel& c : channels)
    {
        const QString n = normalizedName(c.name());
        norms.push_back(n);
        if (!n.isEmpty()) present.insert(n);
    }

    QVector<QString> ids;
    ids.reserve(channels.size());
    QHash<QString, int> seen;      // id -> how many entries derived it
    QStringList reported;
    for (int i = 0; i < channels.size(); ++i)
    {
        QString id = idForTvgId(channels[i].tvgId);
        if (id.isEmpty())
        {
            QString n = norms.at(i);
            const QString bare = withoutQualityTag(n);
            // Strip ONLY when the bare name is a channel this list also carries. Otherwise "CNN HD" is simply
            // what this provider calls the channel and the tag is part of its name.
            if (bare != n && present.contains(bare)) n = bare;
            id = idForName(n);
        }
        // A row with neither a tvg-id nor a name is the one shape that cannot be named from its own content.
        // It still gets an identity — positional, and scoped to nothing but this list — because the
        // alternative is a channel that cannot be starred at all. It resolves only while the list is
        // unchanged, which is honest: there is nothing else about the row to be faithful to.
        if (id.isEmpty()) id = kPrefix + QStringLiteral("unnamed:") + QString::number(i);
        const int n = ++seen[id];
        if (n == 2 && collisionsOut) reported << id;
        ids.push_back(id);
    }
    if (collisionsOut) *collisionsOut = reported;
    return ids;
}

QString LiveTvIdentity::urlFor(const QVector<Channel>& channels, const QString& id)
{
    if (id.isEmpty()) return QString();
    const QVector<QString> ids = idsFor(channels);
    for (int i = 0; i < ids.size(); ++i)
        if (ids.at(i) == id) return channels.at(i).url;   // first in list order wins
    return QString();
}

QString LiveTvIdentity::wireId(const QString& title)
{
    const QString id = idForName(title);
    // A titleless row still travels under a name, so the merge's repair always has a key to match on. It is
    // deliberately the same shape as every other name id, because on a peer that HAS such a channel it is a
    // real identity and will resolve. A row whose TITLE is itself a url (some providers put the stream in the
    // #EXTINF label) takes the same anonymous spelling rather than putting that url on the wire — this
    // function's entire job is to be the thing that is safe to send.
    if (id.isEmpty() || isCredentialShaped(id)) return kNamePrefix + QStringLiteral("(untitled channel)");
    return id;
}
