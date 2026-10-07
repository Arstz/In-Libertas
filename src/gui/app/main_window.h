#pragma once

#include "core/chart_project.h"
#include "core/timing_analyzer.h"
#include "gui/app/visual_settings.h"
#include "gui/app/handling_settings.h"

#include <QtCore/QByteArray>
#include <QtGui/QImage>
#include <QtWidgets/QMainWindow>

#include <memory>

class ConveyorView;
class PlaybackController;
class QAudioBuffer;
class QAudioDecoder;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QShowEvent;
class QCloseEvent;
class QSplitter;
class QSlider;
class QToolBar;
class QWidget;

namespace infalsus::gui {

class EditorState;
class FlatView;
class KeyBindingRouter;
class MetadataWidget;
class PropertiesPanel;
class TimelineWidget;
class EventsWidget;
class VerificationWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildInterface();
    void buildToolBar();
    void registerNavigationKeybinds();
    void openSettings();
    void updateVolumeSliderPlacement();
    void buildWorkspace();
    void arrangeSplitters();
    void restoreLayout();
    void saveLayout() const;
    void resetLayout();
    void selectAllVisibleHitObjects();
    [[nodiscard]] bool confirmDiscardUnsavedChanges(const QString& action);
    void markProjectDirty();
    void importSpc();
    void loadChartFile(const QString& chartPath);
    void createProject();
    void openProject();
    void openRecentProject(const QString& path);
    void loadProject(const QString& path);
    void rememberRecentProject(const QString& path);
    void refreshRecentProjectMenu();
    void saveProject();
    void saveProjectAs();
    void exportProject();
    void storeLoadedDifficulty();
    void loadDifficulty(infalsus::Difficulty difficulty);
    void loadProjectAssets(const QString& songFileName, const QByteArray& songData,
        const QString& jacketFileName, const QByteArray& jacketData);
    void updateProjectDuration(qint64 durationMilliseconds);
    void updateStatus();
    [[nodiscard]] bool readProjectJacket(QImage* image, QString* error) const;
    void updateViewerSongCard();
    void togglePlayback();
    void adjustFlatZoom(double steps);
    void startInitialTimingAnalysis(const QString& audioPath);
    void appendInitialTimingAudio(const QAudioBuffer& buffer);
    void finishInitialTimingAnalysis();
    [[nodiscard]] bool initialTimingAnalysisShouldFinish() const;
    [[nodiscard]] QString audioPathForFolder(const QString& folderPath) const;

    EditorState* m_state = nullptr;
    VisualSettings m_visualSettings;
    HandlingSettings m_handlingSettings;
    KeyBindingRouter* m_keyBindings = nullptr;
    PlaybackController* m_playback = nullptr;
    QAudioDecoder* m_timingDecoder = nullptr;
    ConveyorView* m_viewer = nullptr;
    FlatView* m_flatView = nullptr;
    QWidget* m_contentHost = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MetadataWidget* m_metadata = nullptr;
    EventsWidget* m_timing = nullptr;
    PropertiesPanel* m_properties = nullptr;
    VerificationWidget* m_verification = nullptr;
    QSplitter* m_rootSplitter = nullptr;
    QSplitter* m_mainSplitter = nullptr;
    QSplitter* m_viewerSplitter = nullptr;
    QSplitter* m_bottomSplitter = nullptr;
    QToolBar* m_toolsToolbar = nullptr;
    QMenu* m_recentProjectMenu = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QComboBox* m_flatModeSelector = nullptr;
    QDoubleSpinBox* m_noteSpeedSelector = nullptr;
    QLabel* m_timingPositionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QString m_folderPath;
    ChartProject m_project;
    QString m_projectPath;
    QByteArray m_projectSongData;
    QByteArray m_projectJacketData;
    QImage m_projectJacketImage;
    Difficulty m_loadedDifficulty = Difficulty::Minimal;
    bool m_altHeld = false;
    bool m_hasProject = false;
    bool m_projectDirty = false;
    bool m_loadingProject = false;
    bool m_timingAnalysisInProgress = false;
    bool m_timingAnalysisFailed = false;
    bool m_selectingEventInWidget = false;
    bool m_splittersArranged = false;
    QString m_timingAnalysisSource;
    QString m_timingAnalysisError;
    int m_timingAnalysisSampleRate = 0;
    std::unique_ptr<InitialTimingAnalyzer> m_timingAnalyzer;
};

} // namespace infalsus::gui
