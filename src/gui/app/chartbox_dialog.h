#pragma once

#include <QtWidgets/QDialog>
#include <QtCore/QByteArray>

#include <functional>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QUrl;

namespace infalsus::gui {

class ChartboxDialog final : public QDialog {
public:
    explicit ChartboxDialog(const QString& chartReference, bool hasProject, QWidget* parent = nullptr);

    void setChartPresentOnServer(bool present);
    void setUploadHandlers(std::function<QString()> prepareUpload,
        std::function<bool(const QString&)> saveReference);

protected:
    void reject() override;

private:
    void refreshStatus();
    void uploadProject();
    void deleteChart();
    void openChart();
    void sendUpload();
    void finishMutation(const QString& status);
    void updateControls();
    void request(const QUrl& url, const QByteArray& method, bool authenticated, const QByteArray& body,
        std::function<void(QNetworkReply*)> handleReply);

    QString m_chartReference;
    bool m_hasProject = false;
    QLineEdit* m_apiKey = nullptr;
    QLabel* m_serverStatus = nullptr;
    QLabel* m_userStatus = nullptr;
    QLabel* m_chartStatus = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QPushButton* m_uploadButton = nullptr;
    QPushButton* m_showKeyButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QPushButton* m_openButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_closeButton = nullptr;
    std::function<QString()> m_prepareUpload;
    std::function<bool(const QString&)> m_saveReference;
    QString m_uploadFileName;
    QByteArray m_uploadData;
    bool m_mutationInProgress = false;
    bool m_updateExisting = false;
    int m_pendingRequests = 0;
    int m_refreshGeneration = 0;
};

} // namespace infalsus::gui
