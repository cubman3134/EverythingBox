// A spinning horizontal carousel (à la EmulationStation / RetroBat "system view"). Shows a row of coloured
// cards; the centre one is enlarged + highlighted. Arrow keys / wheel / click spin it; Enter opens the
// centred entry. Used for the media-type layer when the active theme's layout is "carousel".
#pragma once
#include <QWidget>
#include <QVector>
#include <QList>
#include <QColor>
#include <QString>
#include <QPixmap>

class QVariantAnimation;
class QNetworkAccessManager;

struct CarouselEntry
{
    QString navKey;    // identifies the entry (the app maps it to a catalog/Home)
    QString label;
    QColor color;
    QString imageUrl;  // box art (http or local path); empty -> a coloured tile
};

class CarouselView : public QWidget
{
    Q_OBJECT
public:
    explicit CarouselView(QWidget* parent = nullptr);
    void setEntries(const QVector<CarouselEntry>& entries, const QString& selectKey = QString());
    void addEntries(const QVector<CarouselEntry>& more); // append (paged catalogs) without disturbing the view
    void setWrap(bool wrap); // infinite tiling (finite lists) vs clamped at both ends (paged catalogs)
    QString currentKey() const;

signals:
    void activated(const QString& navKey);       // Enter / click the centre card
    void backRequested();                        // Esc / Backspace
    void navUp();                                // Up pressed -> hand off to the top chrome row
    void currentChanged(int wrappedIndex, int total); // centre moved (drives load-more)

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void goTo(int index, bool animate);
    int indexNearestX(int x) const;
    int wrappedIndex() const;
    void queueImage(int i);
    void pumpImages();

    QVector<CarouselEntry> entries_;
    QVector<QPixmap> images_;     // loaded box art, parallel to entries_
    QVector<char> requested_;     // whether an image fetch was already started
    QList<int> imgQueue_;         // pending remote image loads
    int imgActive_ = 0;
    int gen_ = 0;                 // bumped on setEntries to ignore stale image loads
    bool wrap_ = true;            // false -> clamp at both ends (paged catalogs)
    int index_ = 0;
    double pos_ = 0.0;            // animated continuous position in index space
    QVariantAnimation* anim_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
};
