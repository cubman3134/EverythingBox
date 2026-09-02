// An in-app "store": browse one or more public GitHub registries of themes, add-ons or decoration packs
// (each an index.json + raw files) and install entries into the local themes2/, addons/ or bezels/ folder.
// A built-in default registry (github.com/cubman3134) is always present; users can add their own
// independent registries.
//
// The three kinds are shaped differently and only share the browsing chrome:
//   * an ADD-ON entry LISTS its files (or names a URL to subscribe to);
//   * a THEMES2 entry names a FOLDER whose contents come from the registry repo's own tree;
//   * a DECORATION entry (issue #187) names a ZIP with a required sha256, which unpacks into
//     bezels/<system>/<packId>/ — one folder per system it carries, so remove is a folder delete.
// Everything specific to the second lives in ThemeRegistry (core) and everything specific to the third in
// DecorationPack / DecorationInstall, so this dialog and the themed Settings surfaces cannot disagree about
// which paths may become filenames or how an install lands on disk.
//
// Decorations are served from the SAME index document as themes2, under their own key, so they share this
// dialog's registry list (the `registry/themesExtras` ini key) rather than growing a second one — a user
// who adds a registry gets its themes AND its packs, which is the only arrangement that is not a surprise.
#pragma once
#include "../core/DecorationPack.h"  // DecorationPack::Entry — the parsed shape the decorations path works in
#include "../core/ThemeRegistry.h"   // ThemeRegistry::Entry — the parsed shape the themes path works in

#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QStringList>

#include <functional>

class AddonManager;
class QNetworkAccessManager;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QJsonObject;

class RegistryBrowser : public QDialog
{
    Q_OBJECT
public:
    enum Kind { Themes, Addons, Decorations };
    RegistryBrowser(Kind kind, AddonManager* addons, QWidget* parent = nullptr);

    bool installedSomething() const { return installed_; }

    // An install is SYNCHRONOUS: it downloads one file per nested event loop (downloadTo), each behind a
    // 20 s wall, and the surfaces that host this dialog inline keep their own navigation live while it
    // runs. A host that navigates away replaces the panel content, which DELETES this dialog — under its
    // own stack frame, with the download code still to return into it.
    //
    // What carries an exit INTO that nested loop is the QUEUED finished() connection the hosts use. A
    // queued call (QMetaCallEvent) has no loop-level guard: whichever event loop spins next delivers it,
    // the nested one inside downloadTo included. So an exit taken mid-install runs the host's
    // navigate-away handler while we are still down inside the download. Reproduced as an access
    // violation at downloadTo's reply->abort().
    //
    // Deferred DELETION is the opposite and is not the hazard: Qt stamps each DeferredDelete with the loop
    // level it was posted at and re-posts it until the level drops below that, so deleteLater() genuinely
    // outlives a nested loop (LibraryView::popPage relies on this).
    //
    // Hence: a host must ask this before navigating away of its own accord, and every exit the dialog
    // itself can take — Escape, the Close button, a host's explicit reject(), a close event — is funnelled
    // through done(), which defers rather than emitting finished() mid-install.
    bool isInstalling() const { return installing_; }

    // Leave when it is safe to: now if nothing is in flight, otherwise once the install finishes, via the
    // usual accept() -> finished -> host handler (queued, so it lands with no dialog frame on the stack).
    void closeWhenIdle();

    // Every close funnels here (accept/reject/Escape/closeEvent all call it), which is why the guard lives
    // on it rather than on any one button: mid-install it is refused and owed, not obeyed.
    void done(int r) override;

private:
    void deferExit();                      // remember an exit asked for mid-install, and say so on screen
    QString installStatus(const QString& text) const; // `text`, plus the owed-exit note when one is owed
    void finishInstall();                  // end of an install: clear the flag, honour a deferred exit

    // Bracket around an install so EVERY exit from it — including the early error returns — clears the
    // flag and settles a deferred close.
    struct InstallScope
    {
        explicit InstallScope(RegistryBrowser* b) : b_(b) { b_->installing_ = true; }
        ~InstallScope() { b_->finishInstall(); }
        InstallScope(const InstallScope&) = delete;
        InstallScope& operator=(const InstallScope&) = delete;
        RegistryBrowser* b_;
    };

    QString defaultUrl() const;            // the built-in cubman3134 registry
    QStringList extraRegistries() const;   // user-added registries
    void saveExtras(const QStringList& list);
    QStringList allRegistries() const;     // default + extras

    void renderRegistryRows();             // (re)draw the list of configured registries
    void fetchAll();                       // load every registry and merge the entries
    void fetchOne(const QString& indexUrl);

    // The card chrome both kinds share. Everything kind-specific — what "installed" means and what the
    // button does — arrives as a value or a callable, so neither kind can fall into the other's branch by
    // omission. The callable owns the button because both kinds retitle it from their own result.
    //
    // `installedAction`, when non-empty, is what an ALREADY-INSTALLED entry's button says and does instead
    // of the disabled "Installed ✓". Themes deliberately have no such verb (there is no theme uninstall on
    // this surface, and #131 turns on installed themes never being re-offered); decoration packs do — a
    // pack is bulk artwork the user will want to take back off a small disk, and the place they browsed for
    // it is the only place they will look for that.
    void addCard(const QString& name, const QString& author, const QString& description,
                 const QStringList& formFactors, const QString& indexUrl, bool installed,
                 const std::function<void(QPushButton*)>& onInstall,
                 const QString& installedAction = QString());

