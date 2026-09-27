// probe_regbrowser — the classic registry browser's list of registries (issue #458).
//
// Two defects, both only visible on screen, both pinned here by constructing the REAL RegistryBrowser under the
// REAL app-wide style sheet (AppStyleSheet::base) on the offscreen QPA:
//
//  1. STALE ROWS. Every add or remove re-renders the registry rows. Each row used to be a bare QHBoxLayout
//     whose label and remove button were parented to the DIALOG; the re-render took the row out of the
//     outer layout and deleted the layout item, which deletes the QHBoxLayout and NOT the widgets in it. So
//     every earlier row's label and button stayed alive, visible, parented and out of any layout, painted at
//     their last geometry over whatever the dialog laid out there next — two "(default)" lines after the
//     first add, a pile of overlapping hosts and blank buttons after a few more.
//     Asserted as: the registry labels that are VISIBLE are exactly the configured list, in order, after
//     every add and every remove, measured once the events a paint would follow have been processed (a new
//     row is SHOWN by a queued call its layout posts, so "right after the press" would see no new row on any
//     build) — and once deferred deletions have run, no registry row widget outlives its row at all.
//
//  2. THE BLANK REMOVE BUTTON. Not a missing glyph: the sheet gives every QPushButton 16px of horizontal
//     padding a side, and the remove button was fixed at 28px, so its contents rect was NEGATIVE and the
//     caption was clipped away entirely. Asserted as a relationship, never as a pixel count taken from this
//     machine's font (the Linux runner's font is a different width): the caption's advance fits the
//     button's contents rect, and rendering the button with its caption differs from rendering it without.
//
// Plus the focus rule a keyboard/pad user depends on: after a remove, focus lands on the row that took the
// removed one's place, else the one above it, else the "Add registry…" button — never on a deleted button.
//
// The registries are loopback fixtures served by this probe (an empty add-on index). EB_ADDON_REGISTRY_URL
// stands the built-in registry in for one, which is honoured only under EB_UITEST, so both are set before
// anything reads them. No real registry is ever contacted.
#include "ui/RegistryBrowser.h"
#include "ui/AppStyleSheet.h"
#include "ui/GlyphButton.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontMetrics>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cstdio>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, what) do { ++g_checks; if (cond) std::printf("  ok   %s\n", what); \
    else { ++g_fail; std::printf("  FAIL %s  [%s:%d]\n", what, __FILE__, __LINE__); } } while (0)

// A loopback registry that answers every path with an empty add-on index, so each fetch the dialog starts
// completes quickly and cleanly and adds no card (a card's labels could otherwise be mistaken for a row's).
struct Fixture
{
    QTcpServer srv;
    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            QObject::connect(c, &QTcpSocket::readyRead, c, [c] {
                if (!c->readAll().contains("\r\n\r\n")) return;
                const QByteArray body = "{\"addons\":[]}";
                c->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                         + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                c->flush();
                c->disconnectFromHost();
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }
    QString url(const QString& path) const
    { return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(path); }
};

static void pump(int ms = 50)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
}

// A registry row's label carries its full index URL as the tooltip (the text is only the host). Those are the
// only labels in the dialog whose tooltip is an http(s) URL — the probe's fixture index yields no cards.
static bool isRowLabel(const QLabel* l) { return l->toolTip().startsWith(QLatin1String("http")); }

static int yIn(const QWidget* w, const QWidget* top) { return w->mapTo(top, QPoint(0, 0)).y(); }

// What is PAINTED: the visible row labels, top to bottom, as their URLs.
static QStringList paintedRows(QWidget* dlg)
{
    if (dlg->layout()) dlg->layout()->activate();
    QVector<QLabel*> ls;
    for (QLabel* l : dlg->findChildren<QLabel*>())
        if (isRowLabel(l) && l->isVisible()) ls << l;
    std::sort(ls.begin(), ls.end(), [dlg](QLabel* a, QLabel* b) { return yIn(a, dlg) < yIn(b, dlg); });
    QStringList out;
    for (QLabel* l : ls) out << l->toolTip();
    return out;
}

static int aliveRowLabels(QWidget* dlg)
{
    int n = 0;
    for (QLabel* l : dlg->findChildren<QLabel*>()) if (isRowLabel(l)) ++n;
    return n;
}

static QList<QPushButton*> visibleRemoveButtons(QWidget* dlg)
{
    QList<QPushButton*> out;
    for (QPushButton* b : dlg->findChildren<QPushButton*>())
        if (b->isVisible() && b->toolTip() == QLatin1String("Remove this registry")) out << b;
    return out;
}

// The remove button on the same row as the visible label for `url`: the visible remove button whose vertical
// centre lies within that label's row band. Geometric, so it pairs the two the same way a user's eye does
// and needs no knowledge of how the dialog builds a row.
static QPushButton* removeButtonFor(QWidget* dlg, const QString& url)
{
    pump(20);
    if (dlg->layout()) dlg->layout()->activate();
    QLabel* lbl = nullptr;
    for (QLabel* l : dlg->findChildren<QLabel*>())
        if (l->isVisible() && l->toolTip() == url) { lbl = l; break; }
    if (!lbl) return nullptr;
    const int ly = yIn(lbl, dlg) + lbl->height() / 2;
    QPushButton* best = nullptr;
    int bestD = 1 << 30;
    for (QPushButton* b : visibleRemoveButtons(dlg))
    {
        const int d = qAbs(yIn(b, dlg) + b->height() / 2 - ly);
        if (d < bestD) { bestD = d; best = b; }
    }
    return (best && bestD <= qMax(lbl->height(), best->height())) ? best : nullptr;
}

