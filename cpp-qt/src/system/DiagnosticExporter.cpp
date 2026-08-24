#include "DiagnosticExporter.h"

#include "system/LogManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

bool DiagnosticExporter::exportBundle(
    const QString &parentDirectory,
    const WorkstationSettings &settings,
    const RuntimeConfig &runtimeConfig,
    const QVector<StoredUpload> &uploads,
    QString *resultPath,
    QString *errorMessage
) {
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString bundleDirectory = QDir(parentDirectory).filePath("netstar-workstation-diagnostics-" + timestamp);
    QDir dir;
    if (!dir.mkpath(bundleDirectory)) {
        *errorMessage = "cannot create diagnostic directory: " + bundleDirectory;
        return false;
    }

    QJsonObject settingsJson;
    settingsJson["baseUrl"] = settings.baseUrl;
    settingsJson["mac"] = settings.mac;
    settingsJson["ip"] = settings.ip;
    settingsJson["hostname"] = settings.hostname;
    settingsJson["token"] = maskedToken(settings.token);
    settingsJson["exportedAt"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    settingsJson["appVersion"] = QString(APP_VERSION);
    if (!writeJsonFile(QDir(bundleDirectory).filePath("settings.redacted.json"), settingsJson, errorMessage)) {
        return false;
    }

    QJsonObject runtimeJson;
    runtimeJson["configVersion"] = runtimeConfig.configVersion;
    runtimeJson["heartbeatIntervalSeconds"] = runtimeConfig.heartbeatIntervalSeconds;
    runtimeJson["items"] = runtimeConfig.toJsonArray();
    if (!writeJsonFile(QDir(bundleDirectory).filePath("runtime-config.json"), runtimeJson, errorMessage)) {
        return false;
    }

    QJsonArray uploadArray;
    for (const StoredUpload &upload : uploads) {
        QJsonObject object;
        object["deviceId"] = upload.request.deviceId;
        object["localPath"] = upload.request.localPath;
        object["fileName"] = upload.request.fileName;
        object["fileSize"] = QString::number(upload.request.fileSize);
        object["fileMtime"] = upload.request.fileMtime.toUTC().toString(Qt::ISODateWithMs);
        object["fileHash"] = upload.request.fileHash;
        object["status"] = upload.status;
        object["retryCount"] = upload.retryCount;
        object["dataNo"] = upload.dataNo;
        object["lastErrorMessage"] = upload.lastErrorMessage;
        object["updatedAt"] = upload.updatedAt;
        uploadArray.append(object);
    }
    QJsonObject uploadsJson;
    uploadsJson["records"] = uploadArray;
    if (!writeJsonFile(QDir(bundleDirectory).filePath("upload-records.json"), uploadsJson, errorMessage)) {
        return false;
    }

    if (!copyLogFile(bundleDirectory, errorMessage)) {
        return false;
    }

    QString zipPath;
    QString zipError;
    if (tryZipBundle(bundleDirectory, &zipPath, &zipError)) {
        *resultPath = zipPath;
    } else {
        *resultPath = bundleDirectory;
        LogManager::appendLine("diagnostic zip skipped: " + zipError);
    }
    return true;
}

QString DiagnosticExporter::maskedToken(const QString &token) {
    if (token.isEmpty()) {
        return QString();
    }
    if (token.size() <= 8) {
        return "***";
    }
    return token.left(4) + "***" + token.right(4);
}

bool DiagnosticExporter::writeJsonFile(const QString &path, const QJsonObject &object, QString *errorMessage) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *errorMessage = "cannot write file: " + path;
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        *errorMessage = "cannot commit file: " + path;
        return false;
    }
    return true;
}

bool DiagnosticExporter::copyLogFile(const QString &bundleDirectory, QString *errorMessage) {
    const QString source = LogManager::logFilePath();
    const QString target = QDir(bundleDirectory).filePath("workstation.log");
    if (!QFileInfo::exists(source)) {
        QSaveFile placeholder(target);
        if (!placeholder.open(QIODevice::WriteOnly | QIODevice::Text)) {
            *errorMessage = "cannot create empty log placeholder";
            return false;
        }
        placeholder.write("log file does not exist yet\n");
        return placeholder.commit();
    }
    QFile::remove(target);
    if (!QFile::copy(source, target)) {
        *errorMessage = "cannot copy log file: " + source;
        return false;
    }
    return true;
}

bool DiagnosticExporter::tryZipBundle(const QString &bundleDirectory, QString *zipPath, QString *errorMessage) {
    const QString zipProgram = QStandardPaths::findExecutable("zip");
    if (zipProgram.isEmpty()) {
        *errorMessage = "zip executable not found";
        return false;
    }

    const QFileInfo bundleInfo(bundleDirectory);
    const QString parent = bundleInfo.dir().absolutePath();
    const QString zipFile = parent + "/" + bundleInfo.fileName() + ".zip";
    QFile::remove(zipFile);

    QProcess process;
    process.setWorkingDirectory(parent);
    process.start(zipProgram, {"-qr", zipFile, bundleInfo.fileName()});
    if (!process.waitForFinished(30000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *errorMessage = "zip failed: " + QString::fromUtf8(process.readAllStandardError()).left(500);
        return false;
    }
    *zipPath = zipFile;
    return true;
}
