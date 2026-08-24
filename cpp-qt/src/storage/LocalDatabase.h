#pragma once

#include "api/ApiClient.h"

#include <QSqlDatabase>
#include <QVector>
#include <QJsonObject>
#include <QString>

struct StoredUpload {
    UploadRequest request;
    QString dataNo;
    int retryCount = 0;
    QString status;
    QString lastErrorMessage;
    QString updatedAt;
};

class LocalDatabase final {
public:
    bool open(QString *errorMessage);
    bool recordUpload(const UploadRequest &request, const QString &status, QString *errorMessage);
    bool markUploaded(const UploadRequest &request, const QString &dataNo, QString *errorMessage);
    bool markUploadFailed(const UploadRequest &request, const QString &message, QString *errorMessage);
    bool markTaskResult(const QJsonObject &payload, QString *errorMessage);
    int recoverInterruptedUploads(QString *errorMessage);
    QVector<StoredUpload> retryableUploads(int limit, QString *errorMessage);
    QVector<StoredUpload> recentUploads(int limit, QString *errorMessage);
    int clearFailedUploads(QString *errorMessage);

private:
    bool ensureColumn(const QString &name, const QString &definition, QString *errorMessage);
    QVector<StoredUpload> queryUploads(const QString &whereClause, const QString &orderBy, int limit, QString *errorMessage);
    static void bindRequest(QSqlQuery *query, const UploadRequest &request);

    QSqlDatabase db_;
};