static QPushButton* buttonByText(QWidget* dlg, const QString& text)
{
    for (QPushButton* b : dlg->findChildren<QPushButton*>()) if (b->text() == text) return b;
    return nullptr;
}

// The contents rect the STYLE gives a push button's caption. Under a style sheet this is the rule's box minus
// border and padding, which is exactly what clipped the remove button's caption to nothing.
static QRect contentsRectOf(QPushButton* b)
{
    QStyleOptionButton opt;
    opt.initFrom(b);
    opt.text = b->text();
    opt.icon = b->icon();
    opt.iconSize = b->iconSize();
    opt.features = QStyleOptionButton::None;
    if (b->isDefault()) opt.features |= QStyleOptionButton::DefaultButton;
    if (b->isFlat()) opt.features |= QStyleOptionButton::Flat;
    return b->style()->subElementRect(QStyle::SE_PushButtonContents, &opt, b);
}

// Does the caption put any ink on the button? Grab it as it is, grab it with an empty caption, count the
// differing pixels. Independent of which font the machine has: a glyph the font lacks still draws its
// replacement box, which is ink; only a caption clipped to nothing draws none.
static int captionInk(QPushButton* b)
{
    const QString text = b->text();
    const QImage with = b->grab().toImage();
    b->setText(QString());
    const QImage without = b->grab().toImage();
    b->setText(text);
    if (with.size() != without.size()) return -1;
    int n = 0;
    for (int y = 0; y < with.height(); ++y)
        for (int x = 0; x < with.width(); ++x)
            if (with.pixel(x, y) != without.pixel(x, y)) ++n;
    return n;
}

static bool captionFits(QPushButton* b, QString* why)
{
    const QRect cr = contentsRectOf(b);
    const int adv = b->fontMetrics().horizontalAdvance(b->text());
    *why = QStringLiteral("button %1px wide, contents rect %2px, caption advance %3px, caption ink %4px")
               .arg(b->width()).arg(cr.width()).arg(adv).arg(captionInk(b));
    return cr.width() >= adv && captionInk(b) > 0;
}

static QString joined(const QStringList& l) { return l.join(QStringLiteral(" | ")); }

