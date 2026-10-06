#pragma once

#include "core/chart_project.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;

namespace infalsus::gui {

class MetadataWidget final : public QWidget {
    Q_OBJECT

public:
    explicit MetadataWidget(QWidget* parent = nullptr);

    void setMetadata(const ChartMetadata& metadata);
    void setDifficultyMetadata(const DifficultyMetadata& metadata);
    void setDifficulty(Difficulty difficulty);
    void setChartId(const QString& chartId);
    void setJacketAvailable(bool available);
    void setJacketEditingEnabled(bool enabled);
    void setPreviewStartMilliseconds(qint64 milliseconds);
    void setPreviewEndMilliseconds(qint64 milliseconds);

signals:
    void metadataEdited(ChartMetadata metadata);
    void difficultyMetadataEdited(DifficultyMetadata metadata);
    void difficultyRequested(Difficulty difficulty);
    void chartIdEdited(const QString& chartId);
    void jacketRequested();
    void previewStartCurrentTimeRequested();
    void previewEndCurrentTimeRequested();

private:
    void emitMetadata();
    void emitDifficultyMetadata();

    QLineEdit* m_songName = nullptr;
    QLineEdit* m_artistName = nullptr;
    QSpinBox* m_previewStartMilliseconds = nullptr;
    QSpinBox* m_previewEndMilliseconds = nullptr;
    QComboBox* m_gameplayBackground = nullptr;
    QComboBox* m_difficulty = nullptr;
    QLineEdit* m_chartId = nullptr;
    QSpinBox* m_rating = nullptr;
    QLineEdit* m_chartDesigner = nullptr;
    QLineEdit* m_jacketDesigner = nullptr;
    QPushButton* m_jacketButton = nullptr;
    QPushButton* m_previewStartCurrentTimeButton = nullptr;
    QPushButton* m_previewEndCurrentTimeButton = nullptr;
    QString m_hiddenCharacterIdentifier;
    bool m_loading = false;
};

} // namespace infalsus::gui
