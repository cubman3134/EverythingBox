#include "LegendaryBackend.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

LegendaryBackend::LegendaryBackend(QString toolPathOverride)
    : toolOverride_(std::move(toolPathOverride))
{
}

QString LegendaryBackend::id() const          { return QStringLiteral("legendary"); }
QString LegendaryBackend::toolName() const    { return QStringLiteral("legendary"); }
QString LegendaryBackend::storeName() const   { return QStringLiteral("Epic Games"); }
// "epic" is not a label: it is the value pcgame::PcGameSource::launcher carries, so a backend-listed game
// merges into the same group as an Epic-launcher-installed one and the launcher filter finds both.
QString LegendaryBackend::launcherId() const  { return QStringLiteral("epic"); }
QString LegendaryBackend::releasesUrl() const { return QStringLiteral("https://github.com/derrod/legendary/releases"); }

// The URL legendary itself tells the user to open. The page ends at a small JSON body containing
// `authorizationCode`, which is what the sign-in flow asks the user to paste.
QString LegendaryBackend::signInUrl() const
{
    return QStringLiteral("https://legendary.gl/epiclogin");
}

QString LegendaryBackend::toolPath() const
{
    if (!toolOverride_.isEmpty()) return toolOverride_;
    return storeback::findTool(toolName());
}

QStringList LegendaryBackend::authArgs(const QString& authorizationCode)
{
    return { QStringLiteral("auth"), QStringLiteral("--code"), authorizationCode };
}

// ---- pure parsing ---------------------------------------------------------------------------------------

