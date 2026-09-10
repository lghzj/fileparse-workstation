#pragma once

#include "api/ApiClient.h"
#include "storage/LocalDatabase.h"

#include <QJsonArray>
#include <QObject>
#include <QTimer>

class UploadManager final : public QObject {
    Q_OBJECT

public:
    UploadManager(ApiClient *apiClient, LocalDatabase *database, QObject *parent = nullptr);

    void startRetryTimer();
    void stopRetryTimer();
    void setUploadsEnabled(bool enabled);
    void submitUpload(const UploadRequest &request, bool manual = false);
    void retryFailedUploads();
    void reconcilePendingParseUploads();

signals:
    void logMessage(const QString &message);
    void recordsChanged();
    void taskResultReady(const QJsonObject &payload);

private:
    void handleUploadSucceeded(const UploadRequest &request, const QJsonObject &payload);
    void handleUploadFailed(const UploadRequest &request, const QString &message);
    void handleDataStatusesReceived(const QJsonArray &statuses);
    bool canUpload(const UploadRequest &request, QString *message) const;

    ApiClient *apiClient_ = nullptr;
    LocalDatabase *database_ = nullptr;
    QTimer retryTimer_;
    int maxRetries_ = 10;
    bool uploadsEnabled_ = false;
};
