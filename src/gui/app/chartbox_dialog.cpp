#include "gui/app/chartbox_dialog.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <utility>

namespace infalsus::gui {

namespace {

constexpr qint64 kMaximumUploadBytes = 50LL * 1024 * 1024;

QUrl chartUrl(const QString& reference) {
    QUrl url(QStringLiteral("https://chartbox.app/api/charts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("reference"), reference);
    query.addQueryItem(QStringLiteral("data"), QStringLiteral("false"));
    url.setQuery(query);
    return url;
}

QString requestError(const QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403) {
        return QStringLiteral("Invalid API key or access denied");
    }
    if (status >= 400) {
        return QStringLiteral("HTTP %1").arg(status);
    }
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        return QStringLiteral("Request timed out");
    }
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }
    return {};
}

QString userName(const QJsonObject& response) {
    const QString handle = response.value(QStringLiteral("user")).toObject()
        .value(QStringLiteral("handle")).toString().trimmed();
    return handle.isEmpty() ? QStringLiteral("Authenticated (name unavailable)") : handle;
}

QString responseError(const QNetworkReply* reply, const QJsonObject& response) {
    QString error = requestError(reply);
    const QString message = response.value(QStringLiteral("message")).toString();
    if (!error.isEmpty() && !message.isEmpty()) {
        error += QStringLiteral(": %1").arg(message);
    }
    return error;
}

struct ChartLookupResult {
    bool present = false;
    QString error;
};

ChartLookupResult chartLookupResult(QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (status == 404 && document.isObject()) {
        return {};
    }
    const QString error = requestError(reply);
    if (!error.isEmpty()) {
        return {.error = error};
    }
    const QJsonObject response = document.object();
    if (response.value(QStringLiteral("success")).toBool()
        && response.value(QStringLiteral("metadata")).isObject()) {
        return {.present = true};
    }
    return {.error = QStringLiteral("Unexpected response")};
}

} // namespace

ChartboxDialog::ChartboxDialog(const QString& chartReference, const bool hasProject, QWidget* parent)
    : QDialog(parent)
    , m_chartReference(chartReference.trimmed())
    , m_hasProject(hasProject) {
    auto* layout = new QHBoxLayout(this);
    auto* content = new QVBoxLayout;
    auto* fields = new QFormLayout;
    auto* settings = new QSettings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"), this);
    m_apiKey = new QLineEdit(this);
    m_showKeyButton = new QPushButton(QStringLiteral("Show"), this);
    auto* actions = new QVBoxLayout;
    m_network = new QNetworkAccessManager(this);
    m_uploadButton = new QPushButton(this);
    m_deleteButton = new QPushButton(QStringLiteral("Delete"), this);
    m_openButton = new QPushButton(QStringLiteral("Open"), this);
    m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    m_closeButton = new QPushButton(QStringLiteral("Close"), this);
    m_serverStatus = new QLabel(QStringLiteral("Server status - Not checked"), this);
    m_userStatus = new QLabel(QStringLiteral("Logged in as - Not checked"), this);
    m_chartStatus = new QLabel(m_hasProject ? QStringLiteral("Current chart status - Not checked")
        : QStringLiteral("Current chart status - No project loaded"), this);

    setWindowTitle(QStringLiteral("Chartbox"));
    setMinimumWidth(630);
    const QString disabledButtonStyle = QStringLiteral(
        "QPushButton:disabled { background-color: #303030; color: #888888; "
        "border: 1px solid #484848; border-radius: 3px; }");
    for (auto* button : {m_showKeyButton, m_uploadButton, m_deleteButton, m_openButton, m_refreshButton, m_closeButton}) {
        button->setMinimumSize(button->sizeHint());
        button->setStyleSheet(disabledButtonStyle);
    }
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(QStringLiteral("Enter your API key"));
    m_apiKey->setText(settings->value(QStringLiteral("chartbox/api_key")).toString());
    m_showKeyButton->setToolTip(QStringLiteral("Hold to show the API key"));
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    fields->addRow(QStringLiteral("API key"), m_apiKey);
    content->addLayout(fields);
    content->addSpacing(8);
    for (auto* status : {m_serverStatus, m_userStatus, m_chartStatus}) {
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        content->addWidget(status);
    }
    content->addStretch();
    actions->setSpacing(8);
    for (auto* button : {m_showKeyButton, m_uploadButton, m_deleteButton, m_openButton, m_refreshButton}) {
        button->setAutoDefault(false);
        actions->addWidget(button);
    }
    m_closeButton->setAutoDefault(false);
    actions->addSpacing(m_closeButton->sizeHint().height());
    actions->addWidget(m_closeButton);
    layout->addLayout(content, 1);
    layout->addSpacing(8);
    layout->addLayout(actions);
    layout->setAlignment(actions, Qt::AlignTop);
    layout->activate();
    resize(690, sizeHint().height());
    setChartPresentOnServer(false);
    connect(m_apiKey, &QLineEdit::textChanged, this, [this, settings](const QString& key) {
        settings->setValue(QStringLiteral("chartbox/api_key"), key);
        ++m_refreshGeneration;
        m_serverStatus->setText(QStringLiteral("Server status - Refresh required"));
        m_userStatus->setText(QStringLiteral("Logged in as - Refresh required"));
        m_chartStatus->setText(m_hasProject ? QStringLiteral("Current chart status - Refresh required")
            : QStringLiteral("Current chart status - No project loaded"));
        setChartPresentOnServer(false);
        updateControls();
    });
    connect(m_showKeyButton, &QPushButton::pressed, this, [this] {
        m_apiKey->setEchoMode(QLineEdit::Normal);
    });
    connect(m_showKeyButton, &QPushButton::released, this, [this] {
        m_apiKey->setEchoMode(QLineEdit::Password);
    });
    connect(m_refreshButton, &QPushButton::clicked, this, &ChartboxDialog::refreshStatus);
    connect(m_uploadButton, &QPushButton::clicked, this, &ChartboxDialog::uploadProject);
    connect(m_deleteButton, &QPushButton::clicked, this, &ChartboxDialog::deleteChart);
    connect(m_openButton, &QPushButton::clicked, this, &ChartboxDialog::openChart);
    connect(m_closeButton, &QPushButton::clicked, this, &ChartboxDialog::reject);
    updateControls();
    QTimer::singleShot(0, this, &ChartboxDialog::refreshStatus);
}

