#include "AccessDeltaCapture.h"

#include "system/FileHasher.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QSqlError>
#include <QSqlField>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QUuid>

AccessDeltaCapture::AccessDeltaCapture(LocalDatabase *database, QObject *parent)
    : QObject(parent), database_(database) {}

void AccessDeltaCapture::captureFile(const DeviceConfig &device, const QString &path) {
    if (database_ == nullptr) {
        emit logMessage("access capture skipped: local database is unavailable");
        return;
    }

    QString errorMessage;
    const QString snapshotPath = snapshotFile(path, &errorMessage);
    if (!errorMessage.isEmpty()) {
        emit logMessage("access snapshot failed: " + errorMessage);
        return;
    }

    const QVector<TableDelta> deltas = readDeltas(device, path, snapshotPath, &errorMessage);
    QFile::remove(snapshotPath);
    if (!errorMessage.isEmpty()) {
        emit logMessage("access capture failed: " + errorMessage);
        return;
    }
    if (deltas.isEmpty()) {
        emit logMessage("access capture found no new rows: " + QFileInfo(path).fileName());
        return;
    }

    const QString deltaPath = writeDeltaJson(device, path, deltas, &errorMessage);
    if (!errorMessage.isEmpty()) {
        emit logMessage("access delta write failed: " + errorMessage);
        return;
    }

    for (const TableDelta &delta : deltas) {
        const QString monitorColumn = delta.rule.monitorColumns.first();
        if (!database_->markAccessBatchPending(
                deltaPath,
                device.deviceId,
                QFileInfo(path).absoluteFilePath(),
                delta.rule.tableName,
                monitorColumn,
                delta.cursorFrom,
                delta.cursorTo,
                &errorMessage)) {
            emit logMessage("access batch state failed: " + errorMessage);
            return;
        }
    }

    QString hashError;
    const QString hash = FileHasher::sha256(deltaPath, &hashError);
    if (!hashError.isEmpty()) {
        emit logMessage("access delta hash failed: " + hashError);
        return;
    }

    QFileInfo deltaInfo(deltaPath);
    UploadRequest request;
    request.deviceId = device.deviceId;
    request.localPath = deltaInfo.absoluteFilePath();
    request.fileName = deltaInfo.fileName();
    request.fileSize = deltaInfo.size();
    request.fileMtime = deltaInfo.lastModified();
    request.fileHash = hash;
    emit logMessage(QString("access delta ready: %1 table(s) file=%2").arg(deltas.size()).arg(request.fileName));
    emit uploadReady(request);
}

QString AccessDeltaCapture::snapshotFile(const QString &sourcePath, QString *errorMessage) const {
    const QFileInfo sourceInfo(sourcePath);
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/access-snapshots";
    if (!QDir().mkpath(root)) {
        *errorMessage = "cannot create snapshot directory: " + root;
        return {};
    }
    const QString snapshotPath = QString("%1/%2_%3.%4")
                                     .arg(root, sourceInfo.completeBaseName(), QUuid::createUuid().toString(QUuid::WithoutBraces), sourceInfo.suffix());
    QFile::remove(snapshotPath);
    if (!QFile::copy(sourceInfo.absoluteFilePath(), snapshotPath)) {
        *errorMessage = "cannot copy Access file: " + sourceInfo.absoluteFilePath();
        return {};
    }
    return snapshotPath;
}

QVector<AccessDeltaCapture::TableDelta> AccessDeltaCapture::readDeltas(const DeviceConfig &device, const QString &sourcePath, const QString &snapshotPath, QString *errorMessage) {
    if (!QSqlDatabase::isDriverAvailable("QODBC")) {
        *errorMessage = "QODBC driver is unavailable; install Qt ODBC plugin and Microsoft Access ODBC/ACE driver";
        return {};
    }

    const QString connectionName = "access_capture_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase accessDb;
    if (!openAccessDatabase(snapshotPath, connectionName, &accessDb, errorMessage)) {
        QSqlDatabase::removeDatabase(connectionName);
        return {};
    }

    QVector<TableDelta> deltas;
    for (const AccessRuleConfig &rule : device.accessRules) {
        TableDelta delta = readTableDelta(accessDb, device, sourcePath, rule, errorMessage);
        if (!errorMessage->isEmpty()) {
            accessDb.close();
            accessDb = QSqlDatabase();
            QSqlDatabase::removeDatabase(connectionName);
            return {};
        }
        if (!delta.rows.isEmpty()) {
            deltas.append(delta);
        }
    }

    accessDb.close();
    accessDb = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return deltas;
}

