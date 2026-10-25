#include "gui/app/system_integration.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace infalsus::gui {

namespace {

constexpr auto kAssociationPromptedSettingsKey = "system/associationPrompted";
constexpr auto kProjectProgId = "InLibertas.Project";
constexpr auto kApplicationFileName = "In Libertas.exe";
constexpr int kApplicationIconResource = 101;
constexpr int kProjectIconResource = 201;

#ifdef Q_OS_WIN
QString executablePath() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

QString iconRegistryValue(const int resourceId) {
    return QStringLiteral("\"%1\",-%2").arg(executablePath()).arg(resourceId);
}

QString openCommandRegistryValue() {
    return QStringLiteral("\"") + executablePath() + QStringLiteral("\" \"%1\"");
}

bool setRegistryValue(QSettings& settings, const QString& key, const QVariant& value) {
    if (settings.contains(key) && settings.value(key) == value) {
        return false;
    }
    settings.setValue(key, value);

    return true;
}

bool removeRegistryValue(QSettings& settings, const QString& key) {
    if (!settings.contains(key)) {
        return false;
    }
    settings.remove(key);

    return true;
}

bool associationOwnedByApplication() {
    const QSettings extension(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.10no"),
        QSettings::NativeFormat);

    return extension.value(QStringLiteral(".")).toString() == QLatin1String(kProjectProgId);
}
#endif

} // namespace

ProjectFileAssociationState projectFileAssociationState() {
#ifdef Q_OS_WIN
    if (!associationOwnedByApplication()) {
        return ProjectFileAssociationState::NotRegistered;
    }
    const QSettings extension(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.10no"),
        QSettings::NativeFormat);
    const QSettings project(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1")
        .arg(QLatin1String(kProjectProgId)), QSettings::NativeFormat);
    const QSettings application(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Applications\\%1")
        .arg(QLatin1String(kApplicationFileName)), QSettings::NativeFormat);
    const bool current = extension.contains(QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)))
        && project.value(QStringLiteral("DefaultIcon/.")).toString() == iconRegistryValue(kProjectIconResource)
        && project.value(QStringLiteral("shell/open/command/.")).toString() == openCommandRegistryValue()
        && application.value(QStringLiteral("DefaultIcon/.")).toString() == iconRegistryValue(kApplicationIconResource)
        && application.value(QStringLiteral("shell/open/command/.")).toString() == openCommandRegistryValue()
        && application.contains(QStringLiteral("SupportedTypes/.10no"));

    return current ? ProjectFileAssociationState::Registered : ProjectFileAssociationState::NeedsRepair;
#else
    return ProjectFileAssociationState::NotRegistered;
#endif
}

bool setProjectFileAssociationEnabled(const bool enabled, QString* error) {
#ifdef Q_OS_WIN
    QSettings extension(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.10no"),
        QSettings::NativeFormat);
    QSettings project(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1")
        .arg(QLatin1String(kProjectProgId)), QSettings::NativeFormat);
    QSettings application(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Applications\\%1")
        .arg(QLatin1String(kApplicationFileName)), QSettings::NativeFormat);
    bool changed = false;

    if (enabled) {
        changed |= setRegistryValue(extension, QStringLiteral("."), QLatin1String(kProjectProgId));
        changed |= setRegistryValue(extension,
            QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)), QString());
        changed |= setRegistryValue(project, QStringLiteral("."), QStringLiteral("In Libertas Project"));
        changed |= setRegistryValue(project, QStringLiteral("DefaultIcon/."), iconRegistryValue(kProjectIconResource));
        changed |= setRegistryValue(project, QStringLiteral("shell/open/command/."), openCommandRegistryValue());
        changed |= setRegistryValue(application, QStringLiteral("FriendlyAppName"), QStringLiteral("In Libertas"));
        changed |= setRegistryValue(application, QStringLiteral("DefaultIcon/."), iconRegistryValue(kApplicationIconResource));
        changed |= setRegistryValue(application, QStringLiteral("shell/open/command/."), openCommandRegistryValue());
        changed |= setRegistryValue(application, QStringLiteral("SupportedTypes/.10no"), QString());
    } else {
        if (associationOwnedByApplication()) {
            changed |= removeRegistryValue(extension, QStringLiteral("."));
        }
        changed |= removeRegistryValue(extension,
            QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)));
        if (!project.allKeys().isEmpty()) {
            project.clear();
            changed = true;
        }
        if (!application.allKeys().isEmpty()) {
            application.clear();
            changed = true;
        }
    }

    extension.sync();
    project.sync();
    application.sync();
    const bool success = extension.status() == QSettings::NoError
        && project.status() == QSettings::NoError && application.status() == QSettings::NoError;
    if (!success) {
        if (error != nullptr) {
            *error = QStringLiteral("Windows could not update the .10no file association.");
        }
        return false;
    }
    if (changed) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    return true;
#else
    Q_UNUSED(enabled);
    if (error != nullptr) {
        *error = QStringLiteral("File association is available only on Windows.");
    }

    return false;
#endif
}

void configureSystemIntegration(QApplication& application) {
    application.setWindowIcon(QIcon(QStringLiteral(":/system/Logo.png")));
}

void applySystemWindowIcon(QWidget& window) {
    window.setWindowIcon(QApplication::windowIcon());

#ifdef Q_OS_WIN
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HICON largeIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kApplicationIconResource),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    const HICON smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kApplicationIconResource),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    if (largeIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    }
    if (smallIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
#endif
}

void offerProjectFileAssociation(QWidget& parent) {
#ifdef Q_OS_WIN
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    if (settings.value(QLatin1String(kAssociationPromptedSettingsKey), false).toBool()
        || associationOwnedByApplication()) {
        settings.setValue(QLatin1String(kAssociationPromptedSettingsKey), true);
        return;
    }

    QMessageBox box(&parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("File Association"));
    box.setText(QStringLiteral("Associate .10no project files with In Libertas?"));
    QPushButton* yes = box.addButton(QStringLiteral("Yes"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(yes);
    box.exec();
    settings.setValue(QLatin1String(kAssociationPromptedSettingsKey), true);
    if (box.clickedButton() != yes) {
        return;
    }

    QString error;
    if (!setProjectFileAssociationEnabled(true, &error)) {
        QMessageBox::warning(&parent, QStringLiteral("File Association"), error);
    }
#else
    Q_UNUSED(parent);
#endif
}

} // namespace infalsus::gui
