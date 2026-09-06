// VocabularyStore — the words a reader looked up (issue #137), per profile. The MODEST version the issue
// scopes: a reviewable list of the word, the definition that was shown, and the book and the sentence it was
// met in. NO flashcards, no scheduling, no review state — a list is the whole of v1, and inventing a
// scheduler here would be inventing the half of the feature that was deliberately deferred.
//
// SHAPE — the highlights store, twin for twin, which is itself the bookmarks/favourites shape. Per-PROFILE,
// the {items, tombs} layout with a real delete tombstone, on the shared portable everythingbox.ini:
//   vocabulary/<profile>/items -> a JSON array of {id, word, lang, source, definition, bookKey, bookTitle,
//                                                  context, spine, offset, ts}
//   deleted/vocabulary/<profile>/<md5(id)> -> the delete tombstone for a removed word (Tombstones)
// The CloudMerge section is the same union-by-id / newest-ts / tombstone-at-or-after rule.
//
// IDENTITY IS THE WORD, not the occurrence: md5(lowercased word | language). Looking the same word up twice —
// in a second book, on a second device, or on the second time it defeated you — UPDATES the one row rather
// than growing a duplicate, which is what makes the list a vocabulary rather than a log. The consequence is
// deliberate: the row remembers where you LAST met the word.
//
// CLOUD SYNC. "vocabulary/" is in CloudSync::isPerItemStoreKey (NOT isDeviceLocalKey): a word you learned is
// a fact about you, not about this machine, so it rides the lightweight CloudMerge document with the
// bookmarks and highlights. probe_lookup pins the classification and the round trip.
#pragma once
#include <QString>
#include <QVector>
#include <functional>

namespace VocabularyStore
{
    struct Word
    {
        QString id;          // stable merge id = md5(lowercased word | lang); the WORD, not the occurrence
        QString word;        // as the reader selected it, case and all — what the list shows
        QString lang;        // the normalized language code the lookup was made in ("en", "fr", …)
        QString source;      // which verb produced `definition`: "Define" / "Wikipedia" / "Translate"
        QString definition;  // the text the card showed, stored so the list is reviewable offline
        QString bookKey;     // the book's stable natural key (its source path / addon item id)
        QString bookTitle;   // for the list row, so a review does not have to resolve a path
        QString context;     // the sentence around the selection (LookupRequest::contextAround)
        int     spine  = -1; // the anchor this word was met at: chapter …
        int     offset = -1; // … and character offset, so the list can jump back to it
        qint64  ts = 0;      // epoch seconds of the last write (multi-device merge: newest-ts wins per id)
    };

    // The ini key the active profile's word list lives under, and the tombstone store namespace for it —
    // named here so the store and CloudMerge cannot drift on the spelling.
    QString itemsKey();
    QString tombstoneStore();

    // The deterministic merge id for a word in a language. Case-folded and independent of the book, the verb
    // and the time, so the same word always maps to the same row. An empty word -> an empty id.
    QString idFor(const QString& word, const QString& lang);

    // Record (or refresh) a looked-up word, stamped now. A word already in the list is UPDATED in place — the
    // newest definition, book and context win — rather than appended a second time. Clears any delete
    // tombstone, so a word removed and looked up again comes back. An empty word is a no-op.
    Word add(const Word& w);

    // Remove a word and record a delete tombstone, so a peer holding it cannot resurrect it on merge.
    void remove(const QString& id);

    // The active profile's words, MOST RECENT FIRST — the order a list of what you just met wants to be in.
    QVector<Word> all();
    int count();

    // One word by id; a default-constructed Word (empty id) when there is none.
    Word byId(const QString& id);

    // Multi-device sync trigger, mirroring BookmarkStore/HighlightStore::setChangeHook: fired after every
    // mutation so MainWindow can (re)arm the debounced push. QtCore-clean; unset in probes.
    void setChangeHook(std::function<void()> hook);
}
