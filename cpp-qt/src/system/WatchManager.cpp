#include "WatchManager.h"

#include "system/FileHasher.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

WatchManager::WatchManager(QObject *parent) : QObject(parent) {
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &WatchManager::scanOnce);
}

void WatchManager::setRuntimeConfig(const RuntimeConfig &config) {
    config_ = config;
    snapshots_.clear();
    emitted_.clear();
    emit logMessage(QString("watch config loaded: %1 item(s)").arg(config_.devices.size()));
}

void WatchManager::start() {
    if (!timer_.isActive()) {
        scanOnce();
        timer_.start();
        emit logMessage("watcher started");
    }
}

void WatchManager::stop() {
    timer_.stop();
    emit logMessage("watcher stopped");
}

void WatchManager::scanOnce() {
    if (scanning_) {
        return;
    }
    scanning_ = true;
    for (const DeviceConfig &device : config_.devices) {
        if (!device.enabled) {
            continue;
        }
        const QFileInfo watchInfo(device.watchPath);
        if (!watchInfo.exists()) {
            emit logMessage("watch path unavailable: " + device.watchPath);
            continue;
        }
        if (watchInfo.isFile()) {
            inspectFile(device, watchInfo.absoluteFilePath());
            continue;
        }
        if (!watchInfo.isDir()) {
            emit logMessage("watch path is not a file or directory: " + device.watchPath);
            continue;
        }

        QDir dir(watchInfo.absoluteFilePath());
        const QStringList files = collectFiles(device, dir);
        for (const QString &path : files) {
            inspectFile(device, path);
        }
    }
    scanning_ = false;
}

QStringList WatchManager::collectFiles(const DeviceConfig &device, const QDir &dir) const {
    QStringList files;
    const QString rootPath = dir.absolutePath();

    if (!device.recursive) {
        const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (!matchesWatchFilePattern(entry.fileName(), device.watchFilePattern)) {
                continue;
            }
            files.append(entry.absoluteFilePath());
            if (files.size() >= maxFilesPerScan_) {
                return files;
            }
        }
        return files;
    }

    QDirIterator iterator(rootPath, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo entry(path);
        if (!matchesWatchFilePattern(entry.fileName(), device.watchFilePattern)) {
            continue;
        }
        if (device.maxDepth > 0) {
            const QString relative = QDir(rootPath).relativeFilePath(path);
            const int depth = relative.count('/');
            if (depth > device.maxDepth) {
                continue;
            }
        }
        files.append(path);
        if (files.size() >= maxFilesPerScan_) {
            return files;
        }
    }
    return files;
}

void WatchManager::inspectFile(const DeviceConfig &device, const QString &path) {
    QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return;
    }
    if (temporaryFile(fileInfo.fileName())) {
        return;
    }
    if (!matchesWatchFilePattern(fileInfo.fileName(), device.watchFilePattern)) {
        return;
    }
    if (!supportedFileType(fileInfo.fileName(), device.fileType)) {
        return;
    }

    const QString key = QString("%1|%2|%3").arg(device.deviceId).arg(path).arg(fileInfo.lastModified().toMSecsSinceEpoch());
    if (emitted_.contains(key)) {
        return;
    }

    Snapshot &snapshot = snapshots_[key];
    const qint64 size = fileInfo.size();
    const qint64 mtimeMs = fileInfo.lastModified().toMSecsSinceEpoch();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (snapshot.size != size || snapshot.mtimeMs != mtimeMs) {
        snapshot.size = size;
        snapshot.mtimeMs = mtimeMs;
        snapshot.firstStableSeen = now;
        return;
    }

    if (snapshot.firstStableSeen.secsTo(now) < device.stableSeconds) {
        return;
    }

    if (device.fileType.compare("access", Qt::CaseInsensitive) == 0) {
        emitted_.insert(key);
        emit accessFileReady(device, fileInfo.absoluteFilePath());
        return;
    }

    QString errorMessage;
    const QString hash = FileHasher::sha256(path, &errorMessage);
    if (!errorMessage.isEmpty()) {
        emit logMessage(errorMessage);
        return;
    }

    UploadRequest request;
    request.deviceId = device.deviceId;
    request.localPath = fileInfo.absoluteFilePath();
    request.fileName = fileInfo.fileName();
    request.fileSize = size;
    request.fileMtime = fileInfo.lastModified();
    request.fileHash = hash;
    emitted_.insert(key);
    emit fileReady(request);
}

bool WatchManager::matchesWatchFilePattern(const QString &fileName, const QString &watchFilePattern) {
    QString normalized = watchFilePattern.trimmed();
    if (normalized.isEmpty()) {
        return true;
    }
    normalized.replace(',', ';');
    const QStringList patterns = normalized.split(';', Qt::SkipEmptyParts);
    for (const QString &rawPattern : patterns) {
        const QString pattern = rawPattern.trimmed();
        if (pattern.isEmpty() || pattern.contains('/') || pattern.contains('\\')) {
            continue;
        }
        const QRegularExpression regex(
            QRegularExpression::wildcardToRegularExpression(pattern),
            QRegularExpression::CaseInsensitiveOption
        );
        if (regex.match(fileName).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool WatchManager::supportedFileType(const QString &fileName, const QString &fileType) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    const QString type = fileType.toLower();
    if (type == "word") return suffix == "doc" || suffix == "docx";
    if (type == "excel") return suffix == "xls" || suffix == "xlsx" || suffix == "csv";
    if (type == "csv") return suffix == "csv";
    if (type == "ppt") return suffix == "ppt" || suffix == "pptx";
    if (type == "pdf") return suffix == "pdf";
    if (type == "txt") return suffix == "txt";
    if (type == "access") return suffix == "mdb" || suffix == "accdb";
    return false;
}

bool WatchManager::temporaryFile(const QString &fileName) {
    const QString lower = fileName.toLower();
    return lower.startsWith("~$") || lower.startsWith(".") || lower.endsWith(".tmp") || lower.endsWith(".part") || lower.endsWith(".crdownload") || lower.endsWith(".ldb") || lower.endsWith(".laccdb");
}