bool AccessDeltaCapture::openAccessDatabase(const QString &snapshotPath, const QString &connectionName, QSqlDatabase *database, QString *errorMessage) const {
    QSqlDatabase db = QSqlDatabase::addDatabase("QODBC", connectionName);
    db.setDatabaseName(QString("DRIVER={Microsoft Access Driver (*.mdb, *.accdb)};DBQ=%1;READONLY=TRUE;").arg(QDir::toNativeSeparators(snapshotPath)));
    if (!db.open()) {
        *errorMessage = db.lastError().text();
        return false;
    }
    *database = db;
    return true;
}

QJsonArray AccessDeltaCapture::readSchema(QSqlDatabase &database, const QString &tableName, QString *errorMessage) const {
    QSqlQuery query(database);
    if (!query.exec(QString("SELECT * FROM %1 WHERE 1=0").arg(quoteIdentifier(tableName)))) {
        *errorMessage = QString("read schema failed table=%1 error=%2").arg(tableName, query.lastError().text());
        return {};
    }

    const QSqlRecord record = query.record();
    QJsonArray schema;
    for (int index = 0; index < record.count(); ++index) {
        const QSqlField field = record.field(index);
        QJsonObject item;
        item["name"] = field.name();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        item["type"] = QString::fromLatin1(field.metaType().name());
#else
        item["type"] = QString::fromLatin1(QMetaType::typeName(field.type()));
#endif
        item["ordinal"] = index;
        schema.append(item);
    }
    if (schema.isEmpty()) {
        *errorMessage = "table has no readable columns: " + tableName;
    }
    return schema;
}

AccessDeltaCapture::TableDelta AccessDeltaCapture::readTableDelta(QSqlDatabase &database, const DeviceConfig &device, const QString &sourcePath, const AccessRuleConfig &rule, QString *errorMessage) {
    TableDelta delta;
    delta.rule = rule;
    delta.schema = readSchema(database, rule.tableName, errorMessage);
    if (!errorMessage->isEmpty()) {
        return delta;
    }
    if (!validateRuleAgainstSchema(rule, delta.schema, errorMessage)) {
        return delta;
    }
    const QByteArray schemaBytes = QJsonDocument(delta.schema).toJson(QJsonDocument::Compact);
    const QString schemaHash = "sha256:" + QString::fromLatin1(QCryptographicHash::hash(schemaBytes, QCryptographicHash::Sha256).toHex());
    delta.schemaHash = schemaHash;
    if (!database_->saveAccessSchemaCache(device.deviceId, QFileInfo(sourcePath).absoluteFilePath(), rule.tableName, schemaHash, QString::fromUtf8(schemaBytes), errorMessage)) {
        return delta;
    }

    const QString monitorColumn = rule.monitorColumns.first();
    AccessCursorState cursor;
    if (!database_->accessCursor(device.deviceId, QFileInfo(sourcePath).absoluteFilePath(), rule.tableName, monitorColumn, &cursor, errorMessage)) {
        return delta;
    }
    delta.cursorFrom = cursor.lastCursorValue;

    if (!cursor.hasLastCursor && device.accessFirstRunPolicy == "start_from_latest") {
        const QString latestCursor = readMaxCursor(database, rule.tableName, monitorColumn, errorMessage);
        if (!errorMessage->isEmpty()) {
            return delta;
        }
        if (!latestCursor.isEmpty() && !database_->setAccessCursor(device.deviceId, QFileInfo(sourcePath).absoluteFilePath(), rule.tableName, monitorColumn, latestCursor, errorMessage)) {
            return delta;
        }
        emit logMessage(QString("access first run initialized cursor table=%1 column=%2 value=%3").arg(rule.tableName, monitorColumn, latestCursor));
        return delta;
    }

    QString sql = QString("SELECT TOP %1 * FROM %2").arg(rule.maxRows).arg(quoteIdentifier(rule.tableName));
    if (cursor.hasLastCursor) {
        sql += QString(" WHERE %1 > ?").arg(quoteIdentifier(monitorColumn));
    }
    sql += QString(" ORDER BY %1").arg(quoteIdentifier(monitorColumn));

    QSqlQuery query(database);
    query.prepare(sql);
    if (cursor.hasLastCursor) {
        query.addBindValue(cursor.lastCursorValue);
    }
    if (!query.exec()) {
        *errorMessage = QString("read rows failed table=%1 error=%2").arg(rule.tableName, query.lastError().text());
        return delta;
    }

    const QSqlRecord record = query.record();
    const int cursorIndex = record.indexOf(monitorColumn);
    while (query.next()) {
        QJsonObject row;
        for (int index = 0; index < record.count(); ++index) {
            row[record.fieldName(index)] = toJsonValue(query.value(index));
        }
        delta.rows.append(row);
        delta.cursorTo = cursorToString(query.value(cursorIndex));
    }
    delta.rawRowCount = delta.rows.size();
    if (rule.sampling.enabled) {
        delta.rows = applyLastSampling(rule, delta.rows, errorMessage);
        if (!errorMessage->isEmpty()) {
            return delta;
        }
    }
    return delta;
}

