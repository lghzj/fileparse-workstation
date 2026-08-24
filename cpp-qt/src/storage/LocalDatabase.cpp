#include "LocalDatabase.h"

#include <QDir>
#include <QStandardPaths>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

bool LocalDatabase::open(QString *errorMessage) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dir)) {
        *errorMessage = QString("cannot create app data directory: %1").arg(dir);
        return false;
    }

    db_ = QSqlDatabase::addDatabase("QSQLITE");
    db_.setDatabaseName(dir + "/workstation.db");
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
    if (!ensureColumn("retry_count", "ALTER TABLE upload_records ADD COLUMN retry_count INTEGER NOT NULL DEFAULT 0", errorMessage)) return false;
    if (!ensureColumn("last_error_message", "ALTER TABLE upload_records ADD COLUMN last_error_message TEXT", errorMessage)) return false;

    if (!query.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_upload_records_file "
                    "ON upload_records(device_id, local_path, file_size, file_mtime)")) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool LocalDatabase::recordUpload(const UploadRequest &request, const QString &status, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("INSERT INTO upload_records "
                  "(device_id, local_path, file_name, file_size, file_mtime, file_hash, status, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, local_path, file_size, file_mtime) "
                  "DO UPDATE SET status=excluded.status, file_hash=excluded.file_hash, "
                  "last_error_message=NULL, updated_at=CURRENT_TIMESTAMP");
    bindRequest(&query, request);
    query.addBindValue(status);
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
        return false;
    }
    return true;
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
    return true;
}

bool LocalDatabase::markUploadFailed(const UploadRequest &request, const QString &message, QString *errorMessage) {
    QSqlQuery query(db_);
    query.prepare("INSERT INTO upload_records "
                  "(device_id, local_path, file_name, file_size, file_mtime, file_hash, status, retry_count, last_error_message, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, 'upload_failed', 1, ?, CURRENT_TIMESTAMP) "
                  "ON CONFLICT(device_id, local_path, file_size, file_mtime) "
                  "DO UPDATE SET status='upload_failed', retry_count=retry_count + 1, "
                  "last_error_message=excluded.last_error_message, updated_at=CURRENT_TIMESTAMP");
    bindRequest(&query, request);
    query.addBindValue(message.left(2000));
    if (!query.exec()) {
        *errorMessage = query.lastError().text();
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

QVector<StoredUpload> LocalDatabase::recentUploads(int limit, QString *errorMessage) {
    return queryUploads(QString(), "ORDER BY updated_at DESC, id DESC", limit, errorMessage);
}

int LocalDatabase::clearFailedUploads(QString *errorMessage) {
    QSqlQuery query(db_);
    if (!query.exec("DELETE FROM upload_records WHERE status IN ('upload_failed', 'parse_failed')")) {
        *errorMessage = query.lastError().text();
        return -1;
    }
    return query.numRowsAffected();
}

QVector<StoredUpload> LocalDatabase::queryUploads(const QString &whereClause, const QString &orderBy, int limit, QString *errorMessage) {
    QVector<StoredUpload> uploads;
    QSqlQuery query(db_);
    query.prepare("SELECT device_id, local_path, file_name, file_size, file_mtime, file_hash, "
                  "status, retry_count, last_error_message, data_no, updated_at "
                  "FROM upload_records " + whereClause + " " + orderBy + " LIMIT ?");
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
    query->addBindValue(request.fileSize);
    query->addBindValue(request.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    query->addBindValue(request.fileHash);
}