    // A registry that answered, parsed, and turned out not to be a document this app understands. It gets a
    // ROW OF ITS OWN in the list rather than only a line in the status bar, for two reasons the status bar
    // cannot cover: the status bar holds one sentence for every registry at once (so with three configured,
    // the two that worked would bury the one that did not), and it is overwritten by the very next thing
    // that happens — the per-file progress of any install, the "one install at a time" refusal. A card
    // persists until the next fetch, sits next to the entries the other registries did supply, and names
    // WHICH registry and WHY.
    void addProblemCard(const QString& indexUrl, const QString& reason);

    // Add-ons. An add-on entry LISTS its files (or names a remote URL to subscribe to), so it stays on the
    // raw QJsonObject; nothing in ThemeRegistry describes that shape.
    void renderEntry(const QJsonObject& entry, const QString& indexUrl);
    // false when the press was REFUSED — another install is already running, the entry is one this function
    // does not serve at all, or its listing names nothing to install (no id, no files). Nothing was touched
    // in any of those, so the card must go back exactly as it was, and in the last two pressing again cannot
    // change the answer. A FAILED install returns true: it was attempted, and the card is right to offer
    // Retry.
    bool installEntry(const QJsonObject& entry, const QString& indexUrl);
    bool isInstalled(const QJsonObject& entry) const;

    // The network half on its own: fetch a URL into memory, behind the 20 s wall, through a nested event
    // loop. Everything the theme path wants is the BYTES — the old shape wrote them to a fixed temp path and
    // read them straight back, which bought nothing and cost a `/tmp/eb-theme-*.tmp` that is world-writable
    // on desktop Linux (a pre-planted symlink is followed through an O_TRUNC open) and identical on both
    // Appearance surfaces, so two installs racing each other corrupt one temp file with two busy flags that
    // know nothing of each other.
    //
    // `maxBytes` is REQUIRED rather than defaulted, so a new call site has to answer the question: QNAM
    // buffers a whole response, and every caller here reads the body only once it has all arrived, so a
    // budget applied afterwards would bound nothing at all. The transfer is aborted as it crosses it (see
    // boundIncoming). *overBudget, when given, says the false return was that REFUSAL and not a network
    // error — the two want different words on screen, because only one of them is worth retrying.
    bool fetchToBuffer(const QString& url, qint64 maxBytes, QByteArray* out, QString* error,
                       bool* overBudget = nullptr);
    // …and the same fetch, landing in a real destination file. The add-on path installs by writing files
    // where they belong, so it keeps this form.
    bool downloadTo(const QString& url, const QString& destPath, QString* error);
    static QString baseUrl(const QString& indexUrl); // the index URL's directory
    QString localDirFor(const QString& id) const;

    // Themes: the entry names a FOLDER, so the file list comes from the registry repo's own tree rather
    // than from the index. Cached per registry for this dialog's lifetime — one API call per install, and
    // unauthenticated GitHub allows 60 an hour per IP.
    //
    // These take a PARSED ThemeRegistry::Entry, not a QJsonObject: parseIndex is what decides which index
    // key is authoritative and which entries are offerable at all, and re-deriving either here is how this
    // surface ends up listing a theme the themed Appearance surface refuses to show.
    void renderThemeEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl);
    bool isThemeInstalled(const ThemeRegistry::Entry& entry) const;
    bool installThemeEntry(const ThemeRegistry::Entry& entry, const QString& indexUrl); // false = refused
    QByteArray treeFor(const QString& indexUrl, QString* error);
    QHash<QString, QByteArray> treeCache_;

    // Decoration packs (#187): the entry names a ZIP plus the digest it must hash to, so installing is one
    // bounded download, one checksum comparison and one unpack — no repo tree, and therefore no GitHub API
    // call and no 60-an-hour rate limit. Removing is a folder delete, offered from the same card.
    //
    // These take a PARSED DecorationPack::Entry for the same reason the theme pair does: parseDecorations is
    // what decides which entries are offerable at all (no digest, no id, an unusable system name), and
    // re-deriving that here is how this surface ends up offering a pack the themed surface refuses.
    void renderDecoEntry(const DecorationPack::Entry& entry, const QString& indexUrl);
    bool isDecoInstalled(const DecorationPack::Entry& entry) const;
    bool installDecoEntry(const DecorationPack::Entry& entry, const QString& indexUrl); // false = refused
    void removeDecoEntry(const DecorationPack::Entry& entry);
    QString decorationsRoot() const;      // <data>/bezels — the folder #106's renderer already scans
    QStringList knownSystemIds() const;   // the systems a pack's folders are matched against

    void updateRepoLink();

    Kind kind_;
    AddonManager* addons_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
    QVBoxLayout* registriesLayout_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* repoLink_ = nullptr;
    int pending_ = 0;
    int total_ = 0;
    // Registries that answered with something this app could not read. Kept as a COUNT beside total_ because
    // the closing status line has to tell "no registry offers anything" from "nothing here was legible" —
    // the two used to be the same sentence, which is the one thing #174 is about. The reasons themselves are
    // on the cards; this is only how many.
    int shapeProblems_ = 0;
    bool installed_ = false;
    bool installing_ = false;      // an install's nested event loops are on the stack
    bool closeWhenIdle_ = false;   // an exit was asked for while they were, and is owed once they unwind
    // A refresh was asked for while they were, and is likewise owed. Three doors reach fetchAll mid-install
    // and two of them have ALREADY changed the registry list by then (a registry added, a registry removed),
    // so a refusal that merely says "not now" leaves the cards on screen describing a set of registries that
    // no longer exists. finishInstall() posts the refused fetchAll rather than calling it — see the note
    // there; calling it directly is the use-after-free the fetchAll guard exists to prevent.
    bool refreshPending_ = false;
};
