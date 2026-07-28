// The 4-digit numeric entry pad (issue #30): a NavOverlay, so it is an in-window child of the main window
// like every other EB prompt — never a QDialog/QMessageBox/QInputDialog and never a top-level window
// (probe_nav gates that). Arrow keys walk the 3x4 digit grid, Enter presses a key, pad Back deletes a digit
// and backs out once empty, and a physical keyboard's number row types straight in.
//
// WHY NOT JUST Osk WITH Password ECHO — which is what the parental PIN uses. A full QWERTY grid to enter
// four digits is 40 keys to walk past on a D-pad, and the user cannot see how many digits they have entered
// without counting bullets in a text field. This pad shows four boxes, auto-submits on the fourth digit
// (no "Done" to hunt for), and is nine keys wide. The parental PIN keeps the OSK deliberately: it is free-
// form text, not four digits, so it could not be typed on this pad at all.
//
// The entered digits are never echoed as glyphs — a filled box shows a bullet — and never logged.
#pragma once
#include "NavOverlay.h"
#include <QStringList>
#include <functional>

class QLabel;

class PasscodePad : public NavOverlay
{
    Q_OBJECT
public:
    // `extraActions` are the recovery rows the caller decided to offer (ProfilePasscode::entryOptions owns
    // that decision — this class just renders what it is handed). Picking one closes the pad and reports its
    // INDEX; it is not a passcode outcome, so the code is discarded.
    //
    // onDone(code, accepted, extraIndex):
    //   accepted == true                 -> `code` holds kLength digits (auto-submitted).
    //   accepted == false, extraIndex >=0 -> the user chose that recovery row.
    //   accepted == false, extraIndex <0  -> backed out.
    PasscodePad(const QString& title, const QString& message, const QStringList& extraActions,
                const std::function<void(const QString&, bool, int)>& onDone, QWidget* window = nullptr);

    // Blocking prompt (the NavConfirm::ask / Osk::getText shape). Returns the entered digits, or a NULL
    // QString when the user backed out OR chose a recovery row — `*extraChosen` (when non-null) tells the
    // two apart: -1 for a back-out, else the index into `extraActions`.
    static QString ask(const QString& title, const QString& message, const QStringList& extraActions,
                       int* extraChosen, QWidget* window = nullptr, class NavGraph* graph = nullptr);

    QString describe() const override;   // UI-test channel: how many boxes are filled, NEVER the digits

protected:
    bool handleNavKey(int key) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void insertDigit(const QString& d);
    void backspaceDigit();
    void refreshBoxes();
    void finish(bool accepted, int extraIndex);

    QString entered_;
    QVector<QLabel*> boxes_;
    std::function<void(const QString&, bool, int)> onDone_;
    bool done_ = false;   // finish() runs at most once (auto-submit + a queued key must not both fire)
};