void ChartboxDialog::setChartPresentOnServer(const bool present) {
    m_uploadButton->setText(present ? QStringLiteral("Update") : QStringLiteral("Upload"));
}

void ChartboxDialog::setUploadHandlers(std::function<QString()> prepareUpload,
    std::function<bool(const QString&)> saveReference) {
    m_prepareUpload = std::move(prepareUpload);
    m_saveReference = std::move(saveReference);
    updateControls();
}

void ChartboxDialog::reject() {
    if (m_pendingRequests == 0 && !m_mutationInProgress) {
        QDialog::reject();
    }
}

void ChartboxDialog::updateControls() {
    const bool idle = m_pendingRequests == 0 && !m_mutationInProgress;
    const bool hasApiKey = !m_apiKey->text().trimmed().isEmpty();
    m_showKeyButton->setEnabled(idle);
    m_refreshButton->setEnabled(idle);
    m_uploadButton->setEnabled(idle && m_hasProject && m_prepareUpload && m_saveReference
        && hasApiKey);
    m_deleteButton->setEnabled(idle && m_hasProject && hasApiKey && m_saveReference && !m_chartReference.isEmpty());
    m_openButton->setEnabled(idle && m_hasProject && !m_chartReference.isEmpty());
    m_apiKey->setEnabled(idle);
    m_closeButton->setEnabled(idle);
    if (!idle) {
        m_apiKey->setEchoMode(QLineEdit::Password);
    }
}

void ChartboxDialog::request(const QUrl& url, const QByteArray& method, const bool authenticated,
    const QByteArray& body,
    std::function<void(QNetworkReply*)> handleReply) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        authenticated && method == "GET" ? QNetworkRequest::SameOriginRedirectPolicy
            : QNetworkRequest::ManualRedirectPolicy);
    if (authenticated) {
        request.setRawHeader("Authorization", "Bearer " + m_apiKey->text().trimmed().toUtf8());
        request.setRawHeader("Accept", "application/json");
    }
    const bool hasUploadBody = method == "PUT" || method == "PATCH";
    if (hasUploadBody) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        request.setHeader(QNetworkRequest::ContentLengthHeader, qint64(body.size()));
    }
    ++m_pendingRequests;
    updateControls();
    const int generation = m_refreshGeneration;
    QNetworkReply* reply = method == "GET" ? m_network->get(request)
        : m_network->sendCustomRequest(request, method, body);
    QTimer::singleShot(hasUploadBody ? 120000 : 15000, reply, [reply] {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, handleReply] {
        if (generation == m_refreshGeneration) {
            handleReply(reply);
        }
        --m_pendingRequests;
        updateControls();
        reply->deleteLater();
    });
}

