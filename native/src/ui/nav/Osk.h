// The on-screen keyboard: a NavOverlay with a key grid, so every text box in MMV is typeable from the
// couch. Arrow-selecting a QLineEdit row and pressing Enter opens it (NavOverlay::editLineEdit); flows
// that need a value inline call Osk::getText (nested-loop, QInputDialog-style drop-in).
//
// Controls: arrows move over the keys, Enter presses one, pad Back (Backspace) deletes a character —
// and backs out once the text is empty — Start (Escape) commits. A physical keyboard just types into
// it directly (the overlay's keyboard grab feeds keyPressEvent), so desktop users aren't slowed down.
#pragma once
#include "NavOverlay.h"
#include <QLineEdit>

class Osk : public NavOverlay
{
    Q_OBJECT
public:
    // onDone(text, accepted) runs after the overlay closes. echo = QLineEdit::Password for PIN entry.
    Osk(const QString& title, const QString& initial,
        QLineEdit::EchoMode echo, const std::function<void(const QString&, bool)>& onDone,
        QWidget* window = nullptr);

    // Blocking prompt: returns the entered text, or a null QString() when cancelled (an accepted empty
    // string returns "" which is non-null — same contract as promptThemedSearch relies on).
    static QString getText(const QString& title, const QString& initial = QString(),
                           QLineEdit::EchoMode echo = QLineEdit::Normal, QWidget* window = nullptr);

    QString text() const { return preview_->text(); }

protected:
    bool handleNavKey(int key) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void insert(const QString& s);
    void backspaceChar();
    void relabel();          // apply the shift/symbols state to the key captions
    void accept();
    QLineEdit* preview_ = nullptr;
    QVector<class QPushButton*> charKeys_; // keys that carry a "ch" property (relabelled by shift/symbols)
    bool shift_ = false;
    bool symbols_ = false;
    std::function<void(const QString&, bool)> onDone_;
};
