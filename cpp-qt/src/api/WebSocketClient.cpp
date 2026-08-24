#include "WebSocketClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QtGlobal>
#include <QUuid>

WebSocketClient::WebSocketClient(QObject *parent) : QObject(parent) {
    heartbeatTimer_.setInterval(heartbeatIntervalSeconds_ * 1000);
    connect(&heartbeatTimer_, &QTimer::timeout, this, &WebSocketClient::sendHeartbeat);

    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &WebSocketClient::openSocket);

    connect(&socket_, &QWebSocket::connected, this, [this]() {
        resetReconnectBackoff();
        emit logMessage("websocket connected");
        emit connectionStateChanged("connected");
        emit connected();
        sendHeartbeat();
        heartbeatTimer_.start();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this]() {
        heartbeatTimer_.stop();
        emit logMessage("websocket disconnected");
        emit connectionStateChanged(shouldReconnect_ && !stopping_ ? "reconnecting" : "disconnected");
        emit disconnected();
        if (shouldReconnect_ && !stopping_) {
            scheduleReconnect();
        }
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, &WebSocketClient::handleTextMessage);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&socket_, &QWebSocket::errorOccurred, this, [this]() {
        emit logMessage("websocket error: " + socket_.errorString());
    });
#else
    connect(&socket_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this, [this]() {
        emit logMessage("websocket error: " + socket_.errorString());
    });
#endif
}

void WebSocketClient::setSettings(const WorkstationSettings &settings) {
    settings_ = settings;
}

void WebSocketClient::setHeartbeatIntervalSeconds(int seconds) {
    heartbeatIntervalSeconds_ = seconds > 0 ? seconds : 30;
    heartbeatTimer_.setInterval(heartbeatIntervalSeconds_ * 1000);
}

void WebSocketClient::start(const QString &wsUrl) {
    wsUrlOverride_ = wsUrl;
    shouldReconnect_ = true;
    stopping_ = false;
    reconnectTimer_.stop();
    emit connectionStateChanged("connecting");
    openSocket();
}

void WebSocketClient::stop() {
    stopping_ = true;
    shouldReconnect_ = false;
    reconnectTimer_.stop();
    heartbeatTimer_.stop();
    emit connectionStateChanged("stopped");
    socket_.close();
}

void WebSocketClient::openSocket() {
    if (!shouldReconnect_ || stopping_) {
        return;
    }
    if (settings_.token.trimmed().isEmpty() || settings_.mac.trimmed().isEmpty()) {
        emit logMessage("websocket skipped: token or mac is missing");
        scheduleReconnect();
        return;
    }

    if (socket_.state() == QAbstractSocket::ConnectedState || socket_.state() == QAbstractSocket::ConnectingState) {
        socket_.abort();
    }

    QNetworkRequest request(resolvedWsUrl(wsUrlOverride_));
    request.setRawHeader("Authorization", QString("Bearer %1").arg(settings_.token).toUtf8());
    request.setRawHeader("X-Workstation-Mac", settings_.mac.toUtf8());
    emit logMessage("websocket connecting: " + request.url().toString());
    socket_.open(request);
}

void WebSocketClient::scheduleReconnect() {
    if (!shouldReconnect_ || stopping_) {
        return;
    }
    if (reconnectTimer_.isActive()) {
        return;
    }
    emit logMessage(QString("websocket reconnect scheduled in %1 ms").arg(reconnectDelayMs_));
    emit connectionStateChanged("reconnecting");
    reconnectTimer_.start(reconnectDelayMs_);
    reconnectDelayMs_ = qMin(reconnectDelayMs_ * 2, maxReconnectDelayMs_);
}

void WebSocketClient::resetReconnectBackoff() {
    reconnectDelayMs_ = 1000;
}

QUrl WebSocketClient::resolvedWsUrl(const QString &wsUrl) const {
    if (!wsUrl.trimmed().isEmpty()) {
        QUrl explicitUrl(wsUrl.trimmed());
        if (explicitUrl.isRelative()) {
            QUrl base(settings_.baseUrl);
            explicitUrl.setScheme(base.scheme() == "https" ? "wss" : "ws");
            explicitUrl.setHost(base.host());
            explicitUrl.setPort(base.port());
        }
        return explicitUrl;
    }

    QUrl base(settings_.baseUrl);
    base.setScheme(base.scheme() == "https" ? "wss" : "ws");
    base.setPath("/file");
    base.setQuery(QString());
    return base;
}

void WebSocketClient::sendHeartbeat() {
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    QJsonObject message;
    message["type"] = "heartbeat";
    message["messageId"] = "msg-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    message["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    message["data"] = QJsonObject{{"mac", settings_.mac}, {"ip", settings_.ip}};
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void WebSocketClient::sendDoctorResult(const QString &messageId, const QString &requestId, const QJsonObject &result) {
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        emit logMessage("doctor.result skipped: websocket is not connected");
        return;
    }
    QJsonObject data;
    data["requestId"] = requestId;
    data["status"] = result.value("status").toString();
    data["checks"] = result.value("checks").toArray();

    QJsonObject message;
    message["type"] = "doctor.result";
    message["messageId"] = messageId;
    message["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    message["data"] = data;
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    emit logMessage("doctor.result sent requestId=" + requestId);
}

void WebSocketClient::handleTextMessage(const QString &message) {
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) {
        emit logMessage("websocket ignored non-json message");
        return;
    }
    const QJsonObject root = document.object();
    const QString type = root.value("type").toString();
    if (type == "heartbeat.ack") {
        emit logMessage("heartbeat ack");
        return;
    }
    if (type == "config.full") {
        emit configReceived(root.value("data").toObject());
        return;
    }
    if (type == "task.result") {
        emit taskResultReceived(root.value("data").toObject());
        return;
    }
    if (type == "doctor.run") {
        const QJsonObject data = root.value("data").toObject();
        emit doctorRunRequested(
            root.value("messageId").toString(),
            data.value("requestId").toString(),
            data.value("checkNetwork").toBool(true)
        );
        return;
    }
    emit logMessage("websocket message: " + type);
}
