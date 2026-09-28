#include "DeepLink.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QUrl>

#include "AppBrand.h"

namespace DeepLink
{
namespace
{
const QLatin1String kHttps("https://");
const QLatin1String kManifestTail("/manifest.json");
const QByteArray kFrameHead("EBDL1 ");
const QByteArray kRefusalHead("EBDL1! ");

// Printable ASCII, and never a backslash: '\' is a path separator to half the URL parsers on this platform
// (https://evil\@good is "good" to one reader and "evil" to another), so it has no business in a link.
bool cleanChars(const QString& s)
{
    for (const QChar c : s)
    {
        const ushort u = c.unicode();
        if (u <= 0x20 || u >= 0x7f || u == '\\') return false;
    }
    return true;
}

Result refuse(Refusal r) { Result out; out.refusal = r; return out; }

QString tr(const char* s) { return QCoreApplication::translate("DeepLink", s); }

// [A-Za-z0-9_-] and nothing else, capped: what may go into a socket name from outside.
QString nameSafe(const QString& s, int cap)
{
    QString out;
    for (const QChar c : s)
    {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '-' || u == '_';
        out += ok ? c : QLatin1Char('_');
        if (out.size() >= cap) break;
    }
    return out;
}

// One line of the card: "Label: a, b, c", or nothing when the list is empty.
QString line(const QString& label, const QStringList& items)
{
    QStringList clean;
    for (const QString& i : items)
    {
        const QString t = i.simplified().left(40);
        if (!t.isEmpty() && !clean.contains(t)) clean << t;
        if (clean.size() >= 12) break;
    }
    return clean.isEmpty() ? QString() : label.arg(clean.join(QStringLiteral(", ")));
}
} // namespace

bool isValidScheme(const QString& s)
{
    if (s.isEmpty() || s.size() > 64) return false;
    const ushort first = s.at(0).unicode();
    if (!(first >= 'a' && first <= 'z')) return false;
    for (const QChar c : s)
    {
        const ushort u = c.unicode();
        if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '+' || u == '-' || u == '.')) return false;
    }
    return true;
}

Result validateManifestUrl(const QString& url)
{
    if (url.isEmpty()) return refuse(Refusal::Empty);
    if (url.size() > kMaxLinkChars) return refuse(Refusal::TooLong);
    if (!cleanChars(url)) return refuse(Refusal::BadCharacters);
    // https, and only https. Checked on the string as it stands — after the one decode — so a scheme that is
    // still percent-encoded ("https%3A…", "%68ttps:…") is not https and never becomes it.
    if (!url.startsWith(kHttps, Qt::CaseInsensitive)) return refuse(Refusal::NotHttps);

    const int authStart = int(kHttps.size());
    int authEnd = int(url.size());
    for (const QChar stop : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') })
    {
        const int at = url.indexOf(stop, authStart);
        if (at >= 0 && at < authEnd) authEnd = at;
    }
    const QString authority = url.mid(authStart, authEnd - authStart);
    if (authority.contains(QLatin1Char('@'))) return refuse(Refusal::Userinfo);
    // A '%' left in the authority after the one decode is an encoded host or port — "host%2Fmanifest.json",
    // "user%40evil" — which a second decode would turn into something else. Refused, never decoded again.
    if (authority.isEmpty() || authority.contains(QLatin1Char('%'))) return refuse(Refusal::NoHost);
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty() || !parsed.userInfo().isEmpty())
        return refuse(Refusal::NoHost);

    // The whole URL ends in /manifest.json: no query, no fragment, no other file. Case-sensitive, like the
    // install path's own normalisation (AddonRoster::normalizeBase), so what is shown is what is installed.
    if (url.contains(QLatin1Char('?')) || url.contains(QLatin1Char('#')) || !url.endsWith(kManifestTail)
        || authEnd >= int(url.size()))
        return refuse(Refusal::NotManifest);

    Result ok;
    ok.refusal = Refusal::None;
    ok.manifestUrl = url;
    return ok;
}

bool isLinkArgument(const QString& arg, const QString& scheme)
{
    return !scheme.isEmpty() && arg.startsWith(scheme + QLatin1Char(':'), Qt::CaseInsensitive);
}

