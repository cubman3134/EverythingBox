// Page-by-page PDF reader. Renders with Qt's PDF module (QPdfDocument + QPdfView), which embeds PDFium -
// so it renders faithfully and works on every platform Qt targets (the cross-platform replacement for the
// Unity Docnet/PDFium path). Prev/Next flip pages; +/- and Fit Width zoom; last page persists per file.
#pragma once
#include <QWidget>
#include "../theme2/HostedReader.h"

class QPdfDocument;
class QPdfView;
class QLabel;
class QPushButton;

class PdfView : public QWidget, public HostedReader
{
    Q_OBJECT
public:
    explicit PdfView(QWidget* parent = nullptr);

    bool openPdf(const QString& path, QString* error = nullptr);
    void persist(); // save the current page (called when navigating away)
    void setStreamIssueVisible(bool on); // show the "Issue with Streaming" button (remote/Allarr books only)

    // ---- Hosted mode (themed reader chrome, Plan B1 Task 4) ----------------------------------------------
    // Mirrors EbookView's contract: setHostedChrome(true) hides the reader's own bottom control bar so the
    // themed ReaderChromeHost strips drive everything through the thin wrappers below. The wrappers wrap what
    // the bar buttons already call — ZERO render/scroll change. Settings for a PDF are zoom in/out + fit width
    // (no font, no toc). pageInfoChanged() lets the chrome mirror page/zoom moved by raw keys or the buttons.
    QWidget* asWidget() override { return this; }
    void setHostedChrome(bool on) override;
    int  currentPage() const override;   // 1-based
    int  pageCount()  const override;
    int  chromeTopReserve() const override { return 38; } // themed top strip height (no reserved page inset)
    void zoomDelta(int steps) override;  // + = zoom in, - = zoom out (per step, matching the +/- buttons)
    void fitWidth() override;

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (the catalog/list) without resetting Home
    void streamIssueRequested(); // user reports a bad file -> ask the provider for the next source
    void pageInfoChanged(); // page/zoom changed (paged, zoomed, resumed) — hosted chrome refresh

public slots:
    void nextPage() override;
    void prevPage() override;

protected:
    void keyPressEvent(QKeyEvent*) override;

private slots:
    void zoomIn();
    void zoomOut();
    void updateLabel();

private:
    QPdfDocument* doc_ = nullptr;
    QPdfView* view_ = nullptr;
    QWidget* bar_ = nullptr;                // the bottom control bar (hidden in hosted/themed mode)
    QLabel* pageLabel_ = nullptr;
    QPushButton* streamIssueBtn_ = nullptr; // "Issue with Streaming" (hidden unless a remote book)
    QString path_;
    qreal zoom_ = 1.0;
    bool streamVisible_ = false;            // remembered stream-issue visibility (restored leaving hosted mode)
    bool hosted_ = false;
};
