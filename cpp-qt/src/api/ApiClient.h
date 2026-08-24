#pragma once

#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

struct WorkstationSettings {
    QString baseUrl;
    QString mac;
    QString ip;
    QString token;
    QString hostname;
};

struct UploadRequest {
    int deviceId = 0;
    QString localPath;
    QString fileName;
    qint64 fileSize = 0;
    QDateTime fileMtime;
    QString fileHash;
};

class ApiClient final : public QObject {
    Q_OBJECT

public:
    explicit ApiClient(QObject *parent = nullptr);

    void setSettings(const WorkstationSettings &settings);
    void registerWorkstation();
    void pullConfig();
    void uploadFile(const UploadRequest &request);

signals:
    void registerSucceeded(const QJsonObject &payload);
    void configPulled(const QJsonObject &payload);
    void uploadSucceeded(const UploadRequest &request, const QJsonObject &payload);
    void uploadFailed(const UploadRequest &request, const QString &message);
    void requestFailed(const QString &operation, const QString &message);

private:
    QUrl apiUrl(const QString &path) const;
    void applyAuth(QNetworkRequest *request) const;
    static QJsonObject unwrapResponse(const QByteArray &body, QString *errorMessage);

    QNetworkAccessManager manager_;
    WorkstationSettings settings_;
};
