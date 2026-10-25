#pragma once

#include "core/chart_project.h"
#include "core/timing_analyzer.h"
#include "core/playtest_session.h"
#include "gui/app/visual_settings.h"
#include "gui/app/handling_settings.h"
#include "gui/app/playtest_settings.h"

#include <QtCore/QByteArray>
#include <QtCore/QFutureWatcher>
#include <QtCore/QPointer>
#include <QtGui/QImage>
#include <QtWidgets/QMainWindow>

class ConveyorView;
class PlaybackController;
class HitsoundBank;
class QAudioFormat;
class QButtonGroup;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QMenu;
class QShowEvent;
class QCloseEvent;
class QSplitter;
class QSlider;
class QToolBar;
class QToolButton;
class QTimer;
class QWidget;

namespace infalsus::gui {

class EditorState;
class FlatView;
class KeyBindingRouter;
class MetadataWidget;
class PropertiesPanel;
class PlaybackControls;
class TimelineWidget;
class EventsWidget;
class VerificationWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openProjectFile(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildInterface();
    void buildToolBar();
    void updateToolButtonSizes();
    void registerNavigationKeybinds();
    void setSkyGridSize(int divisor);
    void openSettings();
    void interpolateSpeedEvents();
    void openChartbox();
    void updateVolumeSliderPlacement();
    void buildWorkspace();
    void arrangeSplitters();
    void updateBottomPanelVisibility();
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
    bool saveProject();
    void saveProjectAs();
    [[nodiscard]] bool requireChartId(ChartProject* project);
    [[nodiscard]] bool normalizeAssets(ChartProject* project, QByteArray* audio, QByteArray* jacket,
        bool normalizeAudioPeak = false);
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
    void enterPlaytest();
    void leavePlaytest();
    void updatePlaytest();
    void publishPlaytest();
    void constrainPlaytestCursor();
    struct PlaytestClock { double chartTime; double monotonicTime; };
    [[nodiscard]] PlaytestClock samplePlaytestClock() const;
    void adjustFlatZoom(double steps);
    void startInitialTimingAnalysis();
    void analyzeInitialTimingAudio(QByteArray samples, const QAudioFormat& format);
    void finishInitialTimingAnalysis(const TimingEstimate& estimate);
    void cancelInitialTimingAnalysis();
    [[nodiscard]] QString audioPathForFolder(const QString& folderPath) const;

    EditorState* m_state = nullptr;
    VisualSettings m_visualSettings;
    HandlingSettings m_handlingSettings;
    PlaytestSettings m_playtestSettings;
    KeyBindingRouter* m_keyBindings = nullptr;
    PlaybackController* m_playback = nullptr;
    HitsoundBank* m_hitsounds = nullptr;
    QFutureWatcher<TimingEstimate>* m_timingWatcher = nullptr;
    ConveyorView* m_viewer = nullptr;
    FlatView* m_flatView = nullptr;
    QWidget* m_contentHost = nullptr;
    TimelineWidget* m_timeline = nullptr;
    PlaybackControls* m_playbackControls = nullptr;
    MetadataWidget* m_metadata = nullptr;
    EventsWidget* m_timing = nullptr;
    PropertiesPanel* m_properties = nullptr;
    VerificationWidget* m_verification = nullptr;
    QSplitter* m_rootSplitter = nullptr;
    QSplitter* m_mainSplitter = nullptr;
    QSplitter* m_viewerSplitter = nullptr;
    QSplitter* m_bottomSplitter = nullptr;
    std::array<QWidget*, 4> m_workspacePanels{};
    QToolBar* m_toolsToolbar = nullptr;
    QMenu* m_windowMenu = nullptr;
    QMenu* m_recentProjectMenu = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QSlider* m_effectsVolumeSlider = nullptr;
    QLabel* m_songVolumeLabel = nullptr;
    QLabel* m_effectsVolumeLabel = nullptr;
    QComboBox* m_flatModeSelector = nullptr;
    QToolButton* m_skyGridToggle = nullptr;
    QButtonGroup* m_skyGridSizes = nullptr;
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
    bool m_selectingEventInWidget = false;
    bool m_splittersArranged = false;
    PlaytestSession m_playtestSession;
    QTimer* m_playtestTimer = nullptr;
    QWidget* m_viewerEditorParent = nullptr;
    QPointer<QWidget> m_editorFocus;
    QByteArray m_playtestWindowState;
    std::array<QByteArray, 4> m_playtestSplitters;
    QPoint m_editorCursor;
    qint64 m_editorPosition = 0;
    qint64 m_pendingDuration = 0;
    double m_leadInEnd = 0.0;
    int m_viewerLayoutIndex = 0;
    bool m_playtest = false;
    bool m_playtestLeadIn = false;
    bool m_playtestFinished = false;
    bool m_editorMenuVisible = true;
    bool m_editorToolsVisible = true;
    bool m_fieldContact = false;
};

} // namespace infalsus::gui
