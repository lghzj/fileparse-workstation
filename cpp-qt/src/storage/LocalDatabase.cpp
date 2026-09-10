#include "LocalDatabase.h"

#include <QDir>
#include <QStandardPaths>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>

LocalDatabase::LocalDatabase(const QString &connectionName)
    : connectionName_(connectionName.trimmed().isEmpty()
          ? "workstation_sqlite_" + QUuid::createUuid().toString(QUuid::WithoutBraces)
          : connectionName) {}

LocalDatabase::~LocalDatabase() {
    close();
}

void LocalDatabase::close() {
    const QString name = db_.connectionName();
    if (!name.isEmpty()) {
        db_.close();
        db_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

QString LocalDatabase::databasePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/workstation.db";
}

bool LocalDatabase::open(QString *errorMessage) {
    if (db_.isOpen()) {
        return true;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dir)) {
        *errorMessage = QString("cannot create app data directory: %1").arg(dir);
        return false;
    }

    db_ = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    db_.setDatabaseName(databasePath());
    if (!db_.open()) {
        *errorMessage = db_.lastError().text();
        return false;
    }

    QSqlQuery query(db_);
    if (!query.exec("CREATE TABLE IF NOT EXISTS upload_records ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "device_id INTEGER NOT NULL,"
                    "local_path TEXT NOT NULL,"
                    "file_name TEXT NOT NULL,"
                    "upload_path TEXT,"
                    "file_size INTEGER NOT NULL,"
                    "file_mtime TEXT NOT NULL,"
                    "file_hash TEXT NOT NULL,"
                    "status TEXT NOT NULL,"
                    "data_no TEXT,"
                    "retry_count INTEGER NOT NULL DEFAULT 0,"
                    "last_error_message TEXT,"
                    "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                    ")")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (!ensureColumn("data_no", "ALTER TABLE upload_records ADD COLUMN data_no TEXT", errorMessage)) return false;
    if (!ensureColumn("upload_path", "ALTER TABLE upload_records ADD COLUMN upload_path TEXT", errorMessage)) return false;
    if (!ensureColumn("retry_count", "ALTER TABLE upload_records ADD COLUMN retry_count INTEGER NOT NULL DEFAULT 0", errorMessage)) return false;
    if (!ensureColumn("last_error_message", "ALTER TABLE upload_records ADD COLUMN last_error_message TEXT", errorMessage)) return false;

    if (!query.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_upload_records_file "
                    "ON upload_records(device_id, local_path, file_size, file_mtime)")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS access_schema_cache ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "device_id INTEGER NOT NULL,"
                    "access_file_path TEXT NOT NULL,"
                    "table_name TEXT NOT NULL,"
                    "schema_hash TEXT NOT NULL,"
                    "columns_json TEXT NOT NULL,"
                    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "UNIQUE(device_id, access_file_path, table_name)"
                    ")")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS access_table_cursors ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "device_id INTEGER NOT NULL,"
                    "access_file_path TEXT NOT NULL,"
                    "table_name TEXT NOT NULL,"
                    "monitor_column TEXT NOT NULL,"
                    "last_cursor_value TEXT,"
                    "pending_cursor_value TEXT,"
                    "pending_data_no TEXT,"
                    "status TEXT NOT NULL DEFAULT 'idle',"
                    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "UNIQUE(device_id, access_file_path, table_name, monitor_column)"
                    ")")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS access_capture_batches ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "delta_path TEXT NOT NULL,"
                    "device_id INTEGER NOT NULL,"
                    "access_file_path TEXT NOT NULL,"
                    "table_name TEXT NOT NULL,"
                    "monitor_column TEXT NOT NULL,"
                    "cursor_from TEXT,"
                    "cursor_to TEXT NOT NULL,"
                    "data_no TEXT,"
                    "status TEXT NOT NULL DEFAULT 'pending_upload',"
                    "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                    "UNIQUE(delta_path, table_name, monitor_column)"
                    ")")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::recordUpload(const UploadRequest &request, const QString &status, QString *errorMessage) {
    if (status != "converting") {
        QSqlQuery updateConverting(db_);
        updateConverting.prepare("UPDATE upload_records SET file_name=?, upload_path=?, file_size=?, file_mtime=?, file_hash=COALESCE(NULLIF(?, ''), file_hash), "
                                 "status=?, data_no=NULL, last_error_message=NULL, updated_at=CURRENT_TIMESTAMP "
                                 "WHERE device_id=? AND local_path=? AND status='converting'");
        updateConverting.addBindValue(request.fileName);
        updateConverting.addBindValue(request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath);
        updateConverting.addBindValue(request.fileSize);
        updateConverting.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
        updateConverting.addBindValue(request.fileHash);
        updateConverting.addBindValue(status);
        updateConverting.addBindValue(request.deviceId);
        updateConverting.addBindValue(request.localPath);
        if (!updateConverting.exec()) {
            *errorMessage = updateConverting.lastError().text();
            return false;
        }
        if (updateConverting.numRowsAffected() > 0) {
            return true;
        }
    }

    QSqlQuery query(db_);
    query.prepare("INSERT INTO upload_records "
                  "(device_id, local_path, file_name, upload_path, file_size, file_mtime, file_hash, status, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, local_path, file_size, file_mtime) "
                  "DO UPDATE SET status=excluded.status, file_name=excluded.file_name, upload_path=excluded.upload_path, file_hash=excluded.file_hash, "
                  "last_error_message=NULL, updated_at=CURRENT_TIMESTAMP");
    bindRequest(&query, request);
    query.addBindValue(status);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::recordAccessConverting(const UploadRequest &request, QString *errorMessage) {
    return recordUpload(request, "converting", errorMessage);
}

bool LocalDatabase::recordAccessDeltaUpload(const UploadRequest &request, const QString &status, QString *errorMessage) {
    QSqlQuery update(db_);
    update.prepare("UPDATE upload_records SET file_name=?, upload_path=?, file_size=?, file_mtime=?, file_hash=?, "
                   "status=?, data_no=NULL, last_error_message=NULL, updated_at=CURRENT_TIMESTAMP "
                   "WHERE device_id=? AND local_path=? AND status='converting'");
    update.addBindValue(request.fileName);
    update.addBindValue(request.uploadPath);
    update.addBindValue(request.fileSize);
    update.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    update.addBindValue(request.fileHash);
    update.addBindValue(status);
    update.addBindValue(request.deviceId);
    update.addBindValue(request.localPath);
    if (!update.exec()) {
        *errorMessage = update.lastError().text();
        return false;
    }
    if (update.numRowsAffected() > 0) {
        return true;
    }
    return recordUpload(request, status, errorMessage);
}

bool LocalDatabase::markUploaded(const UploadRequest &request, const QString &dataNo, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("UPDATE upload_records SET status='uploaded', data_no=?, last_error_message=NULL, updated_at=CURRENT_TIMESTAMP "
                  "WHERE device_id=? AND local_path=? AND file_size=? AND file_mtime=?");
    query.addBindValue(dataNo);
    query.addBindValue(request.deviceId);
    query.addBindValue(request.localPath);
    query.addBindValue(request.fileSize);
    query.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    const QString batchPath = request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath;
    if (!markAccessBatchUploaded(batchPath, dataNo, errorMessage)) return false;
    return true;
}

bool LocalDatabase::markUploadFailed(const UploadRequest &request, const QString &message, QString *errorMessage) {
    QSqlQuery updateConverting(db_);
    updateConverting.prepare("UPDATE upload_records SET file_name=?, upload_path=?, file_size=?, file_mtime=?, file_hash=COALESCE(NULLIF(?, ''), file_hash), "
                             "status='upload_failed', retry_count=retry_count + 1, last_error_message=?, updated_at=CURRENT_TIMESTAMP "
                             "WHERE device_id=? AND local_path=? AND status='converting'");
    updateConverting.addBindValue(request.fileName);
    updateConverting.addBindValue(request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath);
    updateConverting.addBindValue(request.fileSize);
    updateConverting.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    updateConverting.addBindValue(request.fileHash);
    updateConverting.addBindValue(message.left(2000));
    updateConverting.addBindValue(request.deviceId);
    updateConverting.addBindValue(request.localPath);
    if (!updateConverting.exec()) {
        *errorMessage = updateConverting.lastError().text();
        return false;
    }
    if (updateConverting.numRowsAffected() > 0) {
        return true;
    }

    QSqlQuery query(db_);
    query.prepare("INSERT INTO upload_records "
                  "(device_id, local_path, file_name, upload_path, file_size, file_mtime, file_hash, status, retry_count, last_error_message, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, 'upload_failed', 1, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, local_path, file_size, file_mtime) "
                  "DO UPDATE SET status='upload_failed', retry_count=retry_count + 1, "
                  "file_name=excluded.file_name, upload_path=excluded.upload_path, "
                  "last_error_message=excluded.last_error_message, updated_at=CURRENT_TIMESTAMP");
    bindRequest(&query, request);
    query.addBindValue(message.left(2000));
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::markConversionFailed(const UploadRequest &request, const QString &message, QString *errorMessage) {
    QSqlQuery updateConverting(db_);
    updateConverting.prepare("UPDATE upload_records SET file_name=?, upload_path=?, file_size=?, file_mtime=?, file_hash=COALESCE(NULLIF(?, ''), file_hash), "
                             "status='conversion_failed', last_error_message=?, updated_at=CURRENT_TIMESTAMP "
                             "WHERE device_id=? AND local_path=? AND status='converting'");
    updateConverting.addBindValue(request.fileName);
    updateConverting.addBindValue(request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath);
    updateConverting.addBindValue(request.fileSize);
    updateConverting.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    updateConverting.addBindValue(request.fileHash.isNull() ? QString("") : request.fileHash);
    updateConverting.addBindValue(message.left(2000));
    updateConverting.addBindValue(request.deviceId);
    updateConverting.addBindValue(request.localPath);
    if (!updateConverting.exec()) {
        *errorMessage = updateConverting.lastError().text();
        return false;
    }
    if (updateConverting.numRowsAffected() > 0) {
        return true;
    }

    QSqlQuery query(db_);
    query.prepare("INSERT INTO upload_records "
                  "(device_id, local_path, file_name, upload_path, file_size, file_mtime, file_hash, status, retry_count, last_error_message, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, 'conversion_failed', 0, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, local_path, file_size, file_mtime) "
                  "DO UPDATE SET status='conversion_failed', file_name=excluded.file_name, upload_path=excluded.upload_path, "
                  "file_hash=COALESCE(NULLIF(excluded.file_hash, ''), upload_records.file_hash), "
                  "last_error_message=excluded.last_error_message, updated_at=CURRENT_TIMESTAMP");
    query.addBindValue(request.deviceId);
    query.addBindValue(request.localPath);
    query.addBindValue(request.fileName);
    query.addBindValue(request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath);
    query.addBindValue(request.fileSize);
    query.addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(request.fileHash.isNull() ? QString("") : request.fileHash);
    query.addBindValue(message.left(2000));
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::accessCursor(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, AccessCursorState *state, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("SELECT last_cursor_value, pending_cursor_value, pending_data_no, status "
                  "FROM access_table_cursors WHERE device_id=? AND access_file_path=? AND table_name=? AND monitor_column=?");
    query.addBindValue(deviceId);
    query.addBindValue(accessFilePath);
    query.addBindValue(tableName);
    query.addBindValue(monitorColumn);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (query.next() && state != nullptr) {
        state->lastCursorValue = query.value(0).toString();
        state->hasLastCursor = !query.value(0).isNull() && !state->lastCursorValue.isEmpty();
        state->pendingCursorValue = query.value(1).toString();
        state->pendingDataNo = query.value(2).toString();
        state->status = query.value(3).toString();
    }
    return true;
}

bool LocalDatabase::saveAccessSchemaCache(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &schemaHash, const QString &columnsJson, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("INSERT INTO access_schema_cache "
                  "(device_id, access_file_path, table_name, schema_hash, columns_json, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, access_file_path, table_name) "
                  "DO UPDATE SET schema_hash=excluded.schema_hash, columns_json=excluded.columns_json, updated_at=CURRENT_TIMESTAMP");
    query.addBindValue(deviceId);
    query.addBindValue(accessFilePath);
    query.addBindValue(tableName);
    query.addBindValue(schemaHash);
    query.addBindValue(columnsJson);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::setAccessCursor(int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, const QString &cursorValue, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("INSERT INTO access_table_cursors "
                  "(device_id, access_file_path, table_name, monitor_column, last_cursor_value, pending_cursor_value, pending_data_no, status, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, NULL, NULL, 'idle', CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, access_file_path, table_name, monitor_column) "
                  "DO UPDATE SET last_cursor_value=excluded.last_cursor_value, pending_cursor_value=NULL, pending_data_no=NULL, "
                  "status='idle', updated_at=CURRENT_TIMESTAMP");
    query.addBindValue(deviceId);
    query.addBindValue(accessFilePath);
    query.addBindValue(tableName);
    query.addBindValue(monitorColumn);
    query.addBindValue(cursorValue);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::markAccessBatchPending(const QString &deltaPath, int deviceId, const QString &accessFilePath, const QString &tableName, const QString &monitorColumn, const QString &cursorFrom, const QString &cursorTo, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("INSERT INTO access_capture_batches "
                  "(delta_path, device_id, access_file_path, table_name, monitor_column, cursor_from, cursor_to, status, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending_upload', CURRENT_TIMESTAMP) "
                  "ON CONFLICT(delta_path, table_name, monitor_column) DO UPDATE SET cursor_to=excluded.cursor_to, status='pending_upload', updated_at=CURRENT_TIMESTAMP");
    query.addBindValue(deltaPath);
    query.addBindValue(deviceId);
    query.addBindValue(accessFilePath);
    query.addBindValue(tableName);
    query.addBindValue(monitorColumn);
    query.addBindValue(cursorFrom);
    query.addBindValue(cursorTo);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    QSqlQuery cursorQuery(db_);
    cursorQuery.prepare("INSERT INTO access_table_cursors "
                        "(device_id, access_file_path, table_name, monitor_column, last_cursor_value, pending_cursor_value, pending_data_no, status, updated_at) "
                        "VALUES (?, ?, ?, ?, ?, ?, NULL, 'pending_upload', CURRENT_TIMESTAMP) "
                        "ON CONFLICT(device_id, access_file_path, table_name, monitor_column) "
                        "DO UPDATE SET pending_cursor_value=excluded.pending_cursor_value, status='pending_upload', updated_at=CURRENT_TIMESTAMP");
    cursorQuery.addBindValue(deviceId);
    cursorQuery.addBindValue(accessFilePath);
    cursorQuery.addBindValue(tableName);
    cursorQuery.addBindValue(monitorColumn);
    cursorQuery.addBindValue(cursorFrom.isEmpty() ? QVariant() : QVariant(cursorFrom));
    cursorQuery.addBindValue(cursorTo);
    if (!cursorQuery.exec()) {
        *errorMessage = cursorQuery.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::markAccessBatchUploaded(const QString &deltaPath, const QString &dataNo, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("UPDATE access_capture_batches SET data_no=?, status='uploaded', updated_at=CURRENT_TIMESTAMP "
                  "WHERE delta_path=? AND status='pending_upload'");
    query.addBindValue(dataNo);
    query.addBindValue(deltaPath);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }

    QSqlQuery cursorQuery(db_);
    cursorQuery.prepare("UPDATE access_table_cursors SET pending_cursor_value=("
                        "SELECT b.cursor_to FROM access_capture_batches b WHERE b.delta_path=? "
                        "AND b.device_id=access_table_cursors.device_id "
                        "AND b.access_file_path=access_table_cursors.access_file_path "
                        "AND b.table_name=access_table_cursors.table_name "
                        "AND b.monitor_column=access_table_cursors.monitor_column), "
                        "pending_data_no=?, status='uploaded', updated_at=CURRENT_TIMESTAMP "
                        "WHERE EXISTS (SELECT 1 FROM access_capture_batches b WHERE b.delta_path=? "
                        "AND b.device_id=access_table_cursors.device_id "
                        "AND b.access_file_path=access_table_cursors.access_file_path "
                        "AND b.table_name=access_table_cursors.table_name "
                        "AND b.monitor_column=access_table_cursors.monitor_column)");
    cursorQuery.addBindValue(deltaPath);
    cursorQuery.addBindValue(dataNo);
    cursorQuery.addBindValue(deltaPath);
    if (!cursorQuery.exec()) {
        *errorMessage = cursorQuery.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::markTaskResult(const QJsonObject &payload, QString *errorMessage) {
    const QString dataNo = payload.value("dataNo").toString();
    if (dataNo.isEmpty()) {
        return true;
    }
    const QString remoteStatus = payload.value("status").toString();
    const QString localStatus = remoteStatus == "success" ? "parse_success" : "parse_failed";
    const QString error = payload.value("errorMessage").toString();

    QSqlQuery query(db_);
    query.prepare("UPDATE upload_records SET status=?, last_error_message=?, updated_at=CURRENT_TIMESTAMP WHERE data_no=?");
    query.addBindValue(localStatus);
    query.addBindValue(error);
    query.addBindValue(dataNo);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    if (localStatus == "parse_success") {
        QSqlQuery batchQuery(db_);
        batchQuery.prepare("SELECT device_id, access_file_path, table_name, monitor_column, cursor_to "
                           "FROM access_capture_batches WHERE data_no=? AND status='uploaded'");
        batchQuery.addBindValue(dataNo);
        if (!batchQuery.exec()) {
            *errorMessage = batchQuery.lastError().text();
            return false;
        }
        while (batchQuery.next()) {
            QSqlQuery cursorQuery(db_);
            cursorQuery.prepare("INSERT INTO access_table_cursors "
                                "(device_id, access_file_path, table_name, monitor_column, last_cursor_value, pending_cursor_value, pending_data_no, status, updated_at) "
                                "VALUES (?, ?, ?, ?, ?, NULL, NULL, 'idle', CURRENT_TIMESTAMP) "
                                "ON CONFLICT(device_id, access_file_path, table_name, monitor_column) "
                                "DO UPDATE SET last_cursor_value=excluded.last_cursor_value, pending_cursor_value=NULL, "
                                "pending_data_no=NULL, status='idle', updated_at=CURRENT_TIMESTAMP");
            cursorQuery.addBindValue(batchQuery.value(0));
            cursorQuery.addBindValue(batchQuery.value(1));
            cursorQuery.addBindValue(batchQuery.value(2));
            cursorQuery.addBindValue(batchQuery.value(3));
            cursorQuery.addBindValue(batchQuery.value(4));
            if (!cursorQuery.exec()) {
                *errorMessage = cursorQuery.lastError().text();
                return false;
            }
        }
        QSqlQuery doneQuery(db_);
        doneQuery.prepare("UPDATE access_capture_batches SET status='parse_success', updated_at=CURRENT_TIMESTAMP WHERE data_no=?");
        doneQuery.addBindValue(dataNo);
        if (!doneQuery.exec()) {
            *errorMessage = doneQuery.lastError().text();
            return false;
        }
    } else if (localStatus == "parse_failed") {
        QSqlQuery failedQuery(db_);
        failedQuery.prepare("UPDATE access_capture_batches SET status='parse_failed', updated_at=CURRENT_TIMESTAMP WHERE data_no=?");
        failedQuery.addBindValue(dataNo);
        if (!failedQuery.exec()) {
            *errorMessage = failedQuery.lastError().text();
            return false;
        }

        QSqlQuery cursorQuery(db_);
        cursorQuery.prepare("UPDATE access_table_cursors SET pending_cursor_value=NULL, pending_data_no=NULL, status='idle', updated_at=CURRENT_TIMESTAMP "
                            "WHERE pending_data_no=?");
        cursorQuery.addBindValue(dataNo);
        if (!cursorQuery.exec()) {
            *errorMessage = cursorQuery.lastError().text();
            return false;
        }
    }
    return true;
}

int LocalDatabase::recoverInterruptedUploads(QString *errorMessage) {
    QSqlQuery query(db_);
    if (!query.exec("UPDATE upload_records SET status='upload_failed', retry_count=retry_count + 1, "
                    "last_error_message='upload interrupted before workstation restart', updated_at=CURRENT_TIMESTAMP "
                    "WHERE status='uploading'")) {
        *errorMessage = query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

QVector<StoredUpload> LocalDatabase::retryableUploads(int limit, QString *errorMessage) {
    return queryUploads("WHERE status='upload_failed'", "ORDER BY updated_at ASC, id ASC", limit, errorMessage);
}

QVector<StoredUpload> LocalDatabase::pendingParseUploads(int limit, QString *errorMessage) {
    return queryUploads("WHERE status='uploaded' AND data_no IS NOT NULL AND data_no != ''", "ORDER BY updated_at ASC, id ASC", limit, errorMessage);
}

QVector<StoredUpload> LocalDatabase::recentUploads(int limit, QString *errorMessage) {
    return queryUploads(QString(), "ORDER BY updated_at DESC, id DESC", limit, errorMessage);
}

bool LocalDatabase::uploadByDataNo(const QString &dataNo, StoredUpload *upload, QString *errorMessage) {
    if (dataNo.trimmed().isEmpty()) {
        *errorMessage = "dataNo is empty";
        return false;
    }
    const QVector<StoredUpload> uploads = queryUploads("WHERE data_no = ?", "ORDER BY updated_at DESC, id DESC", 1, errorMessage, {dataNo});
    if (!errorMessage->isEmpty()) {
        return false;
    }
    if (uploads.isEmpty()) {
        *errorMessage = "upload record not found: " + dataNo;
        return false;
    }
    if (upload != nullptr) {
        *upload = uploads.first();
    }
    return true;
}

int LocalDatabase::clearFailedUploads(QString *errorMessage) {
    QSqlQuery query(db_);
    if (!query.exec("DELETE FROM upload_records WHERE status IN ('upload_failed', 'parse_failed', 'conversion_failed')")) {
        *errorMessage = query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

QVector<StoredUpload> LocalDatabase::queryUploads(const QString &whereClause, const QString &orderBy, int limit, QString *errorMessage, const QVariantList &whereValues) {
    QVector<StoredUpload> uploads;
    QSqlQuery query(db_);
    query.prepare("SELECT device_id, local_path, file_name, file_size, file_mtime, file_hash, "
                  "status, retry_count, last_error_message, data_no, updated_at, upload_path "
                  "FROM upload_records " + whereClause + " " + orderBy + " LIMIT ?");
    for (const QVariant &value : whereValues) {
        query.addBindValue(value);
    }
    query.addBindValue(limit);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return uploads;
    }
    while (query.next()) {
        StoredUpload upload;
        upload.request.deviceId = query.value(0).toInt();
        upload.request.localPath = query.value(1).toString();
        upload.request.fileName = query.value(2).toString();
        upload.request.fileSize = query.value(3).toLongLong();
        upload.request.fileMtime = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
        upload.request.fileHash = query.value(5).toString();
        upload.status = query.value(6).toString();
        upload.retryCount = query.value(7).toInt();
        upload.lastErrorMessage = query.value(8).toString();
        upload.dataNo = query.value(9).toString();
        upload.updatedAt = query.value(10).toString();
        upload.request.uploadPath = query.value(11).toString();
        uploads.append(upload);
    }
    return uploads;
}

bool LocalDatabase::ensureColumn(const QString &name, const QString &definition, QString *errorMessage) {
    QSqlQuery query(db_);
    if (!query.exec("PRAGMA table_info(upload_records)")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    while (query.next()) {
        if (query.value("name").toString() == name) {
            return true;
        }
    }
    if (!query.exec(definition)) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

void LocalDatabase::bindRequest(QSqlQuery *query, const UploadRequest &request) {
    query->addBindValue(request.deviceId);
    query->addBindValue(request.localPath);
    query->addBindValue(request.fileName);
    query->addBindValue(request.uploadPath.trimmed().isEmpty() ? request.localPath : request.uploadPath);
    query->addBindValue(request.fileSize);
    query->addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    query->addBindValue(request.fileHash);
}