// Is this record downloadable content rather than a base game? legendary spells it three ways depending on
// version and on whether metadata was cached, so all three are honoured. Getting this wrong in the permissive
// direction shows a user a "game" that is a soundtrack pack; getting it wrong in the strict direction hides a
// game they own, which is worse, so each test is specific rather than a guess at a shape.
static bool legendaryRecordIsDlc(const QJsonObject& o)
{
    if (o.value(QStringLiteral("is_dlc")).toBool()) return true;

    // `metadata.mainGameItem` present == this entitlement hangs off another one.
    const QJsonObject meta = o.value(QStringLiteral("metadata")).toObject();
    if (meta.contains(QStringLiteral("mainGameItem"))
        && meta.value(QStringLiteral("mainGameItem")).isObject()) return true;

    // `metadata.categories[].path == "addons"` (Epic's own vocabulary for DLC).
    for (const QJsonValue& c : meta.value(QStringLiteral("categories")).toArray())
    {
        const QString path = c.toObject().value(QStringLiteral("path")).toString();
        if (path.compare(QStringLiteral("addons"), Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

QVector<StoreGame> LegendaryBackend::parseList(const QByteArray& json, bool* malformed)
{
    if (malformed) *malformed = false;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
    {
        // NOT an array: legendary said something that is not a listing at all (a traceback, a prompt, an
        // empty body). There is no partial answer to salvage, so this — and only this — is Malformed.
        if (malformed) *malformed = true;
        return {};
    }

    QHash<QString, StoreGame> byApp;   // dedupe: the same entitlement can appear twice across namespaces
    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;                      // one bad record costs that record, not the library
        const QJsonObject o = v.toObject();
        StoreGame g;
        g.appName = o.value(QStringLiteral("app_name")).toString();
        if (g.appName.isEmpty()) continue;                // nothing to launch or key on
        if (legendaryRecordIsDlc(o)) continue;

        g.title = o.value(QStringLiteral("app_title")).toString();
        if (g.title.isEmpty())
            g.title = o.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("title")).toString();
        // A title-less entitlement is still owned. Falling back to the app name shows the user SOMETHING they
        // can act on; dropping it would silently shrink the library for a metadata gap on Epic's side.
        if (g.title.isEmpty()) g.title = g.appName;

        // Increment 1 does not read installs from the backend (the Epic manifests still own that on Windows,
        // and installing is a later increment), but the flag is carried so the later work has it.
        g.installed = o.value(QStringLiteral("is_installed")).toBool();
        byApp.insert(g.appName, g);
    }

    QVector<StoreGame> out;
    out.reserve(byApp.size());
    for (auto it = byApp.constBegin(); it != byApp.constEnd(); ++it) out.push_back(it.value());
    // Sorted by title, then by app name, so the order is TOTAL: two records that share a title (Epic does
    // ship those) cannot swap places between two refreshes of the same library.
    std::sort(out.begin(), out.end(), [](const StoreGame& a, const StoreGame& b) {
        const int c = a.title.compare(b.title, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.appName < b.appName;
    });
    return out;
}

StoreAuth LegendaryBackend::parseStatus(const QByteArray& json)
{
    StoreAuth a;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) { a.status = StoreStatus::Malformed; return a; }

    const QString account = doc.object().value(QStringLiteral("account")).toString().trimmed();
    // legendary fills this key with a human placeholder ("<not logged in>") rather than omitting it, so an
    // angle-bracketed value is signed out. Checked by the bracket rather than by matching the exact English
    // string: the placeholder wording is legendary's and has changed before.
    a.signedIn = !account.isEmpty() && !account.startsWith(QLatin1Char('<'));
    a.status   = StoreStatus::Ok;
    return a;
}

StoreListing LegendaryBackend::listingFromRun(const storeback::ToolRun& r)
{
    StoreListing l;
    if (!r.started)  { l.status = StoreStatus::ToolMissing; return l; }   // never launched
    if (r.timedOut)  { l.status = StoreStatus::Timeout;     return l; }   // launched, hung, killed
    if (r.exitCode != 0) { l.status = StoreStatus::Failed;  return l; }   // ran and refused

    bool malformed = false;
    const QVector<StoreGame> games = parseList(r.out, &malformed);
    if (malformed) { l.status = StoreStatus::Malformed; return l; }
    l.status = StoreStatus::Ok;
    l.games  = games;                 // legitimately empty for an account that owns nothing
    return l;
}

// ---- the blocking calls (pool thread only) --------------------------------------------------------------

StoreAuth LegendaryBackend::authState() const
{
    StoreAuth a;
    const QString exe = toolPath();
    if (exe.isEmpty()) { a.status = StoreStatus::ToolMissing; return a; }

    const storeback::ToolRun r = storeback::run(exe, { QStringLiteral("status"), QStringLiteral("--json") },
                                                kStatusTimeoutMs);
    if (!r.started)      { a.status = StoreStatus::ToolMissing; return a; }
    if (r.timedOut)      { a.status = StoreStatus::Timeout;     return a; }
    // legendary exits non-zero when it cannot even reach its own config. That is not "signed out" — it is a
    // failure — and saying so keeps "sign in" from being offered as the fix for something else.
    if (r.exitCode != 0) { a.status = StoreStatus::Failed;      return a; }
    return parseStatus(r.out);
}

StoreListing LegendaryBackend::ownedGames() const
{
    StoreListing l;
    const QString exe = toolPath();
    if (exe.isEmpty()) { l.status = StoreStatus::ToolMissing; return l; }

    // Ask about the account FIRST. `legendary list` on a signed-out install fails with a stack trace, which
    // would surface as the generic "couldn't list your library" when the honest answer — and the one with an
    // action attached — is "you haven't signed in yet".
    const StoreAuth a = authState();
    if (a.status != StoreStatus::Ok) { l.status = a.status;                return l; }
    if (!a.signedIn)                 { l.status = StoreStatus::NotSignedIn; return l; }

    return listingFromRun(storeback::run(exe, { QStringLiteral("list"), QStringLiteral("--json") },
                                         kListTimeoutMs));
}

StoreStatus LegendaryBackend::signIn(const QString& authorizationCode) const
{
    const QString exe = toolPath();
    if (exe.isEmpty()) return StoreStatus::ToolMissing;
    if (authorizationCode.trimmed().isEmpty()) return StoreStatus::Failed;

    const storeback::ToolRun r = storeback::run(exe, authArgs(authorizationCode.trimmed()), kAuthTimeoutMs);
    if (!r.started)      return StoreStatus::ToolMissing;
    if (r.timedOut)      return StoreStatus::Timeout;
    // A rejected or expired code is an ordinary non-zero exit. The child's stderr explains why, and it is
    // NOT surfaced: legendary echoes the request it made, and that request contains the code.
    if (r.exitCode != 0) return StoreStatus::Failed;
    return StoreStatus::Ok;
}
