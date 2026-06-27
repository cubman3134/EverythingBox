// Page-by-page PDF reader. Renders with Qt's PDF module (QPdfDocument + QPdfView), which embeds PDFium -
// so it renders faithfully and works on every platform Qt targets (the cross-platform replacement for the
// Unity Docnet/PDFium path). Prev/Next flip pages; +/- and Fit Width zoom; last page persists per file.
#pragma once
#include <QWidget>

class QPdfDocument;
class QPdfView;
class QLabel;
class QPushButton;

class PdfView : public QWidget
{
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);

    bool openPdf(const QString& path, QString* error = nullptr);
    void persist(); // save the current page (called when navigating away)
    void setStreamIssueVisible(bool on); // show the "Issue with Streaming" button (remote/Allarr books only)

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (the catalog/list) without resetting Home
    void streamIssueRequested(); // user reports a bad file -> ask the provider for the next source

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
    QPushButton* streamIssueBtn_ = nullptr; // "Issue with Streaming" (hidden unless a remote book)
    QString path_;
    qreal zoom_ = 1.0;
};