void ChartboxDialog::refreshStatus() {
    if (m_pendingRequests != 0 || m_mutationInProgress) {
        return;
    }
    ++m_refreshGeneration;
    m_refreshButton->setEnabled(false);
    setChartPresentOnServer(false);
    m_serverStatus->setText(QStringLiteral("Server status - Checking..."));
    request(QUrl(QStringLiteral("https://chartbox.app/")), "GET", false, {}, [this](QNetworkReply* reply) {
        QString result = requestError(reply);
        if (result.isEmpty()) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            result = status >= 200 && status < 400
                ? QStringLiteral("Online") : QStringLiteral("Unexpected response");
        }
        m_serverStatus->setText(QStringLiteral("Server status - %1").arg(result));
    });
    if (m_apiKey->text().trimmed().isEmpty()) {
        m_userStatus->setText(QStringLiteral("Logged in as - API key required"));
        m_chartStatus->setText(QStringLiteral("Current chart status - %1").arg(!m_hasProject
            ? QStringLiteral("No project loaded") : m_chartReference.isEmpty()
                ? QStringLiteral("Not uploaded") : QStringLiteral("API key required")));
        return;
    }
    m_userStatus->setText(QStringLiteral("Logged in as - Checking..."));
    request(QUrl(QStringLiteral("https://chartbox.app/api/user/me")), "GET", true, {}, [this](QNetworkReply* reply) {
        QString result = requestError(reply);
        if (result.isEmpty()) {
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            const QJsonObject response = document.object();
            if (!document.isObject() || response.isEmpty()) {
                result = QStringLiteral("Unexpected response");
            } else if (response.value(QStringLiteral("success")).isBool()
                && !response.value(QStringLiteral("success")).toBool()) {
                result = QStringLiteral("Authentication failed");
            } else {
                result = userName(response);
            }
        }
        m_userStatus->setText(QStringLiteral("Logged in as - %1").arg(result));
    });
    if (!m_hasProject) {
        m_chartStatus->setText(QStringLiteral("Current chart status - No project loaded"));
        return;
    }
    if (m_chartReference.isEmpty()) {
        m_chartStatus->setText(QStringLiteral("Current chart status - Not uploaded"));
        return;
    }
    m_chartStatus->setText(QStringLiteral("Current chart status - Checking..."));
    request(chartUrl(m_chartReference), "GET", true, {}, [this](QNetworkReply* reply) {
        const ChartLookupResult lookup = chartLookupResult(reply);
        const QString result = !lookup.error.isEmpty() ? lookup.error : lookup.present
            ? QStringLiteral("Present on server") : QStringLiteral("Not present on server");
        setChartPresentOnServer(lookup.present);
        m_chartStatus->setText(QStringLiteral("Current chart status - %1").arg(result));
    });
}

void ChartboxDialog::uploadProject() {
    if (m_pendingRequests != 0 || m_mutationInProgress || !m_prepareUpload || !m_saveReference) {
        return;
    }
    if (m_apiKey->text().trimmed().isEmpty()) {
        m_chartStatus->setText(QStringLiteral("Current chart status - API key required"));
        return;
    }
    m_mutationInProgress = true;
    m_updateExisting = false;
    ++m_refreshGeneration;
    updateControls();
    const QString path = m_prepareUpload();
    if (path.isEmpty()) {
        finishMutation(QStringLiteral("Project preparation cancelled or failed"));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        finishMutation(QStringLiteral("Could not read the saved project"));
        return;
    }
    if (file.size() <= 0 || file.size() > kMaximumUploadBytes) {
        finishMutation(QStringLiteral("The project must be at most 50 MiB"));
        return;
    }
    m_uploadFileName = QFileInfo(path).fileName();
    m_uploadData = file.readAll();
    if (file.error() != QFileDevice::NoError || m_uploadData.size() != file.size()) {
        finishMutation(QStringLiteral("Could not read the complete project"));
        return;
    }
    if (m_chartReference.isEmpty()) {
        sendUpload();
        return;
    }
    m_chartStatus->setText(QStringLiteral("Current chart status - Confirming chart on server..."));
    request(chartUrl(m_chartReference), "GET", true, {}, [this](QNetworkReply* reply) {
        const ChartLookupResult lookup = chartLookupResult(reply);
        if (!lookup.error.isEmpty()) {
            finishMutation(QStringLiteral("Could not confirm chart: %1").arg(lookup.error));
            return;
        }
        m_updateExisting = lookup.present;
        setChartPresentOnServer(lookup.present);
        sendUpload();
    });
}

