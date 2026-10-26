#include "gui/widgets/field_presets_widget.h"

#include "core/timing_grid.h"
#include "gui/app/game_palette.h"
#include "gui/state/editor_state.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QRegularExpressionValidator>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace infalsus::gui {

namespace {

double ease(const std::uint32_t auxiliary, const bool right, const double progress) {
    const std::uint32_t selector = right ? (auxiliary >> 3U) : auxiliary;
    if ((selector & 0x08U) != 0) {
        return std::sin(progress * std::numbers::pi * 0.5);
    }
    if ((selector & 0x10U) != 0) {
        return 1.0 - std::cos(progress * std::numbers::pi * 0.5);
    }
    return progress;
}

QPainterPath previewPath(const FieldPreset& preset) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const FieldPresetSegment& segment : preset.segments) {
        QPolygonF polygon;
        for (const bool right : {false, true}) {
            const double sign = right ? 0.5 : -0.5;
            const double start = segment.startX + sign * segment.startWidth;
            const double end = segment.endX + sign * segment.endWidth;
            for (int index = 0; index <= 24; ++index) {
                const double progress = (right ? 24 - index : index) / 24.0;
                polygon.append({std::lerp(start, end, ease(segment.auxiliary, right, progress)),
                    1.0 - std::lerp(segment.start, segment.end, progress)});
            }
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    return path;
}

class PresetTile final : public QPushButton {
public:
    PresetTile(const FieldPreset& preset, QWidget* parent)
        : QPushButton(preset.name, parent), m_path(previewPath(preset)) {
        setFixedSize(132, 132);
        setToolTip(preset.name);
        setAccessibleName(preset.name);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool active = underMouse() || hasFocus();
        painter.fillRect(rect().adjusted(2, 2, -2, -2), active ? palette::panelHover : palette::panel);
        painter.setPen(active ? palette::textMuted : palette::chrome);
        painter.drawRect(rect().adjusted(2, 2, -3, -3));
        const QRectF preview(23, 12, 86, 86);
        painter.fillRect(preview, palette::canvasBackground);
        const QRectF bounds = m_path.boundingRect();
        if (bounds.width() > 0.0 && bounds.height() > 0.0) {
            const QRectF conveyor = preview.adjusted(3, 3, -3, -3);
            painter.save();
            painter.setClipRect(conveyor);
            painter.translate(conveyor.topLeft());
            painter.scale(conveyor.width(), conveyor.height() / bounds.height());
            painter.translate(0.0, -bounds.top());
            painter.fillPath(m_path, palette::skyArea);
            painter.restore();
        }
        painter.setPen(isEnabled() ? palette::text : palette::textMuted);
        painter.drawText(QRect(8, 103, 116, 22), Qt::AlignCenter,
            fontMetrics().elidedText(text(), Qt::ElideRight, 116));
    }

private:
    QPainterPath m_path;
};

}

FieldPresetsWidget::FieldPresetsWidget(EditorState* state, QWidget* parent)
    : QWidget(parent), m_state(state) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_addSelection = new QPushButton(QStringLiteral("Add current selection"), this);
    auto* sizeRow = new QHBoxLayout();
    sizeRow->addWidget(m_addSelection);
    m_sizeMode = new QComboBox(this);
    m_sizeMode->addItems({QStringLiteral("Time"), QStringLiteral("Divisor")});
    m_sizeMode->setToolTip(QStringLiteral("Preset duration"));
    auto* values = new QStackedWidget(this);
    m_milliseconds = new QLineEdit(QStringLiteral("100"), values);
    m_milliseconds->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{1,10}")), m_milliseconds));
    m_milliseconds->setToolTip(QStringLiteral("Duration in milliseconds"));
    m_milliseconds->setAccessibleName(QStringLiteral("Duration in milliseconds"));
    values->addWidget(m_milliseconds);
    m_divisor = new QComboBox(values);
    for (const int divisor : kSupportedDivisors) {
        m_divisor->addItem(QStringLiteral("1/%1").arg(divisor), divisor);
    }
    m_divisor->setCurrentIndex(m_divisor->findData(state->divisor()));
    m_divisor->setToolTip(QStringLiteral("Duration as a fraction of the current measure"));
    values->addWidget(m_divisor);
    sizeRow->addWidget(m_sizeMode);
    sizeRow->addWidget(values, 1);
    m_tiles = new QListWidget(this);
    m_tiles->setViewMode(QListView::IconMode);
    m_tiles->setResizeMode(QListView::Adjust);
    m_tiles->setMovement(QListView::Static);
    m_tiles->setSelectionMode(QAbstractItemView::NoSelection);
    m_tiles->setGridSize(QSize(140, 140));
    m_tiles->setSpacing(0);
    m_tiles->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tiles->setMinimumSize(152, 150);
    layout->addWidget(m_tiles, 1);
    layout->addLayout(sizeRow);
    m_presets.append(circleFieldPreset());
    loadPresets();
    refreshTiles();
    connect(m_sizeMode, &QComboBox::currentIndexChanged, values, &QStackedWidget::setCurrentIndex);
    const auto updateSize = [this] {
        m_tiles->setEnabled(durationMilliseconds() > 0);
    };
    connect(m_sizeMode, &QComboBox::currentIndexChanged, this, updateSize);
    connect(m_milliseconds, &QLineEdit::textChanged, this, updateSize);
    connect(m_divisor, &QComboBox::currentIndexChanged, this, updateSize);
    connect(m_addSelection, &QPushButton::clicked, this, &FieldPresetsWidget::addSelection);
    connect(state, &EditorState::selectionChanged, this, &FieldPresetsWidget::updateSelection);
    connect(state, &EditorState::hitObjectsChanged, this, &FieldPresetsWidget::updateSelection);
    updateSelection();
}

