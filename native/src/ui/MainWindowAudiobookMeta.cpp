// AUDIOBOOK METADATA MATCHING (issue #198), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow's #198 members, for the reason MainWindowPlayOn.cpp states: MainWindow.cpp is 25,000 lines and
// the busiest merge surface in the repository, and everything below reaches the rest of MainWindow only
// through members that already existed. It costs MainWindow.cpp three short insertions instead of two
// hundred lines.
//
// The whole feature in one paragraph. #139's scan produces ENTRIES (what the tags said) and an INDEX built
// from them; both are now installed (AudiobookLibrary::installScanEntries). After each scan the sweep asks
// every addon that declared `metaFor: ["audiobook"]` about each book whose tags left a blank, scores what
// comes back, and stores a match. Storing one re-derives the browse index FROM THE ENTRIES with the matches
// applied — never from the previous index, and never by writing anything into the persisted one — so a
// filled-in narrator is filed under a real Narrators bucket by exactly the code that files a tagged one,
// and a rejection puts everything back by re-deriving without it.
//
// THE ONE INVARIANT WORTH RESTATING HERE: local tags always win, and the persisted index holds tags only.
// AudiobookMeta.h argues both at length. This file's job is to make sure the RE-DERIVATION always starts
// from AudiobookLibrary::scanEntries() and never from an already-enriched vector, because an enrichment
// that fed itself would be indistinguishable from a tag within one scan and unrejectable after two.
#include "MainWindow.h"
#include "HomeView.h"

#include "../addons/AddonManager.h"
#include "../addons/AudiobookMetaAggregator.h"
#include "../browse/AudiobookCatalogs.h"
#include "../core/AudiobookLibrary.h"
#include "../core/AudiobookMatchStore.h"
#include "../core/AudiobookMeta.h"
#include "../core/MetaCache.h"
#include "nav/NavOverlay.h"

#include <QSet>
#include <QStringList>
#include <QTimer>

// The BOOK key behind a metadata-editor key, or "" when that key is not an audiobook at all. The editor is
// keyed by the browse row id (MetaCache::keyFor -> "audiobookbook:<key>"); this store is keyed by the book
// key itself, because the party that asks it — the scan — has entries and no rows.
static QString bookKeyOfMetaKey(const QString& metaKey)
{
    return browse::audiobookKeyOf(metaKey, browse::kAudiobookBookPrefix);
}

// Every book key in the current scan, in one pass and without rebuilding the index to get them.
static QStringList scannedBookKeys()
{
    QStringList keys;
    QSet<QString> seen;
    for (const AudiobookLibrary::FileEntry& e : AudiobookLibrary::scanEntries())
    {
        const QString k = AudiobookLibrary::bookKeyFor(e);
        if (k.isEmpty() || seen.contains(k)) continue;
        seen.insert(k);
        keys << k;
    }
    return keys;
}

// Re-derive and install the browse index from the SCANNED ENTRIES with every stored match applied. Always
// from the entries, never from the index on screen: an enrichment that fed itself would be indistinguishable
// from a tag within one scan and unrejectable after two.
void MainWindow::applyAudiobookMatches()
{
    if (AudiobookLibrary::scanEntries().isEmpty()) return;
    const QHash<QString, AudiobookMeta::Match> matches = AudiobookMatches::forBooks(scannedBookKeys());
    QVector<AudiobookLibrary::FileEntry> entries = AudiobookLibrary::scanEntries();   // a COPY, always
    AudiobookMeta::applyToEntries(entries, matches);
    AudiobookLibrary::installIndex(AudiobookLibrary::buildIndex(entries));
    if (home_) home_->onAudiobookLibraryChanged();
}

// COALESCED, because a sweep of a large library finishes one book at a time and each finish would otherwise
// re-group and re-sort the whole index on the GUI thread. One rebuild per quiet moment instead of one per
// book; the flag is what makes a burst of fifty matches cost a single rebuild.
void MainWindow::scheduleAudiobookMatchApply()
{
    if (audiobookApplyPending_) return;
    audiobookApplyPending_ = true;
    QTimer::singleShot(500, this, [this] { audiobookApplyPending_ = false; applyAudiobookMatches(); });
}

// Ask the providers about the books whose tags left blanks. Dormant — and free — when no addon declares
// itself an audiobook metadata provider, which is the out-of-the-box state.
void MainWindow::sweepAudiobookMatches()
{
    if (!addons_) return;
    if (AudiobookLibrary::index().isEmpty()) return;
    if (!audiobookMeta_)
    {
        audiobookMeta_ = new AudiobookMetaAggregator(addons_.get(), this);
        // Re-derive from EVERY stored match rather than applying the one that just arrived: a listener that
        // missed a signal is still correct on the next, and there is one code path deciding what the index
        // holds.
        connect(audiobookMeta_, &AudiobookMetaAggregator::matchesChanged, this,
                [this](const QString&) { scheduleAudiobookMatchApply(); });
    }
    audiobookMeta_->sweep(AudiobookLibrary::index());
}

// The metadata editor's extra row for an audiobook that carries a match, or "" for anything else — which is
// every film, every game, and every audiobook nothing was matched for. The row NAMES the match, because the
// point of surfacing a confidence is that the user can see what they are rejecting.
QString MainWindow::audiobookMatchEditorRow(const QString& metaKey) const
{
    const QString bookKey = bookKeyOfMetaKey(metaKey);
    if (bookKey.isEmpty()) return QString();
    const AudiobookMeta::Match m = AudiobookMatches::get(bookKey);
    if (m.rejected || !m.hasFields()) return QString();
    return tr("✗  Reject match:  %1").arg(AudiobookMeta::matchSummary(m));
}

// "No, that is not this book." Confirms, records the rejection permanently (AudiobookMatchStore.h), drops
// the cached cover/description the match put in MetaCache, and re-derives the index without it. Returns
// true when something actually changed, so the caller knows whether to refresh.
bool MainWindow::rejectAudiobookMatch(const QString& metaKey)
{
    const QString bookKey = bookKeyOfMetaKey(metaKey);
    if (bookKey.isEmpty()) return false;
    const AudiobookMeta::Match m = AudiobookMatches::get(bookKey);
    if (m.rejected || !m.hasFields()) return false;

    const int c = NavConfirm::ask(
        tr("Reject this match"),
        tr("Go back to what this book's own tags say, and stop matching it online?"),
        { tr("Cancel"), tr("Reject") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (c != 1) return false;

    AudiobookMatches::reject(bookKey);
    MetaCache::remove(metaKey);        // the cover and description this match cached; nothing else is there
    applyAudiobookMatches();           // ...and the index goes back to exactly what the scan found
    return true;
}
