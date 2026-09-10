#include "UploadManager.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

UploadManager::UploadManager(ApiClient *apiClient, LocalDatabase *database, QObject *parent)
    : QObject(parent), apiClient_(apiClient), database_(database) {
    retryTimer_.setInterval(15000);
    retryTimer_.setParent(this);
    connect(&retryTimer_, &QTimer::timeout, this, &UploadManager::retryFailedUploads);

    connect(apiClient_, &ApiClient::uploadSucceeded, this, &UploadManager::handleUploadSucceeded);
    connect(apiClient_, &ApiClient::uploadFailed, this, &UploadManager::handleUploadFailed);
    connect(apiClient_, &ApiClient::dataStatusesReceived, this, &UploadManager::handleDataStatusesReceived);
}

void UploadManager::startRetryTimer() {
    if (!retryTimer_.isActive()) {
        retryFailedUploads();
        retryTimer_.start();
    }
}

void UploadManager::stopRetryTimer() {
    retryTimer_.stop();
}

void UploadManager::setUploadsEnabled(bool enabled) {
    uploadsEnabled_ = enabled;
}

void UploadManager::submitUpload(const UploadRequest &request, bool manual) {
    if (!uploadsEnabled_) {
        emit logMessage("upload skipped: workstation token is missing");
        return;
    }

    QString message;
    if (!canUpload(request, &message)) {
        QString dbError;
        if (!database_->markUploadFailed(request, message, &dbError)) {
            emit logMessage("mark invalid upload failed: " + dbError);
        }
        emit logMessage(message);
        emit recordsChanged();
        return;
    }

    QString dbError;
    const bool usesUploadPath = !request.uploadPath.trimmed().isEmpty() && request.uploadPath != request.localPath;
    const bool recorded = usesUploadPath
        ? database_->recordAccessDeltaUpload(request, "uploading", &dbError)
        : database_->recordUpload(request, "uploading", &dbError);
    if (!recorded) {
        emit logMessage("record upload failed: " + dbError);
    }
    emit recordsChanged();
    emit logMessage(QString("%1uploading %2 deviceId=%3")
                        .arg(manual ? "manual " : "")
                        .arg(request.fileName)
                        .arg(request.deviceId));
    apiClient_->uploadFile(request);
}

void UploadManager::retryFailedUploads() {
    if (!uploadsEnabled_) {
        return;
    }

    reconcilePendingParseUploads();

    QString dbError;
    const QVector<StoredUpload> uploads = database_->retryableUploads(5, &dbError);
    if (!dbError.isEmpty()) {
        emit logMessage("load retry uploads failed: " + dbError);
        return;
    }

    for (const StoredUpload &upload : uploads) {
        if (upload.retryCount >= maxRetries_) {
            emit logMessage(QString("retry limit reached for %1 retry=%2").arg(upload.request.fileName).arg(upload.retryCount));
            continue;
        }
        emit logMessage(QString("retrying upload %1 retry=%2").arg(upload.request.fileName).arg(upload.retryCount));
        submitUpload(upload.request);
    }
}

void UploadManager::reconcilePendingParseUploads() {
    QString dbError;
    const QVector<StoredUpload> uploads = database_->pendingParseUploads(50, &dbError);
    if (!dbError.isEmpty()) {
        emit logMessage("load pending parse uploads failed: " + dbError);
        return;
    }
    if (uploads.isEmpty()) {
        return;
    }

    QStringList dataNos;
    for (const StoredUpload &upload : uploads) {
        const QString dataNo = upload.dataNo.trimmed();
        if (!dataNo.isEmpty()) {
            dataNos.append(dataNo);
        }
    }
    apiClient_->queryDataStatuses(dataNos);
}

void UploadManager::handleUploadSucceeded(const UploadRequest &request, const QJsonObject &payload) {
    QString dbError;
    const QString dataNo = payload.value("dataNo").toString();
    if (!database_->markUploaded(request, dataNo, &dbError)) {
        emit logMessage("mark upload success failed: " + dbError);
    }
    const QString status = payload.value("status").toString().trimmed().toLower();
    if (status == "success" || status == "failed") {
        dbError.clear();
        if (!database_->markTaskResult(payload, &dbError)) {
            emit logMessage("mark upload task result failed: " + dbError);
        } else {
            emit taskResultReady(payload);
        }
    }
    emit logMessage("upload ok: " + request.fileName);
    emit recordsChanged();
}

void UploadManager::handleUploadFailed(const UploadRequest &request, const QString &message) {
    QString dbError;
    if (!database_->markUploadFailed(request, message, &dbError)) {
        emit logMessage("mark upload failed failed: " + dbError);
    }
    emit recordsChanged();
}

void UploadManager::handleDataStatusesReceived(const QJsonArray &statuses) {
    bool changed = false;
    for (const QJsonValue &value : statuses) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject status = value.toObject();
        const QString remoteStatus = status.value("status").toString().trimmed().toLower();
        if (remoteStatus != "success" && remoteStatus != "failed") {
            continue;
        }

        QString dbError;
        if (!database_->markTaskResult(status, &dbError)) {
            emit logMessage("mark reconciled task result failed: " + dbError);
            continue;
        }
        changed = true;
        emit logMessage("parse status reconciled: " + status.value("dataNo").toString());
        emit taskResultReady(status);
    }
    if (changed) {
        emit recordsChanged();
    }
}

bool UploadManager::canUpload(const UploadRequest &request, QString *message) const {
    if (request.deviceId <= 0) {
        *message = "deviceId is required before upload";
        return false;
    }
    if (request.localPath.trimmed().isEmpty()) {
        *message = "local path is empty";
        return false;
    }
    const QString uploadPath = request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath;
    QFileInfo info(uploadPath);
    if (!info.exists() || !info.isFile()) {
        *message = "local file is missing: " + uploadPath;
        return false;
    }
    if (request.fileHash.trimmed().isEmpty()) {
        *message = "file hash is empty: " + request.localPath;
        return false;
    }
    return true;
}
