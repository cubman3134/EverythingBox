// ONLINE BLANK-FILLING FOR THE READING LIBRARY (issue #134 increment 2), the MainWindow half — a SEPARATE
// translation unit that defines MainWindow's #134-increment-2 members, for the reason MainWindowPlayOn.cpp
// and MainWindowAudiobookMeta.cpp both state: MainWindow.cpp is the busiest merge surface in the repository
// and everything below reaches the rest of it only through members that already existed.
//
// THE WHOLE FEATURE IN ONE PARAGRAPH. With one setting on — default OFF — a book whose FILE said no author
// or carried no cover may be asked about online: getMeta to every addon that answers book metadata, and the
// first non-empty answer's cover and author are put in MetaCache under the book's own row key. Nothing is
// asked about a book whose file already says both. Nothing at all is asked with the setting off. Nothing is
// ever written into the scan, the index or the ini.
//
// THE TWO DECISIONS THAT MAKE THAT SAFE ARE NOT HERE. `BookLibrary::enrichmentTargets` decides who may be
// asked and `BookLibrary::acceptedFill` decides what of an answer may be used, and both are pure functions
// with a probe over them — which is what turns "nothing is requested with the setting off" and "local
// metadata always wins" from comments above a network call into assertions over a whole fixture library.
// This file is the wire between them and the addon host, and it is deliberately the only part with no
// judgement in it.
//
// WHY IT DOES NOT RE-GROUP THE LIBRARY. A filled-in author is DISPLAY. The Authors buckets come from the
// scan and from nothing else, so an answer cannot move a book between shelves, cannot be mistaken for
// something its file said, and cannot feed itself on the next sweep. #198 took the other road for
// audiobooks — a scored match store, a re-derivation, a reject door — because a mis-tagged audiobook's
// NARRATOR is a browse dimension somebody navigates by. A bare PDF's blank cover is not, and the issue's
// own words for this are "strictly optional blank-filling".
//
// A FAILURE IS SILENT. No error row, no notify, no retry: the book keeps the blank it already had, which is
// exactly what it looked like before this existed. The provider set being empty is the same non-event, so
// an install with no book-metadata addon runs this at the cost of one empty vector per scan.
#include "MainWindow.h"
#include "HomeView.h"

#include "../addons/AddonManager.h"
#include "../core/BookLibrary.h"
#include "../core/MetaCache.h"
#include "../core/MusicArt.h"
#include "../core/Settings.h"

#include <QFileInfo>

namespace
{
// THE PROVIDERS THIS ASKS. "book" is the type a book-metadata addon declares in `metaFor`, and it is asked
// for first. "audiobook" is asked for too, and the reason is worth stating rather than hiding: the two
// providers this app ships for that type — Open Library and Google Books — are BOOK catalogues that #198
// happened to reach first, they answer a title/author/cover for a book exactly as well as for an audiobook,
// and a bundled manifest is copy-if-absent, so gating purely on a freshly added "book" string would leave
// this feature dormant on every install that already exists. Anything an addon declares either way is an
// equal provider; nothing is compiled in.
const char* kBookMetaType      = "book";
const char* kAudiobookMetaType = "audiobook";

// THE COVER QUESTION, asked the way the browse asks it, so "this book has no cover" means the same thing
// here as it does on the shelf: what the container carried, else a cover.*/folder.* beside the file. It is
// deliberately NOT "MetaCache has nothing for it" — that would make an already-filled book a target for
// ever, and re-ask about it on every scan.
bool bookHasCover(const BookLibrary::Book& b)
{
    if (b.hasCover) return true;
    static const QString dir = MusicArt::cacheDir();
    return !MusicArt::keyedCover(b.key, b.folder, dir).isEmpty();
}
} // namespace

