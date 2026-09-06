// legendary (https://github.com/derrod/legendary) as an EverythingBox store backend — Epic Games without the
// Epic Games Launcher. Issue #118, increment 1: DETECTION, SIGN-IN SURFACING and the OWNED-LIBRARY LISTING.
//
// Read StoreBackend.h first; it holds the rules this file obeys (shell out, never reimplement; detect, never
// install; nothing assumes Windows; every spawn is bounded and off the GUI thread).
//
// THE THREE COMMANDS THIS INCREMENT USES, and nothing else:
//   legendary status --json      -> {"account": "...", "games_available": N, ...}   (is an account linked?)
//   legendary list --json        -> [ { "app_name": ..., "app_title": ..., ... }, ... ]  (what is owned)
//   legendary auth --code <code> -> completes the browser sign-in the user started
// `install`, `update`, `verify`, `repair` and the download queue are later increments and are deliberately
// not wired: a listing that could also start a 90 GB download is not a listing.
//
// WHY THE PARSE IS DEFENSIVE. legendary's `--json` output is a dump of its own Game objects and it has grown
// keys over versions. A single record this build cannot read must cost that record, never the library: a user
// with one odd entitlement should still see the other four hundred games. Only a body that is not a JSON
// ARRAY AT ALL is Malformed — that is the tool having said something else entirely (a traceback, a prompt),
// and there is nothing to salvage.
//
// WHERE THE CREDENTIAL LIVES: with legendary, not with us. Its refresh token sits in legendary's own config
// directory (its business, its file format, its lifetime). The only credential this code ever touches is the
// authorization code the user pastes, which goes on the child's argv — legendary offers no other
// non-interactive route — and is dropped the moment the process exits. Nothing here writes it anywhere.
#pragma once
#include "StoreBackend.h"

class LegendaryBackend final : public StoreBackend
{
public:
    // `toolPathOverride` exists for probes: a probe hands it a fake executable and drives the real process
    // path with no legendary installed. It is a CONSTRUCTOR ARGUMENT rather than a #ifdef'd setter (the
    // PcGameId test-seam pattern) because it redirects ONE instance instead of the process, so a production
    // caller cannot reach it by accident — storeback::legendary() default-constructs.
    explicit LegendaryBackend(QString toolPathOverride = QString());

    QString id()          const override;
    QString toolName()    const override;
    QString storeName()   const override;
    QString launcherId()  const override;
    QString releasesUrl() const override;
    QString signInUrl()   const override;

    QString      toolPath()   const override;
    StoreAuth    authState()  const override;
    StoreListing ownedGames() const override;
    StoreStatus  signIn(const QString& authorizationCode) const override;

    // ---- pure, so the probe drives every outcome without legendary installed --------------------------

    // `legendary list --json` -> owned games. DLC and anything without an app_name is dropped; the rest is
    // deduped by app_name and sorted by title (case-insensitive), so two runs of the same library produce the
    // same order and the folder cannot reshuffle between refreshes.
    //
    // `malformed` (optional out) is set true ONLY when the body is not a JSON array. An EMPTY array is a
    // legitimate, well-formed answer meaning "you own nothing" and must not read as a failure — a user whose
    // account is genuinely empty is not a user whose backend is broken.
    static QVector<StoreGame> parseList(const QByteArray& json, bool* malformed = nullptr);

    // `legendary status --json` -> is an account linked? legendary prints a PLACEHOLDER rather than omitting
    // the key when it is signed out ("<not logged in>"), so an angle-bracketed value counts as signed out.
    // The account name itself is NOT returned: it is an email address, and nothing in this app needs to hold
    // one to say "you are signed in".
    static StoreAuth parseStatus(const QByteArray& json);

    // A completed run -> a listing. THE FOUR FAILURE MAPPINGS LIVE HERE, in a pure function, because they are
    // the behaviour the feature promises ("absent, hangs or malformed degrades to no games with a readable
    // reason") and a promise that can only be tested by hanging a real process is a promise nobody tests.
    static StoreListing listingFromRun(const storeback::ToolRun& r);

    // The argv for the sign-in. Exposed so a probe can pin that the pasted code goes to the CHILD and check,
    // by byte-scan, that it reached nothing else.
    static QStringList authArgs(const QString& authorizationCode);

    // The bounds. `list` talks to Epic over the network, so it is the generous one; `status` is local.
    static constexpr int kStatusTimeoutMs =  15000;
    static constexpr int kListTimeoutMs   =  60000;
    static constexpr int kAuthTimeoutMs   =  60000;

private:
    QString toolOverride_;
};
