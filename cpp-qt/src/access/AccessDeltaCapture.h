#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"
#include "storage/LocalDatabase.h"

#include <QObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>
#include <QVector>

class AccessDeltaCapture final : public QObject {
    Q_OBJECT

public:
    explicit AccessDeltaCapture(LocalDatabase *database, QObject *parent = nullptr);

public slots:
    void captureFile(const DeviceConfig &device, const QString &path);

signals:
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
    };

    QString snapshotFile(const QString &sourcePath, QString *errorMessage) const;
    QVector<TableDelta> readDeltas(const DeviceConfig &device, const QString &sourcePath, const QString &snapshotPath, QString *errorMessage);
    bool openAccessDatabase(const QString &snapshotPath, const QString &connectionName, QSqlDatabase *database, QString *errorMessage) const;
    QJsonArray readSchema(QSqlDatabase &database, const QString &tableName, QString *errorMessage) const;
    TableDelta readTableDelta(QSqlDatabase &database, const DeviceConfig &device, const QString &sourcePath, const AccessRuleConfig &rule, QString *errorMessage);
    QString readMaxCursor(QSqlDatabase &database, const QString &tableName, const QString &monitorColumn, QString *errorMessage) const;
    QString writeDeltaJson(const DeviceConfig &device, const QString &sourcePath, const QVector<TableDelta> &deltas, QString *errorMessage);
    bool validateRuleAgainstSchema(const AccessRuleConfig &rule, const QJsonArray &schema, QString *errorMessage) const;
    static QString quoteIdentifier(const QString &identifier);
    static QJsonValue toJsonValue(const QVariant &value);
    static QString cursorToString(const QVariant &value);

    LocalDatabase *database_ = nullptr;
};