void FieldPresetsWidget::refreshTiles() {
    m_tiles->clear();
    for (int index = 0; index < m_presets.size(); ++index) {
        auto* item = new QListWidgetItem(m_tiles);
        item->setSizeHint(QSize(132, 132));
        auto* tile = new PresetTile(m_presets.at(index), m_tiles);
        m_tiles->setItemWidget(item, tile);
        connect(tile, &QPushButton::clicked, this, [this, index] { placePreset(index); });
        if (index > 0) {
            tile->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(tile, &QWidget::customContextMenuRequested, this, [this, index, tile](const QPoint& point) {
                QMenu menu(this);
                auto* remove = menu.addAction(QStringLiteral("Delete preset"));
                if (menu.exec(tile->mapToGlobal(point)) == remove) {
                    m_presets.removeAt(index);
                    savePresets();
                    refreshTiles();
                }
            });
        }
    }
}

void FieldPresetsWidget::updateSelection() {
    const auto& notes = m_state->chart().notes;
    const auto& selected = m_state->selectedHitObjects();
    m_addSelection->setEnabled(!selected.isEmpty()
        && std::all_of(selected.cbegin(), selected.cend(), [&notes](const int index) {
            return index >= 0 && index < notes.size() && notes.at(index).kind == NoteKind::Sky
                && notes.at(index).endMilliseconds > notes.at(index).startMilliseconds;
        }));
}

qint64 FieldPresetsWidget::durationMilliseconds() const {
    if (m_sizeMode->currentIndex() == 0) {
        bool valid = false;
        const qint64 value = m_milliseconds->text().toLongLong(&valid);
        return valid && value >= 1 && value <= std::numeric_limits<std::uint32_t>::max() ? value : 0;
    }
    TimingPoint point;
    for (const TimingPoint& candidate : m_state->timingPoints()) {
        if (candidate.timeMilliseconds > m_state->playbackPosition()) {
            break;
        }
        point = candidate;
    }
    return std::max<qint64>(1, std::llround(detail::measureDurationMilliseconds(point)
        / m_divisor->currentData().toInt()));
}

void FieldPresetsWidget::addSelection() {
    QVector<ChartNote> fields;
    for (const int index : m_state->selectedHitObjects()) {
        if (index < 0 || index >= m_state->chart().notes.size()) {
            return;
        }
        const ChartNote& field = m_state->chart().notes.at(index);
        fields.append(field);
    }
    if (fields.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Add field preset"),
        QStringLiteral("Name"), QLineEdit::Normal, QStringLiteral("Field %1").arg(m_presets.size()), &accepted).trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    FieldPreset preset = captureFieldPreset(fields, name);
    if (!preset.segments.isEmpty()) {
        m_presets.append(std::move(preset));
        savePresets();
        refreshTiles();
    }
}

void FieldPresetsWidget::placePreset(const int index) {
    const qint64 duration = durationMilliseconds();
    const qint64 start = m_state->playbackPosition();
    if (duration < 1 || start > std::numeric_limits<std::uint32_t>::max() - duration) {
        return;
    }
    QVector<ChartNote> fields = instantiateFieldPreset(m_presets.at(index), start, duration);
    if (fields.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Field presets"),
            QStringLiteral("Increase the duration so each field segment is at least 2 ms long."));
        return;
    }
    m_state->copyHitObjects(std::move(fields));
}

void FieldPresetsWidget::loadPresets() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    const QJsonDocument document = QJsonDocument::fromJson(settings.value(QStringLiteral("fields/presets_v2")).toByteArray());
    for (const QJsonValue& value : document.array()) {
        const QJsonObject object = value.toObject();
        FieldPreset preset{.name = object.value(QStringLiteral("name")).toString().trimmed()};
        bool valid = !preset.name.isEmpty();
        for (const QJsonValue& entry : object.value(QStringLiteral("segments")).toArray()) {
            const QJsonArray values = entry.toArray();
            if (values.size() != 7 || !std::all_of(values.cbegin(), values.cend(), [](const QJsonValue& number) {
                    return number.isDouble() && std::isfinite(number.toDouble());
                })) {
                valid = false;
                break;
            }
            FieldPresetSegment segment{.start = values.at(0).toDouble(), .end = values.at(1).toDouble(),
                .startX = values.at(2).toDouble(), .endX = values.at(3).toDouble(),
                .startWidth = values.at(4).toDouble(), .endWidth = values.at(5).toDouble()};
            const double auxiliary = values.at(6).toDouble();
            if (segment.start < 0.0 || segment.end > 1.0 || segment.end <= segment.start
                || segment.startWidth < 0.0 || segment.endWidth < 0.0
                || auxiliary < 0.0 || auxiliary > std::numeric_limits<std::uint32_t>::max()) {
                valid = false;
                break;
            }
            segment.auxiliary = static_cast<std::uint32_t>(auxiliary);
            preset.segments.append(segment);
        }
        if (valid && !preset.segments.isEmpty()) {
            m_presets.append(std::move(preset));
        }
    }
}

void FieldPresetsWidget::savePresets() const {
    QJsonArray presets;
    for (int index = 1; index < m_presets.size(); ++index) {
        const FieldPreset& preset = m_presets.at(index);
        QJsonArray segments;
        for (const FieldPresetSegment& segment : preset.segments) {
            segments.append(QJsonArray{segment.start, segment.end, segment.startX, segment.endX,
                segment.startWidth, segment.endWidth, static_cast<double>(segment.auxiliary)});
        }
        presets.append(QJsonObject{{QStringLiteral("name"), preset.name}, {QStringLiteral("segments"), segments}});
    }
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("fields/presets_v2"), QJsonDocument(presets).toJson(QJsonDocument::Compact));
}

}
