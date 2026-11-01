#include "gui/app/dark_theme.h"

#include "gui/app/game_palette.h"
#include "gui/widgets/splitter_grip.h"

#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QProxyStyle>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QStyleOption>

#include <limits>

namespace infalsus::gui {

namespace {

class EditorStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void polish(QWidget* widget) override {
        QProxyStyle::polish(widget);
        if (auto* combo = qobject_cast<QComboBox*>(widget)) {
            combo->setMaxVisibleItems(std::numeric_limits<int>::max());
        }
    }

    int pixelMetric(const PixelMetric metric, const QStyleOption* option, const QWidget* widget) const override {
        return metric == PM_SplitterWidth ? 6 : QProxyStyle::pixelMetric(metric, option, widget);
    }

    QRect subControlRect(const ComplexControl control, const QStyleOptionComplex* option,
        const SubControl subControl, const QWidget* widget) const override {
        const auto* spinBox = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (control != CC_SpinBox || spinBox == nullptr) {
            return QProxyStyle::subControlRect(control, option, subControl, widget);
        }
        QStyleOptionSpinBox native = *spinBox;
        native.frame = true;
        QRect area = QProxyStyle::subControlRect(control, &native, subControl, widget);
        if (area.isEmpty()) {
            return area;
        }
        const bool rightToLeft = spinBox->direction == Qt::RightToLeft;
        if (subControl == SC_SpinBoxUp || subControl == SC_SpinBoxDown) {
            area.translate(rightToLeft ? -1 : 1, 0);
        } else if (subControl == SC_SpinBoxEditField) {
            area.adjust(rightToLeft ? -1 : 0, 0, rightToLeft ? 0 : 1, 0);
        }
        return area;
    }

    void drawComplexControl(const ComplexControl control, const QStyleOptionComplex* option,
        QPainter* painter, const QWidget* widget) const override {
        const auto* spinBox = qstyleoption_cast<const QStyleOptionSpinBox*>(option);
        if (control != CC_SpinBox || spinBox == nullptr) {
            QProxyStyle::drawComplexControl(control, option, painter, widget);
            return;
        }
        painter->save();
        painter->fillRect(spinBox->rect, spinBox->palette.base());
        QStyleOptionSpinBox native = *spinBox;
        native.frame = false;
        QProxyStyle::drawComplexControl(control, &native, painter, widget);
        if (spinBox->frame) {
            painter->setPen(spinBox->state & State_HasFocus ? palette::floorNoteBlue : palette::border);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(spinBox->rect.adjusted(0, 0, -1, -1));
        }
        painter->restore();
    }

