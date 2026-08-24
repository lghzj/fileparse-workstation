#pragma once

#include "api/ApiClient.h"
#include "storage/LocalDatabase.h"

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

signals:
    void logMessage(const QString &message);
    void recordsChanged();

private:
    void handleUploadSucceeded(const UploadRequest &request, const QJsonObject &payload);
    void handleUploadFailed(const UploadRequest &request, const QString &message);
    bool canUpload(const UploadRequest &request, QString *message) const;

    ApiClient *apiClient_ = nullptr;
    LocalDatabase *database_ = nullptr;
    QTimer retryTimer_;
    int maxRetries_ = 10;
    bool uploadsEnabled_ = false;
};
