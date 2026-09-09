#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"
#include "storage/LocalDatabase.h"

#include <QObject>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>
#include <QVector>

class AccessDeltaCapture final : public QObject {
    Q_OBJECT

public:
    explicit AccessDeltaCapture(QObject *parent = nullptr);

public slots:
    void captureFile(const DeviceConfig &device, const QString &path);

signals:
    void conversionStarted(const UploadRequest &request);
    void conversionFailed(const UploadRequest &request, const QString &message);
    void conversionSkipped(const UploadRequest &request, const QString &message);
    void uploadReady(const UploadRequest &request);
    void logMessage(const QString &message);

private:
    struct TableDelta {
        AccessRuleConfig rule;
        QString cursorFrom;
        QString cursorTo;
        QString schemaHash;
        QJsonArray schema;
        QJsonArray rows;
        int rawRowCount = 0;
    };

    QString snapshotFile(const QString &sourcePath, QString *errorMessage) const;
    QVector<TableDelta> readDeltas(const DeviceConfig &device, const QString &sourcePath, const QString &snapshotPath, QString *errorMessage);
    bool openAccessDatabase(const QString &snapshotPath, const QString &connectionName, QSqlDatabase *database, QString *errorMessage) const;
    QJsonArray readSchema(QSqlDatabase &database, const QString &tableName, QString *errorMessage) const;
    TableDelta readTableDelta(QSqlDatabase &database, const DeviceConfig &device, const QString &sourcePath, const AccessRuleConfig &rule, QString *errorMessage);
    QString readMaxCursor(QSqlDatabase &database, const QString &tableName, const QString &monitorColumn, QString *errorMessage) const;
    QString writeDeltaJson(const DeviceConfig &device, const QString &sourcePath, const QVector<TableDelta> &deltas, QString *errorMessage);
    bool validateRuleAgainstSchema(const AccessRuleConfig &rule, const QJsonArray &schema, QString *errorMessage) const;
    QJsonArray applyLastSampling(const AccessRuleConfig &rule, const QJsonArray &rows, QString *errorMessage) const;
    static QString quoteIdentifier(const QString &identifier);
    static QJsonValue toJsonValue(const QVariant &value);
    static QString cursorToString(const QVariant &value);
    static QDateTime parseDateTimeValue(const QJsonValue &value);

    LocalDatabase database_;
};