int MainWindow::sweepBookMetadata()
{
    if (!addons_) return 0;

    // THE GATE, and it is the FUNCTION's gate rather than an `if` here: with the setting off the target list
    // is empty, so there is nothing below this line to iterate and no request can be constructed. That is
    // the shape the probe asserts against a whole fixture library.
    const QVector<BookLibrary::Book> targets =
        BookLibrary::enrichmentTargets(BookLibrary::index(), bookHasCover, Settings::booksEnrichOnline());
    if (targets.isEmpty()) return 0;

    QVector<LoadedAddon*> providers = addons_->metaProvidersFor(QString::fromLatin1(kBookMetaType));
    for (LoadedAddon* p : addons_->metaProvidersFor(QString::fromLatin1(kAudiobookMetaType)))
        if (!providers.contains(p)) providers.push_back(p);
    if (providers.isEmpty()) return 0;   // dormant: nobody answers book metadata on this install

    int asked = 0;
    for (const BookLibrary::Book& b : targets)
    {
        if (bookMetaAsked_.contains(b.key)) continue;   // asked once this session, answered or not
        bookMetaAsked_.insert(b.key);

        // WHAT THE PROVIDER IS ASKED. The book's own title — which for an untagged file is its FILE NAME,
        // and that is the honest thing to search on rather than something derived from it — and, when the
        // file named one, the author as the secondary term. No id: this app has never resolved this file to
        // any catalogue, and minting one would be a claim about provenance that is not true.
        MediaItem q;
        q.title    = b.title;
        q.subtitle = b.author.trimmed();

        bool issued = false;
        for (LoadedAddon* p : providers)
        {
            // ASK EACH PROVIDER IN THE TYPE IT DECLARED. A bundled addon is copy-if-absent, so an install
            // that already exists is still carrying the manifest and script that only know "audiobook" —
            // sending it "book" would be a question it answers with silence, for ever. Reading the type off
            // the manifest means a fresh install is asked the new way, an old one the old way, and neither
            // needs to know the other exists.
            q.type = p->manifest.metaFor.contains(QString::fromLatin1(kBookMetaType))
                         ? QString::fromLatin1(kBookMetaType)
                         : QString::fromLatin1(kAudiobookMetaType);
            const int reqId = addons_->requestMeta(p, q);
            if (reqId < 0) continue;
            bookMetaReq_.insert(reqId, b.key);
            issued = true;
        }
        if (issued) ++asked;
    }
    return asked;
}

void MainWindow::onBookMetaReady(int requestId, const MediaDetail& detail)
{
    const auto it = bookMetaReq_.constFind(requestId);
    if (it == bookMetaReq_.constEnd()) return;   // not ours: the game / audiobook aggregators own their ids
    const QString bookKey = it.value();
    bookMetaReq_.erase(it);

    // RE-READ THE BOOK, rather than trusting a copy captured when the request went out: a rescan may have
    // landed in between, and the answer must be measured against what the library says NOW. A book that is
    // gone is an answer with nowhere to go.
    const BookLibrary::Book* b = BookLibrary::index().book(bookKey);
    if (!b) return;

    BookLibrary::Fill offered;
    // THE ANSWER'S OWN TITLE IS THE EVIDENCE, not a field to be filled — acceptedFill drops the whole reply
    // when it does not corroborate. A book catalogue answers EVERY search with its best guess, so an answer
    // taken on trust puts a stranger's name and cover under somebody's untagged scan.
    offered.title       = detail.title.trimmed();
    offered.author      = detail.subtitle.trimmed();   // the addons put a book's author here
    offered.coverUrl    = detail.imageUrl.trimmed();
    offered.description = detail.overview.trimmed();
    if (!detail.art.image(QStringLiteral("poster")).isEmpty() && offered.coverUrl.isEmpty())
        offered.coverUrl = detail.art.image(QStringLiteral("poster"));

    // ...AND THE FILTER IS THE PURE ONE. Everything the file already said is dropped here and cannot reach
    // the store below, which is "local metadata always wins" enforced rather than intended. A provider that
    // answered nothing useful leaves an empty Fill and nothing at all is written — the blank stays a blank,
    // which is exactly what a failure looks like too.
    const BookLibrary::Fill use = BookLibrary::acceptedFill(*b, bookHasCover(*b), offered);
    if (use.isEmpty()) return;

    // The book's BROWSE ROW KEY is its book key (MetaCache::keyFor over a book row returns the row's id,
    // which is exactly this), so what is stored here is found again by the cover resolver and by every
    // metadata surface that already reads a card by key. No new store, no new key scheme.
    if (!use.coverUrl.isEmpty())
    {
        MediaArt art;
        art.addImage(QStringLiteral("poster"), use.coverUrl);
        MetaCache::saveArt(bookKey, art);
    }
    MediaDetail card = MetaCache::cachedDetail(bookKey);
    card.title = b->title;
    if (!use.author.isEmpty())      card.subtitle = use.author;
    if (!use.description.isEmpty()) card.overview = use.description;
    if (!use.coverUrl.isEmpty())    card.imageUrl = use.coverUrl;
    card.valid = !card.title.isEmpty() || !card.overview.isEmpty() || !card.subtitle.isEmpty();
    if (card.valid) MetaCache::saveDetail(bookKey, card);

    // The shelf the user is standing on, if it is a Books one. onBookLibraryChanged repopulates the current
    // level and nothing else, which is exactly the right amount of redraw for one filled-in cover.
    if (home_) home_->onBookLibraryChanged();
}