Result parse(const QString& link, const QString& scheme)
{
    if (link.isEmpty()) return refuse(Refusal::Empty);
    if (link.size() > kMaxLinkChars) return refuse(Refusal::TooLong);
    if (!cleanChars(link)) return refuse(Refusal::BadCharacters);

    const QString head = scheme + QStringLiteral("://");
    if (scheme.isEmpty() || !link.startsWith(head, Qt::CaseInsensitive)) return refuse(Refusal::WrongScheme);
    const QString rest = link.mid(head.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    const QString verb = slash < 0 ? rest : rest.left(slash);
    if (verb.compare(QLatin1String(kVerbAddon), Qt::CaseInsensitive) != 0) return refuse(Refusal::UnknownVerb);
    const QString payload = slash < 0 ? QString() : rest.mid(slash + 1);
    if (payload.isEmpty()) return refuse(Refusal::NoPayload);

    // EXACTLY ONE percent-decode. Whatever that yields must itself be the https manifest URL.
    const QString decoded = QUrl::fromPercentEncoding(payload.toLatin1());
    return validateManifestUrl(decoded);
}

QString refusalText(Refusal r)
{
    switch (r)
    {
    case Refusal::None:          return QString();
    case Refusal::Empty:
    case Refusal::NoPayload:     return tr("That link doesn't say which add-on to install.");
    case Refusal::TooLong:       return tr("That add-on link is too long to be a real one.");
    case Refusal::BadCharacters: return tr("That add-on link contains characters an add-on address can't have.");
    case Refusal::WrongScheme:   return tr("That isn't an EverythingBox link.");
    case Refusal::UnknownVerb:   return tr("EverythingBox links can only offer an add-on to install.");
    case Refusal::NotHttps:      return tr("That add-on link doesn't use a secure (https) address, so it wasn't opened.");
    case Refusal::Userinfo:      return tr("That add-on link hides a user name in its address, so it wasn't opened.");
    case Refusal::NoHost:        return tr("That add-on link doesn't name a valid website.");
    case Refusal::NotManifest:   return tr("That link doesn't point at an add-on's manifest.json.");
    }
    return QString();
}

const char* refusalCode(Refusal r)
{
    switch (r)
    {
    case Refusal::None:          return "ok";
    case Refusal::Empty:         return "empty";
    case Refusal::TooLong:       return "too-long";
    case Refusal::BadCharacters: return "bad-characters";
    case Refusal::WrongScheme:   return "wrong-scheme";
    case Refusal::UnknownVerb:   return "unknown-verb";
    case Refusal::NoPayload:     return "no-payload";
    case Refusal::NotHttps:      return "not-https";
    case Refusal::Userinfo:      return "userinfo";
    case Refusal::NoHost:        return "no-host";
    case Refusal::NotManifest:   return "not-manifest";
    }
    return "unknown";
}

QByteArray encodeHandoff(const QString& manifestUrl)
{
    const Result r = validateManifestUrl(manifestUrl);
    if (!r.ok()) return QByteArray();
    return kFrameHead + r.manifestUrl.toLatin1() + '\n';
}

QByteArray encodeRefusalHandoff(Refusal r)
{
    if (r == Refusal::None) return QByteArray();
    return kRefusalHead + QByteArray(refusalCode(r)) + '\n';
}

Result decodeHandoff(const QByteArray& message)
{
    if (message.size() > kMaxHandoffBytes) return refuse(Refusal::TooLong);
    if (message.startsWith(kRefusalHead) && message.endsWith('\n'))
    {
        const QByteArray code = message.mid(kRefusalHead.size(), message.size() - kRefusalHead.size() - 1);
        for (int r = int(Refusal::Empty); r <= int(Refusal::NotManifest); ++r)
            if (code == refusalCode(Refusal(r))) return refuse(Refusal(r));
        return refuse(Refusal::Empty);
    }
    if (!message.startsWith(kFrameHead) || !message.endsWith('\n')) return refuse(Refusal::Empty);
    const QByteArray body = message.mid(kFrameHead.size(), message.size() - kFrameHead.size() - 1);
    if (body.contains('\n') || body.contains('\r')) return refuse(Refusal::BadCharacters);
    return validateManifestUrl(QString::fromLatin1(body));
}

QString handoffServerName(const QString& userKey, const QString& sessionKey, const QString& testSuffix)
{
    const QByteArray h = QCryptographicHash::hash(userKey.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    QString n = QStringLiteral("EverythingBox-links-") + QString::fromLatin1(h);
    if (!sessionKey.isEmpty()) n += QStringLiteral("-s") + nameSafe(sessionKey, 12);
    if (!testSuffix.isEmpty()) n += QStringLiteral("-t-") + nameSafe(testSuffix, 40);
    return n;
}

QString schemeFor(bool uitest, const QString& uitestOverride)
{
    if (uitest && isValidScheme(uitestOverride)) return uitestOverride;
    return QString::fromLatin1(kScheme);
}

QString registrationSchemeFor(bool uitest, const QString& uitestOverride)
{
    if (!uitest) return QString::fromLatin1(kScheme);
    if (!isValidScheme(uitestOverride)) return QString();
    if (uitestOverride.compare(QLatin1String(kScheme), Qt::CaseInsensitive) == 0) return QString();
    return uitestOverride;
}

QString windowsClassKey(const QString& scheme)
{
    return isValidScheme(scheme) ? QStringLiteral("Software\\Classes\\") + scheme : QString();
}

QString windowsOpenCommand(const QString& exePath)
{
    // Quoted, so a space (C:\Program Files\…) and a non-ASCII profile name are fine. A quote would end the
    // quoting early and let the rest of the path become arguments, and a control character has no place in a
    // path: either one refuses.
    if (exePath.isEmpty() || exePath.contains(QLatin1Char('"'))) return QString();
    for (const QChar c : exePath) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return QString();
    return QLatin1Char('"') + QDir::toNativeSeparators(exePath) + QStringLiteral("\" \"%1\"");
}

QVector<RegOp> windowsRegisterPlan(const QString& scheme, const QString& exePath)
{
    const QString key = windowsClassKey(scheme);
    const QString cmd = windowsOpenCommand(exePath);
    if (key.isEmpty() || cmd.isEmpty()) return {};
    const QString exe = QDir::toNativeSeparators(exePath);
    return {
        { RegOp::SetValue, key, QString(), QStringLiteral("URL:%1 link").arg(QLatin1String(AppBrand::kDisplayName)) },
        { RegOp::SetValue, key, QStringLiteral("URL Protocol"), QString() },
        { RegOp::SetValue, key + QStringLiteral("\\DefaultIcon"), QString(), QLatin1Char('"') + exe + QStringLiteral("\",0") },
        { RegOp::SetValue, key + QStringLiteral("\\shell\\open\\command"), QString(), cmd },
    };
}

QVector<RegOp> windowsUnregisterPlan(const QString& scheme)
{
    const QString key = windowsClassKey(scheme);
    if (key.isEmpty()) return {};
    return { { RegOp::DeleteTree, key, QString(), QString() } };
}

QString linuxDesktopFileName(const QString& scheme)
{
    return isValidScheme(scheme) ? scheme + QStringLiteral("-url-handler.desktop") : QString();
}

QString linuxDesktopEntry(const QString& scheme, const QString& exePath)
{
    if (!isValidScheme(scheme) || exePath.isEmpty()) return QString();
    for (const QChar c : exePath) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return QString();
    // Desktop Entry spec, "The Exec key": inside a quoted argument, '"', '`', '$' and '\' are backslash-escaped;
    // the value as a whole is then a string, whose own escape rule doubles every backslash; and a literal '%'
    // is written '%%' so it is not taken for a field code.
    QString quoted;
    for (const QChar c : exePath)
    {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\'))
            quoted += QLatin1Char('\\');
        quoted += c;
    }
    quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    quoted.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=%1\n"
                          "NoDisplay=true\n"
                          "Terminal=false\n"
                          "Exec=\"%2\" %u\n"
                          "MimeType=x-scheme-handler/%3;\n")
        .arg(QLatin1String(AppBrand::kDisplayName), quoted, scheme);
}

Inbox::Arrival Inbox::arrive()
{
    Arrival a;
    a.ticket = ++next_;
    a.closeOpenCard = open_ != 0;
    latest_ = a.ticket;
    open_ = 0;
    return a;
}

bool Inbox::presentCard(quint64 ticket)
{
    if (ticket == 0 || ticket != latest_) return false;
    open_ = ticket;
    return true;
}

void Inbox::cardClosed(quint64 ticket)
{
    if (ticket != 0 && ticket == open_) open_ = 0;
}

bool Inbox::takeInstall(quint64 ticket)
{
    if (ticket == 0 || ticket != open_ || ticket == installed_) return false;
    installed_ = ticket;
    return true;
}

QString confirmTitle(const QString& name)
{
    QString n = name.simplified();
    if (n.size() > 60) n = n.left(59) + QChar(0x2026);
    return n.isEmpty() ? tr("Install this add-on?") : tr("Install %1?").arg(n);
}

QString confirmMessage(const QString& host, const QStringList& resources, const QStringList& catalogTypes,
                       const QStringList& permissions)
{
    QStringList lines;
    lines << tr("From: %1").arg(host.simplified().left(120));
    const QString res = line(tr("It provides: %1"), resources);
    lines << (res.isEmpty() ? tr("It provides: nothing it declares") : res);
    const QString types = line(tr("Catalog types: %1"), catalogTypes);
    if (!types.isEmpty()) lines << types;
    const QString perms = line(tr("It asks for: %1"), permissions);
    if (!perms.isEmpty()) lines << perms;
    lines << QString();
    lines << tr("This link came from outside EverythingBox. Install it only if you just asked a website to add it.");
    return lines.join(QLatin1Char('\n'));
}
} // namespace DeepLink
