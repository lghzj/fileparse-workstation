#include "UploadManager.h"

#include <QFileInfo>

UploadManager::UploadManager(ApiClient *apiClient, LocalDatabase *database, QObject *parent)
    : QObject(parent), apiClient_(apiClient), database_(database) {
    retryTimer_.setInterval(15000);
    retryTimer_.setParent(this);
    connect(&retryTimer_, &QTimer::timeout, this, &UploadManager::retryFailedUploads);

    connect(apiClient_, &ApiClient::uploadSucceeded, this, &UploadManager::handleUploadSucceeded);
    connect(apiClient_, &ApiClient::uploadFailed, this, &UploadManager::handleUploadFailed);
}

void UploadManager::startRetryTimer() {
    if (!retryTimer_.isActive()) {
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
    if (!database_->recordUpload(request, "uploading", &dbError)) {
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

void UploadManager::handleUploadSucceeded(const UploadRequest &request, const QJsonObject &payload) {
    QString dbError;
    const QString dataNo = payload.value("dataNo").toString();
    if (!database_->markUploaded(request, dataNo, &dbError)) {
        emit logMessage("mark upload success failed: " + dbError);
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

bool UploadManager::canUpload(const UploadRequest &request, QString *message) const {
    if (request.deviceId <= 0) {
        *message = "deviceId is required before upload";
        return false;
    }
    if (request.localPath.trimmed().isEmpty()) {
        *message = "local path is empty";
        return false;
    }
    QFileInfo info(request.localPath);
    if (!info.exists() || !info.isFile()) {
        *message = "local file is missing: " + request.localPath;
        return false;
    }
    if (request.fileHash.trimmed().isEmpty()) {
        *message = "file hash is empty: " + request.localPath;
        return false;
    }
    return true;
}
