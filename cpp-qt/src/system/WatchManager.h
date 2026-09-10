#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

class WatchManager final : public QObject {
    Q_OBJECT

public:
    explicit WatchManager(QObject *parent = nullptr);

    void setRuntimeConfig(const RuntimeConfig &config);
    void start(bool baselineExisting = true);
    void stop();
    void resetState();

signals:
    void fileReady(const UploadRequest &request);
    void accessConversionStarted(const UploadRequest &request);
    void accessFileReady(const DeviceConfig &device, const QString &path);
    void logMessage(const QString &message);

private:
    struct Snapshot {
        qint64 size = -1;
        qint64 mtimeMs = -1;
        QDateTime firstStableSeen;
    };

    void scanOnce();
    void baselineExistingFiles();
    void inspectFile(const DeviceConfig &device, const QString &path);
    QStringList collectFiles(const DeviceConfig &device, const QDir &dir) const;
    static QString emissionKey(const DeviceConfig &device, const QFileInfo &fileInfo);
    static bool matchesWatchFilePattern(const QString &fileName, const QString &watchFilePattern);
    static bool supportedFileType(const QString &fileName, const QString &fileType);
    static bool temporaryFile(const QString &fileName);

    QTimer timer_;
    RuntimeConfig config_;
    QHash<QString, Snapshot> snapshots_;
    QSet<QString> emitted_;
    bool scanning_ = false;
    int maxFilesPerScan_ = 1000;
};
