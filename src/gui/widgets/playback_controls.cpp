#include "gui/widgets/playback_controls.h"

#include "gui/app/game_palette.h"
#include "gui/state/playback_rates.h"
#include "gui/widgets/timeline_widget.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <cmath>

namespace infalsus::gui {

namespace {

constexpr int kControlVerticalMargin = 5;
constexpr int kButtonSize = TimelineWidget::kWidgetHeight - kControlVerticalMargin * 2;
constexpr int kIconSize = 20;
constexpr double kPauseBarWidth = kIconSize * 0.3;
constexpr int kControlMargin = 8;
constexpr int kControlSpacing = 8;
constexpr int kSelectorWidth = 200;
constexpr int kControlWidth = kControlMargin * 2 + kButtonSize + kControlSpacing + kSelectorWidth;
constexpr double kSelectorMargin = 18.0;
constexpr double kLabelTop = kControlVerticalMargin;
constexpr double kLabelWidth = 36.0;
constexpr double kLabelHeight = 18.0;
constexpr double kTrackY = 29.0;
constexpr double kTickHalfHeight = 3.0;
constexpr double kIndicatorGap = 5.0;
constexpr double kIndicatorSize = 7.0;
constexpr double kIndicatorBottom = kControlVerticalMargin + kButtonSize;

class PlaybackButton final : public QToolButton {
public:
    explicit PlaybackButton(QWidget* parent)
        : QToolButton(parent) {
        setCheckable(true);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        const QRectF iconArea((width() - kIconSize) * 0.5, (height() - kIconSize) * 0.5,
            kIconSize, kIconSize);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette::floorNoteBlue);
        if (isChecked()) {
            painter.drawRect(QRectF(iconArea.left(), iconArea.top(), kPauseBarWidth, iconArea.height()));
            painter.drawRect(QRectF(iconArea.right() - kPauseBarWidth, iconArea.top(),
                kPauseBarWidth, iconArea.height()));
        } else {
            painter.drawPolygon(QPolygonF{iconArea.topLeft(), QPointF(iconArea.right(), iconArea.center().y()),
                iconArea.bottomLeft()});
        }
    }
};

[[nodiscard]] qreal rateForIndex(const int index) {
    return kPlaybackRates.at(kPlaybackRates.size() - 1 - static_cast<std::size_t>(index));
}

class PlaybackRateSlider final : public QSlider {
public:
    explicit PlaybackRateSlider(QWidget* parent)
        : QSlider(Qt::Horizontal, parent) {
        setRange(0, static_cast<int>(kPlaybackRates.size()) - 1);
        setSingleStep(1);
        setPageStep(1);
        setValue(maximum());
        setFixedWidth(kSelectorWidth);
        setFixedHeight(TimelineWidget::kWidgetHeight);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(QStringLiteral("Playback speed"));
        setToolTip(QStringLiteral("Playback speed"));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        m_dragging = true;
        setValue(indexForPosition(event->position().x()));
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
            event->ignore();
            return;
        }
        setValue(indexForPosition(event->position().x()));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_dragging) {
            event->ignore();
            return;
        }
        m_dragging = false;
        setValue(indexForPosition(event->position().x()));
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        const double trackWidth = std::max(1.0, width() - kSelectorMargin * 2.0);
        const double indicatorX = kSelectorMargin + trackWidth * value() / maximum();
        const double indicatorTop = kTrackY + kIndicatorGap;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette::textMuted, 1.0));
        painter.drawLine(QPointF(kSelectorMargin, kTrackY), QPointF(width() - kSelectorMargin, kTrackY));
        for (int index = minimum(); index <= maximum(); ++index) {
            const double x = kSelectorMargin + trackWidth * index / maximum();
            painter.setPen(palette::text);
            painter.drawText(QRectF(x - kLabelWidth * 0.5, kLabelTop, kLabelWidth, kLabelHeight),
                Qt::AlignCenter, QString::number(rateForIndex(index), 'g', 2));
            painter.setPen(QPen(palette::textMuted, 1.0));
            painter.drawLine(QPointF(x, kTrackY - kTickHalfHeight), QPointF(x, kTrackY + kTickHalfHeight));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette::floorNoteBlue);
        painter.drawPolygon(QPolygonF{
            QPointF(indicatorX, indicatorTop),
            QPointF(indicatorX - kIndicatorSize, kIndicatorBottom),
            QPointF(indicatorX + kIndicatorSize, kIndicatorBottom),
        });
    }

private:
    [[nodiscard]] int indexForPosition(const double x) const {
        const double trackWidth = std::max(1.0, width() - kSelectorMargin * 2.0);
        const double progress = std::clamp((x - kSelectorMargin) / trackWidth, 0.0, 1.0);

        return static_cast<int>(std::lround(progress * maximum()));
    }

    bool m_dragging = false;
};

} // namespace

PlaybackControls::PlaybackControls(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    m_playPauseButton = new PlaybackButton(this);
    m_speedSelector = new PlaybackRateSlider(this);
    setFixedSize(kControlWidth, TimelineWidget::kWidgetHeight);
    layout->setContentsMargins(kControlMargin, 0, kControlMargin, 0);
    layout->setSpacing(kControlSpacing);
    m_playPauseButton->setFixedSize(kButtonSize, kButtonSize);
    m_playPauseButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_playPauseButton->setFocusPolicy(Qt::NoFocus);
    m_playPauseButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_playPauseButton, 0, Qt::AlignVCenter);
    layout->addWidget(m_speedSelector);
    setPlaying(false);
    connect(m_playPauseButton, &QToolButton::clicked, this, &PlaybackControls::togglePlaybackRequested);
    connect(m_speedSelector, &QSlider::valueChanged, this, [this](const int index) {
        emit playbackRateRequested(rateForIndex(index));
    });
}

void PlaybackControls::setPlaying(const bool playing) {
    const QString label = playing ? QStringLiteral("Pause") : QStringLiteral("Play");
    m_playPauseButton->setChecked(playing);
    m_playPauseButton->setToolTip(label);
    m_playPauseButton->setAccessibleName(label);
}

void PlaybackControls::setPlaybackRate(const qreal rate) {
    const auto found = std::find(kPlaybackRates.crbegin(), kPlaybackRates.crend(), rate);
    if (found == kPlaybackRates.crend()) {
        return;
    }
    const QSignalBlocker blocker(m_speedSelector);
    m_speedSelector->setValue(static_cast<int>(std::distance(kPlaybackRates.crbegin(), found)));
}

} // namespace infalsus::gui
