#include "gui/app/dark_theme.h"

#include "gui/app/game_palette.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

namespace infalsus::gui {

QPalette darkThemePalette() {
    QPalette result;
    result.setColor(QPalette::Window, palette::chrome);
    result.setColor(QPalette::WindowText, palette::text);
    result.setColor(QPalette::Base, palette::canvasBackground);
    result.setColor(QPalette::AlternateBase, palette::panel);
    result.setColor(QPalette::ToolTipBase, palette::panelHover);
    result.setColor(QPalette::ToolTipText, palette::text);
    result.setColor(QPalette::Text, palette::text);
    result.setColor(QPalette::Button, palette::panel);
    result.setColor(QPalette::ButtonText, palette::buttonText);
    result.setColor(QPalette::Highlight, palette::floorNoteBlue);
    result.setColor(QPalette::HighlightedText, palette::chrome);
    result.setColor(QPalette::Link, palette::guideGreen);
    result.setColor(QPalette::BrightText, palette::white);
    result.setColor(QPalette::Disabled, QPalette::Text, palette::textMuted);
    result.setColor(QPalette::Disabled, QPalette::ButtonText, palette::textMuted);

    return result;
}

void applyDarkTheme(QApplication& application) {
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    application.setPalette(darkThemePalette());
}

} // namespace infalsus::gui
