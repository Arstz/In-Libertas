#include "gui/app/sv_interpolation_dialog.h"

#include "core/timing_grid.h"

#include <QtCore/QRegularExpression>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

namespace infalsus::gui {

SvInterpolationDialog::SvInterpolationDialog(const int divisor, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Interpolate SV events"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    m_spacing = new QComboBox(this);
    m_spacing->addItems({QStringLiteral("Time"), QStringLiteral("Divisor")});

    auto* intervals = new QStackedWidget(this);
    auto* timeRow = new QWidget(intervals);
    auto* timeLayout = new QHBoxLayout(timeRow);
    timeLayout->setContentsMargins(0, 0, 0, 0);
    m_milliseconds = new QLineEdit(QStringLiteral("100"), timeRow);
    m_milliseconds->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,19}")), m_milliseconds));
    timeLayout->addWidget(m_milliseconds);
    timeLayout->addWidget(new QLabel(QStringLiteral("ms"), timeRow));
    intervals->addWidget(timeRow);
    m_divisor = new QComboBox(intervals);
    for (const int value : kSupportedDivisors) {
        m_divisor->addItem(QStringLiteral("1/%1").arg(value), value);
    }
    m_divisor->setCurrentIndex(m_divisor->findData(divisor));
    intervals->addWidget(m_divisor);

    m_easing = new QComboBox(this);
    m_easing->addItem(QStringLiteral("Sine"), static_cast<int>(SpeedInterpolationOptions::Easing::Sine));
    m_easing->addItem(QStringLiteral("Cosine"), static_cast<int>(SpeedInterpolationOptions::Easing::Cosine));
    m_easing->addItem(QStringLiteral("Linear"), static_cast<int>(SpeedInterpolationOptions::Easing::Linear));
    m_easing->setCurrentIndex(2);
    form->addRow(QStringLiteral("Interpolate by"), m_spacing);
    form->addRow(QStringLiteral("Every"), intervals);
    form->addRow(QStringLiteral("Easing"), m_easing);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Interpolate"));
    layout->addWidget(buttons);
    const auto updateValidity = [this, buttons] {
        bool valid = false;
        const qint64 milliseconds = m_milliseconds->text().toLongLong(&valid);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(m_spacing->currentIndex() == 1
            || (valid && milliseconds >= 1));
    };
    connect(m_spacing, &QComboBox::currentIndexChanged, intervals, &QStackedWidget::setCurrentIndex);
    connect(m_spacing, &QComboBox::currentIndexChanged, this, updateValidity);
    connect(m_milliseconds, &QLineEdit::textChanged, this, updateValidity);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateValidity();
    m_milliseconds->selectAll();
    m_milliseconds->setFocus();
}

SpeedInterpolationOptions SvInterpolationDialog::options() const {
    return {
        .spacing = m_spacing->currentIndex() == 0
            ? SpeedInterpolationOptions::Spacing::Time : SpeedInterpolationOptions::Spacing::Divisor,
        .everyMilliseconds = m_milliseconds->text().toLongLong(),
        .divisor = m_divisor->currentData().toInt(),
        .easing = static_cast<SpeedInterpolationOptions::Easing>(m_easing->currentData().toInt()),
    };
}

} // namespace infalsus::gui
