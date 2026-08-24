#include "ApiClient.h"

#include <QFile>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

ApiClient::ApiClient(QObject *parent) : QObject(parent) {}

void ApiClient::setSettings(const WorkstationSettings &settings) {
    settings_ = settings;
}

void ApiClient::registerWorkstation() {
    QNetworkRequest request(apiUrl("/api/fileparse/workstations/register"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["mac"] = settings_.mac;
    if (!settings_.ip.trimmed().isEmpty()) {
        body["ip"] = settings_.ip.trimmed();
    }
    body["hostname"] = settings_.hostname;
    body["clientVersion"] = QString(APP_VERSION);

    auto *reply = manager_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit requestFailed("register", reply->errorString());
            return;
        }
        QString errorMessage;
        const QJsonObject payload = unwrapResponse(reply->readAll(), &errorMessage);
        if (!errorMessage.isEmpty()) {
            emit requestFailed("register", errorMessage);
            return;
        }
        emit registerSucceeded(payload);
    });
}

void ApiClient::pullConfig() {
    QUrl url = apiUrl("/api/fileparse/workstations/config");
    QUrlQuery query;
    query.addQueryItem("mac", settings_.mac);
    url.setQuery(query);

    QNetworkRequest request(url);
    applyAuth(&request);

    auto *reply = manager_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit requestFailed("pullConfig", reply->errorString());
            return;
        }
        QString errorMessage;
        const QJsonObject payload = unwrapResponse(reply->readAll(), &errorMessage);
        if (!errorMessage.isEmpty()) {
            emit requestFailed("pullConfig", errorMessage);
            return;
        }
        emit configPulled(payload);
    });
}

void ApiClient::uploadFile(const UploadRequest &upload) {
    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    const auto addField = [multiPart](const QString &name, const QString &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multiPart->append(part);
    };

    addField("deviceId", QString::number(upload.deviceId));
    addField("localPath", upload.localPath);
    addField("fileName", upload.fileName);
    addField("fileSize", QString::number(upload.fileSize));
    addField("fileMtime", upload.fileMtime.toUTC().toString(Qt::ISODateWithMs));
    addField("fileHash", upload.fileHash);

    auto *file = new QFile(upload.localPath);
    if (!file->open(QIODevice::ReadOnly)) {
        file->deleteLater();
        multiPart->deleteLater();
        const QString message = QString("cannot open file: %1").arg(upload.localPath);
        emit uploadFailed(upload, message);
        emit requestFailed("upload", message);
        return;
    }

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"file\"; filename=\"%1\"").arg(upload.fileName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    filePart.setBodyDevice(file);
    file->setParent(multiPart);
    multiPart->append(filePart);

    QNetworkRequest request(apiUrl("/api/fileparse/files/upload"));
    applyAuth(&request);

    auto *reply = manager_.post(request, multiPart);
    multiPart->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, upload]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit uploadFailed(upload, reply->errorString());
            emit requestFailed("upload", reply->errorString());
            return;
        }
        QString errorMessage;
        const QJsonObject payload = unwrapResponse(reply->readAll(), &errorMessage);
        if (!errorMessage.isEmpty()) {
            emit uploadFailed(upload, errorMessage);
            emit requestFailed("upload", errorMessage);
            return;
        }
        emit uploadSucceeded(upload, payload);
    });
}

QUrl ApiClient::apiUrl(const QString &path) const {
    QString base = settings_.baseUrl.trimmed();
    while (base.endsWith('/')) {
        base.chop(1);
    }
    return QUrl(base + path);
}

void ApiClient::applyAuth(QNetworkRequest *request) const {
    request->setRawHeader("X-Workstation-Mac", settings_.mac.toUtf8());
    request->setRawHeader("Authorization", QString("Bearer %1").arg(settings_.token).toUtf8());
}

QJsonObject ApiClient::unwrapResponse(const QByteArray &body, QString *errorMessage) {
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        *errorMessage = "server returned non-json response";
        return {};
    }

    const QJsonObject root = document.object();
    const int code = root.value("code").toInt(0);
    if (root.contains("code") && code != 0) {
        *errorMessage = root.value("message").toString("server returned error");
        return {};
    }

    if (root.value("data").isObject()) {
        return root.value("data").toObject();
    }
    return root;
}