static void expectRows(QWidget* dlg, const QStringList& want, const char* step)
{
    pump(20);   // what the next paint shows: the new rows' queued show has run, and nothing else has
    const QStringList got = paintedRows(dlg);
    const bool same = (got == want);
    const QByteArray what = QByteArray("[") + step + "] exactly the configured registries are painted, in order";
    CHECK(same, what.constData());
    if (!same)
        std::printf("         want: %s\n         got:  %s\n", qPrintable(joined(want)), qPrintable(joined(got)));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("EB_UITEST", "1");            // the fixture below stands in for the built-in registry only under this
    QApplication app(argc, argv);
    app.setStyleSheet(AppStyleSheet::base());   // THE app's sheet — its button padding is half of defect 2

    Fixture fx;
    if (!fx.start()) { std::printf("FAIL: could not listen on loopback\n"); return 1; }
    const QString D  = fx.url(QStringLiteral("/reg0/index.json"));   // the built-in registry's stand-in
    const QString R2 = fx.url(QStringLiteral("/reg2/index.json"));
    const QString R3 = fx.url(QStringLiteral("/reg3/index.json"));
    const QString R4 = fx.url(QStringLiteral("/reg4/index.json"));
    qputenv("EB_ADDON_REGISTRY_URL", D.toUtf8());

    std::printf("== the registry rows are rebuilt, not stacked (issue #458) ==\n");
    RegistryBrowser dlg(RegistryBrowser::Addons, nullptr);
    dlg.resize(640, 560);
    dlg.show();
    dlg.activateWindow();
    pump(100);

    QLineEdit* urlEdit = dlg.findChild<QLineEdit*>();
    QPushButton* addBtn = buttonByText(&dlg, QStringLiteral("Add registry…"));
    CHECK(urlEdit && addBtn, "the dialog has its add-registry line edit and button");
    if (!urlEdit || !addBtn) { std::printf("FAIL: cannot drive the dialog\n"); return 1; }

    auto add = [&](const QString& url) {
        addBtn->click();                  // reveals the inline add row, exactly as a user does
        urlEdit->setText(url);
        emit urlEdit->returnPressed();    // the same commit the Add button and Enter both run
    };

    expectRows(&dlg, { D }, "opened");

    add(R2);
    expectRows(&dlg, { D, R2 }, "added R2");
    add(R3);
    expectRows(&dlg, { D, R2, R3 }, "added R3");

    // Remove the FIRST added registry: the row below it moves up into its place, and so does focus.
    QPushButton* rm2 = removeButtonFor(&dlg, R2);
    CHECK(rm2 != nullptr, "[remove R2] R2's row has a visible remove button");
    if (rm2)
    {
        rm2->setFocus();
        rm2->click();                     // the button is deleted from inside its own clicked() — see the .cpp
        expectRows(&dlg, { D, R3 }, "removed R2");
        pump(20);
        QPushButton* rm3 = removeButtonFor(&dlg, R3);
        CHECK(rm3 && dlg.focusWidget() == rm3,
              "[removed R2] focus moved to the row that took its place (R3's remove button)");
    }

    add(R4);
    expectRows(&dlg, { D, R3, R4 }, "added R4");

    // Remove the LAST row: there is nothing below it, so focus goes to the row above.
    if (QPushButton* rm4 = removeButtonFor(&dlg, R4))
    {
        rm4->setFocus();
        rm4->click();
        expectRows(&dlg, { D, R3 }, "removed R4");
        pump(20);
        QPushButton* rm3 = removeButtonFor(&dlg, R3);
        CHECK(rm3 && dlg.focusWidget() == rm3, "[removed R4] focus moved to the row above (R3's remove button)");
    }
    else CHECK(false, "[remove R4] R4's row has a visible remove button");

    // Remove the only added registry: the default row has no remove button, so focus goes to "Add registry…".
    if (QPushButton* rm3 = removeButtonFor(&dlg, R3))
    {
        rm3->setFocus();
        rm3->click();
        expectRows(&dlg, { D }, "removed R3");
        pump(20);
        CHECK(dlg.focusWidget() == addBtn, "[removed R3] no registry row left to take focus: it goes to \"Add registry…\"");
    }
    else CHECK(false, "[remove R3] R3's row has a visible remove button");

    // Once the deferred deletions run, the earlier rows are GONE, not merely hidden: a re-render that only hid
    // them would leak two widgets per add/remove for the dialog's lifetime.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    pump(20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        const int alive = aliveRowLabels(&dlg);
        CHECK(alive == 1, "after deferred deletion exactly ONE registry row label exists (no stale row outlives its render)");
        if (alive != 1) std::printf("         %d row labels alive\n", alive);
        int buttons = 0;
        for (QPushButton* b : dlg.findChildren<QPushButton*>())
            if (b->toolTip() == QLatin1String("Remove this registry")) ++buttons;
        CHECK(buttons == 0, "…and no remove button survives the rows it belonged to");
        if (buttons) std::printf("         %d remove buttons alive\n", buttons);
    }

    std::printf("== the remove button's caption is on its face (issue #458) ==\n");
    add(R2);
    pump(20);
    {
        QPushButton* rm = removeButtonFor(&dlg, R2);
        CHECK(rm != nullptr, "R2's row has a visible remove button");
        if (rm)
        {
            // The cause, measured and printed rather than assumed: does the font the button draws with hold
            // U+2715 at all (with fallback), and does a button with room for it draw it?
            QPushButton roomy(rm->text(), &dlg);
            roomy.show();
            roomy.adjustSize();
            const bool inFont = QFontMetrics(rm->font()).inFontUcs4(0x2715);
            std::printf("  info the button's font %s U+2715; the caption at its natural size puts %d px of ink down\n",
                        inFont ? "holds" : "does NOT hold", captionInk(&roomy));
            roomy.hide();

            QString why;
            const bool fits = captionFits(rm, &why);
            CHECK(fits, "the remove button's caption fits inside the contents rect the style leaves it, and draws");
            std::printf("         %s\n", qPrintable(why));
            rm->setFocus();                // the focused look has its own rule (a 2px border) — check it too
            pump(10);
            const bool fitsFocused = captionFits(rm, &why);
            CHECK(fitsFocused, "…and still does while focused");
            std::printf("         %s\n", qPrintable(why));
        }
    }

    // The helper every fixed-width glyph button in the classic UI goes through (the profile list's ✎ and ✕,
    // a split-screen pane's pause and ✕): at the narrowest width any of them uses, under the same sheet, the
    // caption fits and draws. Without the helper the same button draws nothing — the control shows the
    // measurement is capable of failing.
    {
        QWidget host;
        host.show();
        QPushButton plain(QStringLiteral("✕"), &host);
        plain.setFixedWidth(28);
        plain.show();
        QPushButton glyph(QStringLiteral("✕"), &host);
        GlyphButton::apply(&glyph, 28);
        glyph.show();
        pump(10);
        QString why;
        const bool plainFits = captionFits(&plain, &why);
        CHECK(!plainFits, "control: a 28px button WITHOUT the helper has no room for its caption under the app sheet");
        std::printf("         %s\n", qPrintable(why));
        CHECK(captionFits(&glyph, &why), "a 28px button through GlyphButton::apply fits and draws its caption");
        std::printf("         %s\n", qPrintable(why));
        CHECK(glyph.width() == 28, "…at the width it was asked for");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { std::printf("REGBROWSER-FAIL\n"); return 1; }
    std::printf("REGBROWSER-OK\n");
    return 0;
}
