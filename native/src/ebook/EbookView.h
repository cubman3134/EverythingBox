// Page-by-page EPUB reader view: a QTextBrowser renders each chapter's XHTML (images/inline CSS resolved
// from the extracted book folder), with prev/next-page flipping, font sizing, and a contents panel that
// jumps between chapters. Reading position + font size persist per book. Mirrors the Unity ereader UX.
#pragma once
#include <QWidget>
#include "EpubBook.h"

class QTextBrowser;
class QListWidget;
class QLabel;
class QSplitter;
class QUrl;

class EbookView : public QWidget
{
    Q_OBJECT
public:
    explicit EbookView(QWidget* parent = nullptr);

    bool openBook(const QString& path, QString* error = nullptr);
    void persist(); // save reading position (called when navigating away)

signals:
    void homeRequested();

protected:
    void keyPressEvent(QKeyEvent*) override;

private slots:
    void nextPage();
    void prevPage();
    void biggerFont();
    void smallerFont();
    void toggleContents();
    void onTocActivated();
    void onAnchorClicked(const QUrl& url); // follow an in-book hyperlink to its chapter/anchor
    void updatePageLabel();

private:
    void loadChapter(int index, bool toBottom = false);
    void applyFont();
    void restoreState();

    EpubBook book_;
    QTextBrowser* browser_ = nullptr;
    QListWidget* tocList_ = nullptr;
    QSplitter* split_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    int chapter_ = -1;
    int fontPt_ = 14;
};
