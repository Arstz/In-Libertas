#include "gui/widgets/metadata_widget.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

#include <algorithm>

namespace infalsus::gui {

namespace {

struct GameplayBackgroundOption {
    const char* songKey;
    int value;
};

// These keys are representative vanilla songs for each populated entry in
// GameplayBackgrounds. The stored value is the native enum/index value.
constexpr GameplayBackgroundOption kGameplayBackgroundOptions[] = {
    {"fallingshadow", 0}, {"greyscalecity", 1}, {"shouldhavebeen", 2}, {"alamode", 3},
    {"newvision", 4}, {"bethere", 5}, {"init", 6}, {"coldsea", 7},
    {"destroyer", 8}, {"tutorialevolong", 9}, {"yggthrasir", 10}, {"chronomia", 11},
    {"cryogenic", 12}, {"ordirehv", 13}, {"lightsout", 14}, {"exceedmind", 16},
    {"falsequre", 17}, {"primevaltexture", 18}, {"latentduality", 19}, {"enigma", 20},
    {"tactom", 21}, {"cyaegha", 22}, {"ffff", 23}, {"alterededge", 27},
    {"mydeadlysins", 29}, {"lfdy", 31}, {"tokonagi", 32}, {"kuukaku", 33},
    {"fochaos", 34}, {"memoire", 36}, {"kiretsu", 37}, {"codeleviathan", 38},
    {"chronophobia", 39}, {"moonsliders", 40}, {"phylaxron", 41}, {"thingsitreasure", 42},
    {"hanabi", 43}, {"chaoticismlegacy", 44}, {"everything", 45}, {"toccatafunebre", 46},
};

} // namespace

MetadataWidget::MetadataWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QFormLayout(this);
    m_songName = new QLineEdit(this);
    m_artistName = new QLineEdit(this);
    m_previewStartMilliseconds = new QSpinBox(this);
    m_previewEndMilliseconds = new QSpinBox(this);
    for (QSpinBox* spinBox : {m_previewStartMilliseconds, m_previewEndMilliseconds}) {
        spinBox->setRange(0, 36000000);
        spinBox->setSingleStep(100);
        spinBox->setSuffix(QStringLiteral(" ms"));
    }
    m_previewStartCurrentTimeButton = new QPushButton(QStringLiteral("Set current time"), this);
    m_previewEndCurrentTimeButton = new QPushButton(QStringLiteral("Set current time"), this);
    m_gameplayBackground = new QComboBox(this);
    m_difficulty = new QComboBox(this);
    m_chartId = new QLineEdit(this);
    m_rating = new QSpinBox(this);
    m_rating->setRange(0, 99);
    m_chartDesigner = new QLineEdit(this);
    m_jacketDesigner = new QLineEdit(this);
    m_jacketButton = new QPushButton(this);
    for (int index = 0; index < 4; ++index) {
        m_difficulty->addItem(difficultyName(difficultyForIndex(index)));
    }
    for (const GameplayBackgroundOption& option : kGameplayBackgroundOptions) {
        m_gameplayBackground->addItem(
            QStringLiteral("%1 (%2)").arg(QString::fromLatin1(option.songKey)).arg(option.value),
            QString::number(option.value));
    }

    layout->addRow(QStringLiteral("Song"), m_songName);
    layout->addRow(QStringLiteral("Artist"), m_artistName);
    auto* previewStartRow = new QWidget(this);
    auto* previewStartLayout = new QHBoxLayout(previewStartRow);
    previewStartLayout->setContentsMargins(0, 0, 0, 0);
    previewStartLayout->addWidget(m_previewStartMilliseconds, 1);
    previewStartLayout->addWidget(m_previewStartCurrentTimeButton);
    auto* previewEndRow = new QWidget(this);
    auto* previewEndLayout = new QHBoxLayout(previewEndRow);
    previewEndLayout->setContentsMargins(0, 0, 0, 0);
    previewEndLayout->addWidget(m_previewEndMilliseconds, 1);
    previewEndLayout->addWidget(m_previewEndCurrentTimeButton);
    layout->addRow(QStringLiteral("Preview start"), previewStartRow);
    layout->addRow(QStringLiteral("Preview end"), previewEndRow);
    layout->addRow(QStringLiteral("Gameplay background"), m_gameplayBackground);
    layout->addRow(QStringLiteral("Chart designer"), m_chartDesigner);
    layout->addRow(QStringLiteral("Jacket designer"), m_jacketDesigner);
    layout->addRow(QStringLiteral("Difficulty"), m_difficulty);
    layout->addRow(QStringLiteral("Chart ID"), m_chartId);
    layout->addRow(QStringLiteral("Rating"), m_rating);
    layout->addRow(QStringLiteral("Jacket"), m_jacketButton);
    setJacketAvailable(false);
    setJacketEditingEnabled(false);

