#pragma once

#include <QtCore/QString>

class QApplication;
class QWidget;

namespace infalsus::gui {

enum class ProjectFileAssociationState {
    NotRegistered,
    Registered,
    NeedsRepair,
};

[[nodiscard]] ProjectFileAssociationState projectFileAssociationState();
bool setProjectFileAssociationEnabled(bool enabled, QString* error = nullptr);
void configureSystemIntegration(QApplication& application);
void applySystemWindowIcon(QWidget& window);
void offerProjectFileAssociation(QWidget& parent);

} // namespace infalsus::gui