void ChartboxDialog::sendUpload() {
    QUrl url(QStringLiteral("https://chartbox.app/api/charts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("filename"), m_uploadFileName);
    if (m_updateExisting) {
        query.addQueryItem(QStringLiteral("reference"), m_chartReference);
    } else {
        query.addQueryItem(QStringLiteral("game"), QStringLiteral("infalsus"));
    }
    url.setQuery(query);
    m_chartStatus->setText(m_updateExisting ? QStringLiteral("Current chart status - Updating...")
        : QStringLiteral("Current chart status - Uploading..."));
    request(url, m_updateExisting ? "PATCH" : "PUT", true, m_uploadData, [this](QNetworkReply* reply) {
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        const QString error = responseError(reply, response);
        if (!error.isEmpty()) {
            finishMutation(QStringLiteral("Upload/update failed: %1").arg(error));
            return;
        }
        const QString reference = response.value(QStringLiteral("reference")).toString().trimmed();
        if (!response.value(QStringLiteral("success")).toBool() || reference.isEmpty()) {
            finishMutation(QStringLiteral("The server did not confirm the upload/update"));
            return;
        }
        m_chartReference = reference;
        setChartPresentOnServer(true);
        const bool saved = m_saveReference(reference);
        finishMutation(saved ? QStringLiteral("Present on server; project saved")
            : QStringLiteral("Present on server; Chartbox ID could not be saved. Save the project again."));
    });
}

void ChartboxDialog::deleteChart() {
    if (m_pendingRequests != 0 || m_mutationInProgress || m_chartReference.isEmpty()
        || m_apiKey->text().trimmed().isEmpty() || !m_saveReference) {
        return;
    }
    m_mutationInProgress = true;
    ++m_refreshGeneration;
    updateControls();
    QUrl url(QStringLiteral("https://chartbox.app/api/charts"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("reference"), m_chartReference);
    url.setQuery(query);
    m_chartStatus->setText(QStringLiteral("Current chart status - Deleting..."));
    request(url, "DELETE", true, {}, [this](QNetworkReply* reply) {
        const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
        const QString error = responseError(reply, response);
        if (!error.isEmpty()) {
            finishMutation(QStringLiteral("Delete failed: %1").arg(error));
            return;
        }
        if (!response.value(QStringLiteral("success")).toBool()) {
            finishMutation(QStringLiteral("The server did not confirm deletion"));
            return;
        }
        m_chartReference.clear();
        setChartPresentOnServer(false);
        const bool saved = m_saveReference(QString());
        finishMutation(saved ? QStringLiteral("Deleted from server; project saved")
            : QStringLiteral("Deleted from server; cleared Chartbox ID could not be saved. Save the project again."));
    });
}

void ChartboxDialog::openChart() {
    if (m_pendingRequests != 0 || m_mutationInProgress || m_chartReference.isEmpty()) {
        return;
    }
    QUrl url(QStringLiteral("https://chartbox.app/"));
    url.setPath(QStringLiteral("/charts/%1").arg(m_chartReference));
    if (!QDesktopServices::openUrl(url)) {
        m_chartStatus->setText(QStringLiteral("Current chart status - Could not open the chart in the browser"));
    }
}

void ChartboxDialog::finishMutation(const QString& status) {
    m_mutationInProgress = false;
    m_uploadData.clear();
    m_uploadFileName.clear();
    m_chartStatus->setText(QStringLiteral("Current chart status - %1").arg(status));
    updateControls();
}

} // namespace infalsus::gui
