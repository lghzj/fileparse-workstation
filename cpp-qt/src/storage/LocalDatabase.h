#pragma once

#include "api/ApiClient.h"

#include <QSqlDatabase>
#include <QVector>
#include <QJsonObject>
#include <QString>
#include <QVariantList>

struct AccessCursorState {
    bool hasLastCursor = false;
    QString lastCursorValue;
    QString pendingCursorValue;
    QString pendingDataNo;
    QString status;
};

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
    explicit LocalDatabase(const QString &connectionName = QString());
    ~LocalDatabase();

    bool open(QString *errorMessage);
    bool recordUpload(const UploadRequest &request, const QString &status, QString *errorMessage);
    bool recordAccessConverting(const UploadRequest &request, QString *errorMessage);
    bool recordAccessDeltaUpload(const UploadRequest &request, const QString &status, QString *errorMessage);
    bool markUploaded(const UploadRequest &request, const QString &dataNo, QString *errorMessage);
    bool markUploadFailed(const UploadRequest &request, const QString &message, QString *errorMessage);
    bool markTaskResult(const QJsonObject &payload, QString *errorMessage);
    bool accessCursor(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, AccessCursorState *state, QString *errorMessage);
    bool saveAccessSchemaCache(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &schemaHash, const QString &columnsJson, QString *errorMessage);
    bool setAccessCursor(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, const QString &cursorValue, QString *errorMessage);
    bool markAccessBatchPending(const QString &deltaPath, int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, const QString &cursorFrom, const QString &cursorTo, QString *errorMessage);
    bool markAccessBatchUploaded(const QString &deltaPath, const QString &dataNo, QString *errorMessage);
    int recoverInterruptedUploads(QString *errorMessage);
    QVector<StoredUpload> retryableUploads(int limit, QString *errorMessage);
    QVector<StoredUpload> recentUploads(int limit, QString *errorMessage);
    bool uploadByDataNo(const QString &dataNo, StoredUpload *upload, QString *errorMessage);
    int clearFailedUploads(QString *errorMessage);

private:
    bool ensureColumn(const QString &name, const QString &definition, QString *errorMessage);
    QVector<StoredUpload> queryUploads(const QString &whereClause, const QString &orderBy, int limit, QString *errorMessage, const QVariantList &whereValues = {});
    static void bindRequest(QSqlQuery *query, const UploadRequest &request);

    QSqlDatabase db_;
    QString connectionName_;
};
