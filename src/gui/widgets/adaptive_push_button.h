#pragma once

#include <QtCore/QEvent>
#include <QtCore/QStringList>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionButton>

#include <utility>

namespace infalsus::gui {

class AdaptivePushButton final : public QPushButton {
public:
    explicit AdaptivePushButton(QStringList labels, QWidget* parent = nullptr)
        : QPushButton(labels.front(), parent), m_labels(std::move(labels)) {
        setToolTip(m_labels.front());
        setAccessibleName(m_labels.front());
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override {
        return labelSize(m_labels.front());
    }

    QSize minimumSizeHint() const override {
        return labelSize(m_labels.back());
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QPushButton::event(event);
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            QString label = m_labels.back();
            for (const QString& candidate : m_labels) {
                if (labelSize(candidate).width() <= width()) {
                    label = candidate;
                    break;
                }
            }
            if (text() != label) {
                setText(label);
            }
        }
        return handled;
    }

private:
    QSize labelSize(const QString& label) const {
        QStyleOptionButton option;
        initStyleOption(&option);
        const int padding = 2 * (style()->pixelMetric(QStyle::PM_ButtonMargin, &option, this)
            + style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, this));
        return {fontMetrics().size(Qt::TextShowMnemonic, label).width() + padding,
            QPushButton::sizeHint().height()};
    }

    QStringList m_labels;
};

}