QString AccessDeltaCapture::readMaxCursor(QSqlDatabase &database, const QString &tableName, const QString &monitorColumn, QString *errorMessage) const {
    QSqlQuery query(database);
    const QString sql = QString("SELECT MAX(%1) FROM %2").arg(quoteIdentifier(monitorColumn), quoteIdentifier(tableName));
    if (!query.exec(sql)) {
        *errorMessage = QString("read max cursor failed table=%1 error=%2").arg(tableName, query.lastError().text());
        return {};
    }
    if (!query.next()) {
        return {};
    }
    return cursorToString(query.value(0));
}

QString AccessDeltaCapture::writeDeltaJson(const DeviceConfig &device, const QString &sourcePath, const QVector<TableDelta> &deltas, QString *errorMessage) {
    const QFileInfo sourceInfo(sourcePath);
    QString hashError;
    const QString sourceHash = FileHasher::sha256(sourcePath, &hashError);
    if (!hashError.isEmpty()) {
        *errorMessage = hashError;
        return {};
    }

    QJsonObject root;
    root["kind"] = "access_delta";
    root["schemaVersion"] = "1.0";
    root["captureTime"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    root["sourceFile"] = sourceInfo.fileName();
    root["sourcePath"] = sourceInfo.absoluteFilePath();
    root["sourceFileSize"] = sourceInfo.size();
    root["sourceFileMtime"] = sourceInfo.lastModified().toUTC().toString(Qt::ISODateWithMs);
    root["sourceFileHash"] = sourceHash;
    root["accessMode"] = device.accessMode;

    QJsonArray tables;
    for (const TableDelta &delta : deltas) {
        QJsonObject table;
        table["tableName"] = delta.rule.tableName;
        table["rowCount"] = delta.rows.size();
        table["rawRowCount"] = delta.rawRowCount;
        QJsonArray monitorColumns;
        for (const QString &column : delta.rule.monitorColumns) {
            monitorColumns.append(column);
        }
        table["monitorColumns"] = monitorColumns;
        table["cursorFrom"] = delta.cursorFrom;
        table["cursorTo"] = delta.cursorTo;
        table["schemaHash"] = delta.schemaHash;
        table["schema"] = delta.schema;
        if (delta.rule.sampling.enabled) {
            table["sampling"] = delta.rule.sampling.toJson();
        }
        table["rows"] = delta.rows;
        tables.append(table);
    }
    root["tables"] = tables;

    const QString outDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/access-delta";
    if (!QDir().mkpath(outDir)) {
        *errorMessage = "cannot create access delta directory: " + outDir;
        return {};
    }
    const QString fileName = QString("%1_%2.access_delta.json")
                             .arg(sourceInfo.completeBaseName(), QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmsszzz"));
    const QString outPath = outDir + "/" + fileName;
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = "cannot write access delta json: " + outPath;
        return {};
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    return outPath;
}

bool AccessDeltaCapture::validateRuleAgainstSchema(const AccessRuleConfig &rule, const QJsonArray &schema, QString *errorMessage) const {
    QStringList columns;
    for (const QJsonValue &value : schema) {
        columns.append(value.toObject().value("name").toString());
    }
    for (const QString &monitorColumn : rule.monitorColumns) {
        if (!columns.contains(monitorColumn, Qt::CaseInsensitive)) {
            *errorMessage = QString("monitor column not found table=%1 column=%2").arg(rule.tableName, monitorColumn);
            return false;
        }
    }
    if (rule.sampling.enabled && !columns.contains(rule.sampling.timeColumn, Qt::CaseInsensitive)) {
        *errorMessage = QString("sampling time column not found table=%1 column=%2").arg(rule.tableName, rule.sampling.timeColumn);
        return false;
    }
    return true;
}

QJsonArray AccessDeltaCapture::applyLastSampling(const AccessRuleConfig &rule, const QJsonArray &rows, QString *errorMessage) const {
    QJsonArray sampled;
    if (rows.isEmpty()) {
        return sampled;
    }
    if (rule.sampling.intervalHours <= 0 || rule.sampling.strategy != "last") {
        *errorMessage = QString("invalid sampling config table=%1").arg(rule.tableName);
        return {};
    }

    QMap<qint64, QJsonObject> latestByBucket;
    QMap<qint64, QDateTime> latestTimeByBucket;
    const qint64 bucketSeconds = static_cast<qint64>(rule.sampling.intervalHours) * 3600;

    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const QDateTime time = parseDateTimeValue(row.value(rule.sampling.timeColumn));
        if (!time.isValid()) {
            *errorMessage = QString("sampling time parse failed table=%1 column=%2 value=%3")
                                .arg(rule.tableName, rule.sampling.timeColumn, row.value(rule.sampling.timeColumn).toVariant().toString());
            return {};
        }
        const qint64 bucket = time.toSecsSinceEpoch() / bucketSeconds;
        if (!latestByBucket.contains(bucket) || time >= latestTimeByBucket.value(bucket)) {
            latestByBucket[bucket] = row;
            latestTimeByBucket[bucket] = time;
        }
    }

    for (auto iterator = latestByBucket.constBegin(); iterator != latestByBucket.constEnd(); ++iterator) {
        sampled.append(iterator.value());
    }
    return sampled;
}

QString AccessDeltaCapture::quoteIdentifier(const QString &identifier) {
    QString escaped = identifier;
    escaped.replace("]", "]]");
    return "[" + escaped + "]";
}

QJsonValue AccessDeltaCapture::toJsonValue(const QVariant &value) {
    if (value.isNull()) {
        return QJsonValue();
    }
    if (value.canConvert<QDateTime>()) {
        const QDateTime dateTime = value.toDateTime();
        if (dateTime.isValid()) {
            return dateTime.toString(Qt::ISODateWithMs);
        }
    }
    const int typeId =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        value.typeId();
#else
        static_cast<int>(value.type());
#endif
    switch (typeId) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
        return value.toInt();
    case QMetaType::UInt:
        return static_cast<double>(value.toUInt());
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return static_cast<double>(value.toLongLong());
    case QMetaType::Double:
        return value.toDouble();
    default:
        return value.toString();
    }
}

