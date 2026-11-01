#pragma once

#include "gui/app/game_palette.h"

#include <QtGui/QPainter>
#include <QtWidgets/QLabel>

#include <algorithm>

namespace infalsus::gui {

class MonospaceTimeLabel final : public QLabel {
public:
    using QLabel::QLabel;

    QSize sizeHint() const override {
        QSize result = QLabel::sizeHint();
        const QMargins margins = contentsMargins();
        result.setWidth(std::max(10, static_cast<int>(text().size())) * cellWidth()
            + margins.left() + margins.right());
        return result;
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette::chrome);
        painter.setFont(font());
        painter.setPen(isEnabled() ? palette::text : palette::textDisabled);
        const QRect area = contentsRect();
        const QString value = text();
        const int advance = cellWidth();
        int x = area.left() + (area.width() - static_cast<int>(value.size()) * advance) / 2;
        for (const QChar character : value) {
            painter.drawText(QRect(x, area.top(), advance, area.height()), Qt::AlignCenter, QString(character));
            x += advance;
        }
    }

private:
    int cellWidth() const {
        const QFontMetrics metrics(font());
        int width = 0;
        for (const QChar character : QStringLiteral("0123456789:")) {
            width = std::max({width, metrics.horizontalAdvance(character), metrics.boundingRect(character).width()});
        }
        return width;
    }
};

} // namespace infalsus::gui
