// In-book lookup's settings half (issue #137), the MainWindow end — a SEPARATE translation unit that defines
// MainWindow's #137 members, for the reason MainWindowPlayOn.cpp gives: MainWindow.cpp is the single busiest
// merge surface in the repository, and a feature that can be self-contained should cost it two insertions
// rather than two hundred lines.
//
// WHAT LIVES HERE, AND WHAT DELIBERATELY DOES NOT. The lookup itself — the verbs, the card, the network — is
// in the READER (EbookView), because that is where the reader is when they want it, and putting it there is
// what makes the feature identical on the themed layout and the classic one. What is here is only the part
// that has to be reachable from Settings: the count for the row's label, and the browsable list the row opens.
//
// THE LIST IS THE WHOLE OF v1. The issue is explicit that flashcard-mode gamification is not this increment —
// "a reviewable list is the honest 90%". So: the words, most recent first, each opening a card with what the
// lookup said, which book it was met in and the sentence around it, and a Remove. No review state, no
// scheduling, no due dates — because a scheduler nobody asked for is a scheduler nobody maintains.
//
// NAV KIT ONLY. NavMenu and NavConfirm, never a QDialog or a QMessageBox — probe_nav gates that, and a list
// this long is exactly the surface that tempts somebody into a real dialog.
#include "MainWindow.h"

#include <QStringList>
#include <QTimer>

#include "../core/LookupRequest.h"
#include "../core/VocabularyStore.h"
#include "nav/NavOverlay.h"

int MainWindow::vocabularyWordCount() const
{
    return VocabularyStore::count();
}

// The word list. One NavMenu of rows, each "word — the first line of what was said about it", so the list is
// scannable without opening anything; choosing a row opens the full card.
void MainWindow::openVocabularyList()
{
    const QVector<VocabularyStore::Word> words = VocabularyStore::all();
    if (words.isEmpty())
    {
        new NavConfirm(tr("Words I looked up"),
                       tr("Nothing yet. Select a word while reading a book and choose Define, Wikipedia or "
                          "Translate — the words you look up are collected here."),
                       QStringList() << tr("OK"), 0, this);
        return;
    }

    QStringList rows;
    QStringList ids;
    for (const VocabularyStore::Word& w : words)
    {
        // The first line only: a definition runs to several senses, and a menu row is one line. The full text
        // is one press away in the card below.
        const QString head = w.definition.section(QLatin1Char('\n'), 0, 0).simplified();
        rows << (head.isEmpty() ? w.word : QStringLiteral("%1 — %2").arg(w.word, head));
        ids  << w.id;
    }

    new NavMenu(tr("Words I looked up"), rows, [this, ids](int row) {
        if (row < 0 || row >= ids.size()) return;
        const VocabularyStore::Word w = VocabularyStore::byId(ids.at(row));
        if (w.id.isEmpty()) return;

        QStringList body;
        body << w.definition;
        if (!w.context.isEmpty())   body << tr("In the book: %1").arg(w.context);
        if (!w.bookTitle.isEmpty()) body << tr("From: %1").arg(w.bookTitle);
        body << tr("Looked up with %1 (%2).").arg(w.source, w.lang);

        auto* card = new NavConfirm(w.word, body.join(QStringLiteral("\n\n")),
                                    QStringList() << tr("Remove") << tr("Close"), 1, this);
        connect(card, &NavOverlay::closed, this, [this, id = w.id](int r) {
            if (r != 0) return;
            VocabularyStore::remove(id);
            // Deferred one turn of the loop: this runs INSIDE the card's dismiss(), and opening the next
            // overlay from under an emission is the #28/#211 shape. A zero timer puts it after the frame.
            QTimer::singleShot(0, this, [this] { openVocabularyList(); });
        });
    }, this);
}