QString AccessDeltaCapture::cursorToString(const QVariant &value) {
    if (value.isNull()) {
        return {};
    }
    if (value.canConvert<QDateTime>()) {
        const QDateTime dateTime = value.toDateTime();
        if (dateTime.isValid()) {
            return dateTime.toString(Qt::ISODateWithMs);
        }
    }
    return value.toString();
}

QDateTime AccessDeltaCapture::parseDateTimeValue(const QJsonValue &value) {
    if (value.isNull() || value.isUndefined()) {
        return {};
    }

    const QString text = value.toString().trimmed();
    if (text.isEmpty()) {
        return {};
    }

    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (parsed.isValid()) {
        return parsed;
    }
    parsed = QDateTime::fromString(text, Qt::ISODate);
    if (parsed.isValid()) {
        return parsed;
    }

    const QStringList formats = {
        "yyyy/M/d H:m:s",
        "yyyy/M/d HH:mm:ss",
        "yyyy/MM/dd HH:mm:ss",
        "yyyy-MM-dd HH:mm:ss",
        "M/d/yy H:m:s",
        "MM/dd/yy HH:mm:ss"
    };
    for (const QString &format : formats) {
        parsed = QDateTime::fromString(text, format);
        if (parsed.isValid()) {
            return parsed;
        }
    }
    return {};
}