    void drawControl(const ControlElement element, const QStyleOption* option,
        QPainter* painter, const QWidget* widget) const override {
        if (element != CE_Splitter) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        painter->save();
        const QRect area = option->rect;
        const QPoint center = area.center();
        const bool horizontal = (option->state & State_Horizontal) != 0;
        const int gripY = horizontal ? splitterGripY(widget, area) : center.y();
        painter->fillRect(area, option->state & State_MouseOver ? palette::panelHover : palette::panel);
        painter->setPen(palette::gridLine);
        if (horizontal) {
            painter->drawLine(center.x(), area.top(), center.x(), area.bottom());
        } else {
            painter->drawLine(area.left(), center.y(), area.right(), center.y());
        }
        painter->setPen(option->state & State_Sunken ? palette::floorNoteBlue : palette::text);
        for (const int offset : {-4, 0, 4}) {
            if (horizontal) {
                painter->drawLine(area.left() + 1, gripY + offset, area.right() - 1, gripY + offset);
            } else {
                painter->drawLine(center.x() + offset, area.top() + 1, center.x() + offset, area.bottom() - 1);
            }
        }
        painter->restore();
    }
};

[[nodiscard]] QString darkThemeStyleSheet() {
    return QStringLiteral(R"(
        QFrame[workspacePanel="true"] {
            background: @surface;
            border: 1px solid @border;
            border-radius: 0;
        }
        QWidget#workspacePanelHeader {
            background: @header;
            border-bottom: 1px solid @headerBorder;
            border-left: 3px solid transparent;
            min-height: 30px;
        }
        QWidget#workspacePanelHeader[panelFocused="true"] { border-left-color: @accent; }
        QLabel#workspacePanelTitle {
            color: @text;
            font-weight: 600;
            padding: 0 8px;
            border: none;
            background: transparent;
        }
        QLabel { color: @text; }
        QLabel#workspaceStatus { padding: 1px 6px; }
        QWidget#transportBar, QLabel#transportTime { background: @surface; }
        QLabel#transportTime { color: @text; }
        QPushButton, QToolButton {
            color: @text;
            background: @control;
            border: 1px solid @border;
            border-radius: 0;
            min-height: 20px;
            padding: 3px 8px;
        }
        QToolButton { padding: 3px 6px; }
        QToolButton#playbackButton {
            padding: 0;
            min-width: 44px; max-width: 44px;
            min-height: 44px; max-height: 44px;
        }
        QPushButton:hover, QToolButton:hover { background: @hover; }
        QPushButton:pressed, QToolButton:pressed,
        QPushButton:checked, QToolButton:checked {
            background: @selection;
            border-color: @accent;
        }
        QPushButton:checked:hover, QToolButton:checked:hover { background: @hover; }
        QPushButton:default, QPushButton:focus, QToolButton:focus { border-color: @accent; }
        QPushButton:disabled, QToolButton:disabled {
            color: @disabled;
            background: @surface;
            border-color: @control;
        }
        QLineEdit, QComboBox, QTextEdit, QPlainTextEdit {
            color: @text;
            background: @input;
            border: 1px solid @border;
            border-radius: 0;
            padding: 3px 7px;
            selection-background-color: @selection;
            selection-color: @text;
        }
        QLineEdit, QComboBox { min-height: 20px; }
        QAbstractSpinBox { min-height: 26px; }
        QLineEdit:hover, QComboBox:hover { border-color: @text; }
        QLineEdit:focus, QComboBox:focus,
        QTextEdit:focus, QPlainTextEdit:focus { border-color: @accent; }
        QLineEdit:disabled, QComboBox:disabled {
            color: @disabled;
            background: @surface;
            border-color: @control;
        }
        QAbstractSpinBox QLineEdit {
            border: none;
            border-radius: 0;
            background: transparent;
            padding: 0;
            min-height: 0;
        }
        QAbstractItemView {
            color: @text;
            background: @input;
            alternate-background-color: @surface;
            border: none;
            selection-background-color: @selection;
            selection-color: @text;
            outline: 0;
        }
        QAbstractItemView::item { min-height: 20px; padding: 4px 8px; }
        QAbstractItemView::item:hover { background: @hover; }
        QAbstractItemView::item:selected { background: @selection; color: @text; }
        QAbstractItemView::item:focus { border: 1px solid @accent; }
        QListView#eventsList:focus, QListWidget#verificationList:focus,
        QListView#eventsList::item:focus, QListWidget#verificationList::item:focus {
            border: none;
            outline: 0;
        }
        QHeaderView::section {
            color: @text;
            background: @surface;
            border: none;
            border-bottom: 1px solid @border;
            padding: 5px 8px;
            font-weight: 600;
        }
        QWidget#eventsColumnHeader { background: @surface; min-height: 26px; }
        QWidget#eventsColumnHeader QLabel { color: @text; font-weight: 600; }
        QScrollArea { border: none; background: @surface; }
        QScrollBar:vertical { background: @surface; width: 10px; margin: 0; }
        QScrollBar:horizontal { background: @surface; height: 10px; margin: 0; }
        QScrollBar::handle { background: @border; border-radius: 0; }
        QScrollBar::handle:vertical { min-height: 28px; margin: 1px 2px; }
        QScrollBar::handle:horizontal { min-width: 28px; margin: 2px 1px; }
        QScrollBar::handle:hover { background: @text; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QFrame[frameShape="4"] {
            border: none; background: @border; min-height: 1px; max-height: 1px;
        }
        QFrame[frameShape="5"] {
            border: none; background: @border; min-width: 1px; max-width: 1px;
        }
        QToolTip {
            color: @text;
            background: @hover;
            border: 1px solid @border;
            padding: 5px 8px;
        }
        QGroupBox {
            border: 1px solid @border;
            border-radius: 0;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title { color: @text; subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QTabWidget::pane { border: 1px solid @border; }
        QTabBar::tab {
            background: @surface;
            color: @text;
            border-bottom: 2px solid transparent;
            padding: 7px 12px;
        }
        QTabBar::tab:selected { color: @text; border-bottom-color: @accent; }
        QTabBar::tab:hover { background: @hover; }
        QToolBar#editorToolsToolbar {
            background: @surface;
            border: none;
            border-right: 1px solid @border;
            spacing: 3px;
            padding: 3px;
        }
        QToolBar#editorToolsToolbar QToolButton {
            background: @control;
            border: 1px solid @border;
            border-left: 3px solid @border;
            padding: 4px 3px;
        }
        QToolBar#editorToolsToolbar QToolButton:hover { background: @hover; }
        QToolBar#editorToolsToolbar QToolButton:checked {
            color: @text;
            background: @selection;
            border-left-color: @accent;
        }
        QToolBar#editorToolsToolbar QToolButton:focus { border-color: @accent; }
        QToolButton#toolsReveal { background: @surface; border: none; padding: 0; }
        QToolButton#toolsReveal:hover { background: @selection; color: @accent; }
        QSlider#volumeSlider, QSlider#effectsVolumeSlider {
            background: rgb(3, 11, 18);
            border: 1px solid rgb(0, 0, 0);
            padding: 2px 1px;
        }
        QSlider#volumeSlider::groove:vertical, QSlider#effectsVolumeSlider::groove:vertical {
            background: rgb(6, 17, 27);
            border: 1px solid rgb(0, 8, 13);
            width: 30px;
            margin: 0;
        }
        QSlider#volumeSlider::add-page:vertical, QSlider#effectsVolumeSlider::add-page:vertical {
            background: @accent;
        }
        QSlider#volumeSlider::sub-page:vertical, QSlider#effectsVolumeSlider::sub-page:vertical {
            background: rgb(6, 17, 27);
        }
        QSlider#volumeSlider::handle:vertical, QSlider#effectsVolumeSlider::handle:vertical {
            background: rgb(191, 244, 244);
            border: 1px solid rgb(219, 255, 255);
            height: 5px;
            margin: 0 -2px;
        }
    )")
        .replace(QStringLiteral("@headerBorder"), palette::panelHeaderBorder.name())
        .replace(QStringLiteral("@header"), palette::panelHeader.name())
        .replace(QStringLiteral("@surface"), palette::panel.name())
        .replace(QStringLiteral("@control"), palette::control.name())
        .replace(QStringLiteral("@input"), palette::inputBackground.name())
        .replace(QStringLiteral("@hover"), palette::panelHover.name())
        .replace(QStringLiteral("@border"), palette::border.name())
        .replace(QStringLiteral("@text"), palette::text.name())
        .replace(QStringLiteral("@disabled"), palette::textDisabled.name())
        .replace(QStringLiteral("@selection"), palette::selection.name())
        .replace(QStringLiteral("@accent"), palette::floorNoteBlue.name());
}

} // namespace

