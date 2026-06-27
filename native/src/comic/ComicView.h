// Page-by-page comic reader for CBZ archives (a ZIP of page images). Opens the archive with miniz, sorts
// the image entries in natural page order, and shows one page at a time with fit-width / zoom and per-file
// resume - mirroring the PDF reader. (CBR/CB7/CBT would need a RAR/7z/tar decoder; CBZ covers most comics.)
#pragma once
#include <QWidget>
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QString>

class QScrollArea;
class QLabel;

class ComicView : public QWidget
{
    Q_OBJECT
public:
    explicit ComicView(QWidget* parent = nullptr);

    bool openComic(const QString& path, QString* error = nullptr);
    void persist(); // save the current page (called when navigating away)

    static bool isComicFile(const QString& path); // .cbz (and plain .zip of images)

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (e.g. the chapter list) without resetting Home

protected:
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private slots:
    void nextPage();
    void prevPage();
    void zoomIn();
    void zoomOut();
    void fitWidth();

private:
    void showPage(int index);
    void rescale();
    void updateLabel();

    bool spreadActive() const; // currently showing two pages side by side (book mode)

    QVector<QByteArray> pages_; // each entry = one page's encoded image bytes (jpg/png/…)
    int current_ = 0;
    QImage image_;             // the decoded current page
    qreal zoom_ = 1.0;
    bool fit_ = true;          // fit-to-width vs. manual zoom
    bool twoUp_ = false;       // viewport is wide enough to pair pages book-style (set during rescale)
    QString path_;

    QScrollArea* scroll_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    QLabel* pageLabel_ = nullptr;
};
