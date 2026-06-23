// Page-by-page PDF reader. Renders with Qt's PDF module (QPdfDocument + QPdfView), which embeds PDFium -
// so it renders faithfully and works on every platform Qt targets (the cross-platform replacement for the
// Unity Docnet/PDFium path). Prev/Next flip pages; +/- and Fit Width zoom; last page persists per file.
#pragma once
#include <QWidget>

class QPdfDocument;
class QPdfView;
class QLabel;

class PdfView : public QWidget
{
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);

    bool openPdf(const QString& path, QString* error = nullptr);
    void persist(); // save the current page (called when navigating away)

signals:
    void homeRequested();

protected:
    void keyPressEvent(QKeyEvent*) override;

private slots:
    void nextPage();
    void prevPage();
    void zoomIn();
    void zoomOut();
    void fitWidth();
    void updateLabel();

private:
    QPdfDocument* doc_ = nullptr;
    QPdfView* view_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    QString path_;
    qreal zoom_ = 1.0;
};
