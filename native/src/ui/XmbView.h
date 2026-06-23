// A PlayStation 3 "XrossMediaBar" (XMB) view, built from the ground up. A horizontal row of category
// tiles (the media types) crosses a vertical column of the active category's items. Both axes slide so
// the active element stays pinned at the cross. Left/Right move between categories (at the root), Up/Down
// move through the active category's items, Enter opens, Backspace/Left(drilled-in) goes back.
// Used when the active theme's layout is "xmb".
#pragma once
#include <QWidget>
#include <QVector>
#include <QList>
#include <QColor>
#include <QString>
#include <QPixmap>

class QVariantAnimation;
class QNetworkAccessManager;

struct XmbEntry
{
    QString navKey;    // category navKey, or "item:<index>" for an item
    QString label;
    QColor color;
    QString imageUrl;  // box art for items (http/local); empty -> a coloured tile
};

class XmbView : public QWidget
{
    Q_OBJECT
public:
    explicit XmbView(QWidget* parent = nullptr);

    void setCategories(const QVector<XmbEntry>& cats, const QString& activeKey = QString());
    void setActiveCategory(const QString& navKey);            // sync/animate to a category WITHOUT emitting
    void setItems(const QVector<XmbEntry>& items, const QString& selectKey = QString());
    void addItems(const QVector<XmbEntry>& more);             // append a page (no view disruption)
    void clearItems();                                        // empty the column (e.g. while a category loads)
    void setAtRoot(bool atRoot);                              // root: Left/Right switch categories; else Left=back

    QString currentItemKey() const;
    QString activeCategoryKey() const;

signals:
    void activated(const QString& itemKey);          // Enter on the focused item
    void categoryChanged(const QString& navKey);     // moved to another category (root only)
    void backRequested();
    void navUpOffTop();                              // Up pressed at the top item -> leave for the chrome row
    void currentChanged(int itemIndex, int total);   // focused item moved (drives load-more)

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void animateCatTo(int index);
    void animateItemTo(int index);
    void queueItemImage(int i);
    void pumpImages();

    // Categories (horizontal axis). Rendered as coloured tiles; no async art.
    QVector<XmbEntry> cats_;
    int catIndex_ = 0;
    double catPos_ = 0.0;
    QVariantAnimation* catAnim_ = nullptr;

    // Items of the active category (vertical axis). Box art loaded async, like the carousel.
    QVector<XmbEntry> items_;
    QVector<QPixmap> itemImg_;
    QVector<char> itemReq_;
    int itemIndex_ = 0;
    double itemPos_ = 0.0;
    QVariantAnimation* itemAnim_ = nullptr;

    bool atRoot_ = true;
    int gen_ = 0;            // bumped on setItems to drop stale image loads
    QList<int> imgQueue_;
    int imgActive_ = 0;
    QNetworkAccessManager* nam_ = nullptr;
};
