#pragma once

#include <QtWidgets/QWidget>

class QSlider;
class QToolButton;

namespace infalsus::gui {

class PlaybackControls final : public QWidget {
    Q_OBJECT

public:
    explicit PlaybackControls(QWidget* parent = nullptr);

    void setPlaying(bool playing);
    void setPlaybackRate(qreal rate);

signals:
    void togglePlaybackRequested();
    void playbackRateRequested(qreal rate);

private:
    QToolButton* m_playPauseButton = nullptr;
    QSlider* m_speedSelector = nullptr;
};

} // namespace infalsus::gui
