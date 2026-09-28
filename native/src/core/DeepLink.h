// everythingbox:// deep links (issue #80, proposal 1): the PURE half. QtCore only, no I/O, no registry, no socket.
//
// A deep link is an UNAUTHENTICATED INSTALL VECTOR. Any web page, any chat message and any other program on the
// machine can hand the OS an everythingbox:// URL, and the OS will start us with it. So the rules below are the
// whole of what such a link can make EverythingBox do, and the confirmation card the app shows afterwards is the
// only thing that ever turns one into an install (MainWindowDeepLink.cpp; nothing installs without Install).
//
// ---- THE URL FORM -------------------------------------------------------------------------------------------
//
//     everythingbox://addon/<url-encoded https manifest URL>
//
// parse() returns either the manifest URL or a refusal, and it refuses:
//   * a payload scheme other than https — http too, because a configured add-on's manifest URL carries the
//     user's options (debrid keys among them) and must not cross the network in the clear; javascript:, file:
//     and data: are simply "not https";
//   * userinfo in the manifest URL (https://user:pass@host/…), which only ever serves to disguise the host;
//   * a manifest URL that does not END in /manifest.json — so no query, no fragment and no other file;
//   * anything over kMaxLinkChars (2 KiB) of input;
//   * any verb but `addon`;
//   * double-encoding tricks. The payload is percent-decoded EXACTLY ONCE, and the result must itself be the
//     https URL. `https%253A%252F%252F…` decodes once to `https%3A%2F%2F…`, which is not an https URL, so it is
//     refused; a decoder that went round again would have accepted it. A percent sign that survives the one
//     decode belongs to the manifest URL's own path (a configured add-on's options segment routinely holds
//     %7C and friends) and is carried through untouched.
//
// stremio:// is NOT claimed as a scheme: Stremio users rely on it. The #80 clipboard rule
// (StremioTranslate::installLinkFromText) already converts a pasted stremio:// link.
//
// ---- THE HANDOFF --------------------------------------------------------------------------------------------
//
// The OS starts a NEW process for every link. When one is already running, the new one hands the link over a
// dedicated per-user local socket and exits (DeepLinkChannel). Only the parsed, validated manifest URL crosses,
// in the one-line message encodeHandoff() writes; the receiver runs it through validateManifestUrl() again,
// because a local socket is itself something any program of this user can write to.
//
// ---- LOGGING ------------------------------------------------------------------------------------------------
//
// Nothing here logs, and no caller may log a manifest URL whole: it can carry the user's options and keys. Use
// LogSafeText::url(), which keeps scheme, host and file name only.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DeepLink
{
inline constexpr const char* kScheme       = "everythingbox";
inline constexpr const char* kVerbAddon    = "addon";
inline constexpr int         kMaxLinkChars = 2048;   // the whole link as the OS hands it over
inline constexpr int         kMaxHandoffBytes = 4096; // one handoff message, newline included

enum class Refusal
{
    None,           // accepted
    Empty,          // nothing at all
    TooLong,        // over kMaxLinkChars
    BadCharacters,  // whitespace, a control character, a backslash or non-ASCII
    WrongScheme,    // not <scheme>://
    UnknownVerb,    // <scheme>://<anything but addon>/
    NoPayload,      // <scheme>://addon/ with nothing after it
    NotHttps,       // the manifest URL is not https (http, javascript:, file:, data:, a still-encoded scheme, …)
    Userinfo,       // user[:pass]@ in the manifest URL
    NoHost,         // https:// with no usable host
    NotManifest,    // does not end in /manifest.json (a query or fragment included)
};

struct Result
{
    QString manifestUrl;                 // set only when accepted
    Refusal refusal = Refusal::Empty;
    bool ok() const { return refusal == Refusal::None; }
};

// A whole link, as the OS passed it on the command line, against the scheme this process answers to.
Result parse(const QString& link, const QString& scheme = QString::fromLatin1(kScheme));
// The manifest-URL half of the rules above, on its own: what the handoff receiver re-checks.
Result validateManifestUrl(const QString& url);
// Whether a command-line argument is addressed to `scheme` at all (a prefix check; parse() decides the rest).
bool isLinkArgument(const QString& arg, const QString& scheme);

// A plain sentence for the screen, and a short token for the log. Neither ever contains the link.
QString refusalText(Refusal r);
const char* refusalCode(Refusal r);

// ---- the handoff message --------------------------------------------------------------------------------
// "EBDL1 <manifest url>\n". Empty when `manifestUrl` does not validate: nothing unvalidated is ever sent.
QByteArray encodeHandoff(const QString& manifestUrl);
// A link the SENDER refused carries no URL at all — only why, so the running app can say so instead of the
// click doing nothing: "EBDL1! <refusal code>\n". Empty for Refusal::None.
QByteArray encodeRefusalHandoff(Refusal r);
// The receiver's half: the frame, then validateManifestUrl() on what it carried. A refusal frame decodes to
// its refusal (never ok()); an unknown code decodes to Refusal::Empty.
Result decodeHandoff(const QByteArray& message);
// The local-socket name. Derived from the user profile (`userKey`, hashed — the name never spells the user's
// home path) and the login session (`sessionKey`), so two users, or one user's two Windows sessions, never meet
// on one name. A non-empty `testSuffix` (a UI-test rig's pipe name) gives a rig a name of its own.
QString handoffServerName(const QString& userKey, const QString& sessionKey, const QString& testSuffix);

// ---- which scheme -----------------------------------------------------------------------------------------
bool isValidScheme(const QString& s);   // RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), lower case here
// The scheme this process ANSWERS to: the UI-test override when the test channel is on and it is valid,
// otherwise everythingbox.
QString schemeFor(bool uitest, const QString& uitestOverride);
// The scheme this process may REGISTER with the OS. Under the UI-test channel only a valid override that is
// not the real scheme — "" (register nothing) otherwise — so a test rig can never write the real key.
QString registrationSchemeFor(bool uitest, const QString& uitestOverride);

// ---- registration, as data ----------------------------------------------------------------------------------
// Windows, per user and without admin: HKEY_CURRENT_USER\<key>. Every key is under Software\Classes\<scheme>.
struct RegOp
{
    enum Kind { SetValue, DeleteTree };
    Kind    kind = SetValue;
    QString key;     // relative to HKEY_CURRENT_USER
    QString name;    // value name; "" = the key's default value
    QString value;   // SetValue only
    bool operator==(const RegOp& o) const
    { return kind == o.kind && key == o.key && name == o.name && value == o.value; }
};
QString windowsClassKey(const QString& scheme);            // Software\Classes\<scheme>
QString windowsOpenCommand(const QString& exePath);        // "<exe>" "%1"   ("" for an unusable path)
QVector<RegOp> windowsRegisterPlan(const QString& scheme, const QString& exePath);   // empty = refuse
QVector<RegOp> windowsUnregisterPlan(const QString& scheme);                         // empty = refuse
// Linux: a .desktop file in ~/.local/share/applications claiming x-scheme-handler/<scheme>.
QString linuxDesktopFileName(const QString& scheme);
QString linuxDesktopEntry(const QString& scheme, const QString& exePath);             // "" = refuse

// ---- one card at a time -------------------------------------------------------------------------------------
// While a confirmation card is open, a second link REPLACES it rather than stacking a second card on top: the
// newer link closes the older card, and the older link's fetch, if it is still in flight, is dropped when it
// lands. An Install press counts only on the card that is open now, and only once.
class Inbox
{
public:
    struct Arrival
    {
        quint64 ticket = 0;          // this link's ticket
        bool    closeOpenCard = false; // a card is open for an older link: close it now
    };
    Arrival arrive();
    // The fetch for `ticket` finished. True: present its card now. False: a newer link superseded it.
    bool presentCard(quint64 ticket);
    // The card for `ticket` closed, whatever closed it.
    void cardClosed(quint64 ticket);
    // An Install press on `ticket`'s card: true exactly once, and only while that card is the open one.
    bool takeInstall(quint64 ticket);

    quint64 openCard() const { return open_; }   // 0 = no card open
    quint64 latest() const { return latest_; }

private:
    quint64 next_   = 0;
    quint64 latest_ = 0;
    quint64 open_   = 0;
    quint64 installed_ = 0;
};

// ---- the card -----------------------------------------------------------------------------------------------
// The confirmation card's words, from what the fetched manifest declares. `name` is the add-on's own claim
// about itself, so the host line is what the user should trust.
QString confirmTitle(const QString& name);
QString confirmMessage(const QString& host, const QStringList& resources, const QStringList& catalogTypes,
                       const QStringList& permissions);
} // namespace DeepLink
