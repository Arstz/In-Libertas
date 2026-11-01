#pragma once

#include <QtCore/QSignalBlocker>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <functional>
#include <utility>

namespace infalsus::gui {

class SkyGridControls final : public QWidget {
public:
    using Changed = std::function<void(bool, int)>;

    explicit SkyGridControls(Changed changed, QWidget* parent = nullptr)
        : QWidget(parent), m_changed(std::move(changed)) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_toggle = new QToolButton(this);
        m_toggle->setCheckable(true);
        m_toggle->setFocusPolicy(Qt::NoFocus);
        m_toggle->setToolTip(QStringLiteral("Toggle the sky snap grid"));
        m_divisors = new QComboBox(this);
        m_divisors->setAccessibleName(QStringLiteral("Sky grid divisor"));
        m_divisors->setToolTip(QStringLiteral("Sky grid subdivisions per lane"));
        m_combined = new QComboBox(this);
        m_combined->setAccessibleName(QStringLiteral("Sky grid"));
        m_combined->setToolTip(QStringLiteral("Sky grid: OFF or subdivisions per lane"));
        m_combined->addItem(QStringLiteral("OFF"));
        for (int divisor = 1; divisor <= 4; ++divisor) {
            const QString label = QStringLiteral("1/%1").arg(divisor);
            m_divisors->addItem(label);
            m_combined->addItem(label);
        }
        layout->addWidget(m_toggle);
        layout->addWidget(m_divisors);
        layout->addWidget(m_combined);
        m_combined->hide();
        sync();
        connect(m_toggle, &QToolButton::toggled, this, [this](const bool enabled) {
            m_enabled = enabled;
            publish();
        });
        connect(m_divisors, &QComboBox::activated, this, [this](const int index) { setDivisor(index + 1); });
        connect(m_combined, &QComboBox::activated, this, [this](const int index) {
            m_enabled = index != 0;
            if (m_enabled) {
                m_divisor = index;
            }
            publish();
        });
    }

    void toggleGrid() {
        m_enabled = !m_enabled;
        publish();
    }

    void setDivisor(const int divisor) {
        m_divisor = std::clamp(divisor, 1, 4);
        publish();
    }

    QSize sizeHint() const override {
        return {expandedWidth(), minimumSizeHint().height()};
    }

    QSize minimumSizeHint() const override {
        return m_combined->sizeHint();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        updateMode();
    }

    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
            updateGeometry();
            updateMode();
        }
    }

private:
    int expandedWidth() const {
        return m_toggle->sizeHint().width() + m_divisors->sizeHint().width() + 3;
    }

    void updateMode() {
        const bool compact = width() < expandedWidth();
        m_toggle->setVisible(!compact);
        m_divisors->setVisible(!compact);
        m_combined->setVisible(compact);
    }

    void sync() {
        const QSignalBlocker toggleBlocker(m_toggle);
        const QSignalBlocker divisorsBlocker(m_divisors);
        const QSignalBlocker combinedBlocker(m_combined);
        m_toggle->setText(m_enabled ? QStringLiteral("GRID ON") : QStringLiteral("GRID OFF"));
        m_toggle->setChecked(m_enabled);
        m_divisors->setCurrentIndex(m_divisor - 1);
        m_combined->setCurrentIndex(m_enabled ? m_divisor : 0);
    }

    void publish() {
        sync();
        updateGeometry();
        updateMode();
        m_changed(m_enabled, m_divisor);
    }

    Changed m_changed;
    QToolButton* m_toggle = nullptr;
    QComboBox* m_divisors = nullptr;
    QComboBox* m_combined = nullptr;
    bool m_enabled = false;
    int m_divisor = 1;
};

} // namespace infalsus::gui