    connect(m_songName, &QLineEdit::editingFinished, this, &MetadataWidget::emitMetadata);
    connect(m_artistName, &QLineEdit::editingFinished, this, &MetadataWidget::emitMetadata);
    connect(m_previewStartMilliseconds, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emitMetadata(); });
    connect(m_previewEndMilliseconds, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emitMetadata(); });
    connect(m_gameplayBackground, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { emitMetadata(); });
    connect(m_chartDesigner, &QLineEdit::editingFinished, this, &MetadataWidget::emitMetadata);
    connect(m_jacketDesigner, &QLineEdit::editingFinished, this, &MetadataWidget::emitMetadata);
    connect(m_chartId, &QLineEdit::editingFinished, this, [this] { emit chartIdEdited(m_chartId->text().trimmed()); });
    connect(m_rating, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emitDifficultyMetadata(); });
    connect(m_jacketButton, &QPushButton::clicked, this, &MetadataWidget::jacketRequested);
    connect(m_previewStartCurrentTimeButton, &QPushButton::clicked,
        this, &MetadataWidget::previewStartCurrentTimeRequested);
    connect(m_previewEndCurrentTimeButton, &QPushButton::clicked,
        this, &MetadataWidget::previewEndCurrentTimeRequested);
    connect(m_difficulty, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (!m_loading) {
            emit difficultyRequested(difficultyForIndex(index));
        }
    });
}

void MetadataWidget::setMetadata(const ChartMetadata& metadata) {
    m_loading = true;
    const QSignalBlocker songBlocker(m_songName);
    const QSignalBlocker artistBlocker(m_artistName);
    const QSignalBlocker previewStartBlocker(m_previewStartMilliseconds);
    const QSignalBlocker previewEndBlocker(m_previewEndMilliseconds);
    const QSignalBlocker backgroundBlocker(m_gameplayBackground);
    const QSignalBlocker chartDesignerBlocker(m_chartDesigner);
    const QSignalBlocker jacketDesignerBlocker(m_jacketDesigner);
    m_songName->setText(metadata.songName);
    m_artistName->setText(metadata.artistName);
    m_chartDesigner->setText(metadata.chartDesigner);
    m_jacketDesigner->setText(metadata.jacketDesigner);
    m_previewStartMilliseconds->setValue(static_cast<int>(metadata.previewStartSeconds * 1000.0));
    m_previewEndMilliseconds->setValue(static_cast<int>(metadata.previewEndSeconds * 1000.0));
    m_hiddenCharacterIdentifier = metadata.characterIdentifier;
    int backgroundIndex = m_gameplayBackground->findData(metadata.gameplayBackground);
    if (backgroundIndex < 0 && !metadata.gameplayBackground.isEmpty()) {
        m_gameplayBackground->addItem(
            QStringLiteral("Unknown background (%1)").arg(metadata.gameplayBackground), metadata.gameplayBackground);
        backgroundIndex = m_gameplayBackground->count() - 1;
    }
    m_gameplayBackground->setCurrentIndex(std::max(backgroundIndex, 0));
    m_loading = false;
}

void MetadataWidget::setDifficultyMetadata(const DifficultyMetadata& metadata) {
    m_loading = true;
    const QSignalBlocker ratingBlocker(m_rating);
    m_rating->setValue(metadata.rating);
    m_loading = false;
}

void MetadataWidget::setChartId(const QString& chartId) {
    const QSignalBlocker blocker(m_chartId);
    m_chartId->setText(chartId);
}

void MetadataWidget::setDifficulty(const Difficulty difficulty) {
    m_loading = true;
    const QSignalBlocker blocker(m_difficulty);
    m_difficulty->setCurrentIndex(difficultyIndex(difficulty));
    m_loading = false;
}

void MetadataWidget::setJacketAvailable(const bool available) {
    m_jacketButton->setText(available ? QStringLiteral("Change jacket") : QStringLiteral("Add jacket"));
}

void MetadataWidget::setJacketEditingEnabled(const bool enabled) {
    m_jacketButton->setEnabled(enabled);
}

void MetadataWidget::setPreviewStartMilliseconds(const qint64 milliseconds) {
    m_previewStartMilliseconds->setValue(static_cast<int>(milliseconds));
}

void MetadataWidget::setPreviewEndMilliseconds(const qint64 milliseconds) {
    m_previewEndMilliseconds->setValue(static_cast<int>(milliseconds));
}

void MetadataWidget::emitMetadata() {
    if (m_loading) {
        return;
    }
    emit metadataEdited({
        .artistName = m_artistName->text(),
        .songName = m_songName->text(),
        .chartDesigner = m_chartDesigner->text(),
        .jacketDesigner = m_jacketDesigner->text(),
        // The hook's JSON contract uses seconds, but editor input and the
        // playback clock use integral milliseconds.
        .previewStartSeconds = m_previewStartMilliseconds->value() / 1000.0,
        .previewEndSeconds = m_previewEndMilliseconds->value() / 1000.0,
        .characterIdentifier = m_hiddenCharacterIdentifier,
        .gameplayBackground = m_gameplayBackground->currentData().toString(),
    });
}

void MetadataWidget::emitDifficultyMetadata() {
    if (m_loading) {
        return;
    }
    emit difficultyMetadataEdited({
        .rating = m_rating->value(),
    });
}

} // namespace infalsus::gui
