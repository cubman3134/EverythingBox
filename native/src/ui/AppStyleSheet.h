// The app-wide base style sheet, applied once in main() with QApplication::setStyleSheet.
//
// It lives in a header rather than inline in main() so that a probe can lay a real widget out under exactly
// the rules the app runs with (issue #458). The sheet's QPushButton padding is 16px a side, so a button
// narrower than about 34px has NO room left for its caption at all, and the classic registry browser's
// remove button (28px wide) drew an empty face because of it. A probe that styled its own widgets with a
// copy of these rules would stay green the day the app's copy changed; one that includes this header cannot.
//
// Comfortable, remote/touch-friendly base sizing for generic controls (dialogs, lists, inputs). Views
// that set their own styles (Home chrome, settings panels) keep theirs; this just enlarges the rest.
// The :focus rules are the app-wide SELECTION HIGHLIGHT: stylesheet-styled controls suppress the
// native focus rectangle, so any widget without its own :focus rule (e.g. the profile picker's
// buttons) looked completely unselected while focused — "the selection disappeared" when arrowing
// onto it. Screens with their own :focus styles (panel rows, overlays, the esc menu) win over these.
#pragma once
#include <QString>

namespace AppStyleSheet
{
inline QString base()
{
    return QStringLiteral(
        "QPushButton{min-height:30px;padding:8px 16px;font-size:14px;}"
        "QPushButton:focus{background:#2D6CDF;color:#fff;border:2px solid #5B8CFF;border-radius:6px;}"
        "QLineEdit,QComboBox,QAbstractSpinBox{min-height:30px;padding:5px 10px;font-size:14px;}"
        // Focused = SELECTED: an outline around the box (you navigated to it, you're not typing yet).
        "QLineEdit:focus,QComboBox:focus,QAbstractSpinBox:focus{border:2px solid #5B8CFF;border-radius:4px;}"
        // EDITING (a live cursor, set by NavTextField): a brighter, filled look so it's clearly distinct
        // from the plain selection outline.
        "QLineEdit[mmvEditing=\"true\"]{background:#0d0f14;border:2px solid #8FB2FF;border-radius:4px;}"
        // A scrollable text view (the Debug log) gets the same two-state outline: SELECTED shows a border,
        // INTERACTING (scroll mode) shows the brighter one.
        "QPlainTextEdit:focus,QTextEdit:focus{border:2px solid #5B8CFF;}"
        "QPlainTextEdit[mmvEditing=\"true\"],QTextEdit[mmvEditing=\"true\"]{border:2px solid #8FB2FF;}"
        "QCheckBox,QRadioButton{font-size:14px;spacing:8px;}"
        "QCheckBox:focus,QRadioButton:focus{color:#2D6CDF;font-weight:bold;}"
        "QCheckBox::indicator,QRadioButton::indicator{width:20px;height:20px;}"
        "QSlider:focus{background:rgba(91,140,255,0.20);border-radius:4px;}"
        "QListWidget::item,QListView::item{min-height:34px;}"
        "QScrollBar:vertical{width:14px;}QScrollBar:horizontal{height:14px;}");
}
} // namespace AppStyleSheet