QPalette darkThemePalette() {
    QPalette result;
    result.setColor(QPalette::Window, palette::chrome);
    result.setColor(QPalette::WindowText, palette::text);
    result.setColor(QPalette::Base, palette::inputBackground);
    result.setColor(QPalette::AlternateBase, palette::panel);
    result.setColor(QPalette::ToolTipBase, palette::panelHover);
    result.setColor(QPalette::ToolTipText, palette::text);
    result.setColor(QPalette::Text, palette::text);
    result.setColor(QPalette::Button, palette::control);
    result.setColor(QPalette::ButtonText, palette::buttonText);
    result.setColor(QPalette::Highlight, palette::floorNoteBlue);
    result.setColor(QPalette::HighlightedText, palette::chrome);
    result.setColor(QPalette::Mid, palette::border);
    result.setColor(QPalette::Dark, palette::inputBackground);
    result.setColor(QPalette::Light, palette::panelHover);
    result.setColor(QPalette::PlaceholderText, palette::text);
    result.setColor(QPalette::Link, palette::guideGreen);
    result.setColor(QPalette::BrightText, palette::white);
    result.setColor(QPalette::Disabled, QPalette::WindowText, palette::textDisabled);
    result.setColor(QPalette::Disabled, QPalette::Text, palette::textDisabled);
    result.setColor(QPalette::Disabled, QPalette::ButtonText, palette::textDisabled);

    return result;
}

void applyDarkTheme(QApplication& application) {
    application.setStyle(new EditorStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    application.setFont(QFont(QStringLiteral("Segoe UI"), 10));
    application.setPalette(darkThemePalette());
    application.setStyleSheet(darkThemeStyleSheet());
}

} // namespace infalsus::gui
