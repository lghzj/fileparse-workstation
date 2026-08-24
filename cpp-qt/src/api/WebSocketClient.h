#pragma once

#include "api/ApiClient.h"

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QWebSocket>

class WebSocketClient final : public QObject {
    Q_OBJECT

public:
    explicit WebSocketClient(QObject *parent = nullptr);

    void setSettings(const WorkstationSettings &settings);
    void setHeartbeatIntervalSeconds(int seconds);
    void start(const QString &wsUrl = QString());
    void stop();
    void sendDoctorResult(const QString &messageId, const QString &requestId, const QJsonObject &result);

signals:
    void connected();
    void disconnected();
    void connectionStateChanged(const QString &state);
    void configReceived(const QJsonObject &payload);
    void taskResultReceived(const QJsonObject &payload);
    void doctorRunRequested(const QString &messageId, const QString &requestId, bool checkNetwork);
    void logMessage(const QString &message);

private:
    QUrl resolvedWsUrl(const QString &wsUrl) const;
    void openSocket();
    void scheduleReconnect();
    void resetReconnectBackoff();
    void sendHeartbeat();
    void handleTextMessage(const QString &message);

    QWebSocket socket_;
    QTimer heartbeatTimer_;
    QTimer reconnectTimer_;
    WorkstationSettings settings_;
    QString wsUrlOverride_;
    int heartbeatIntervalSeconds_ = 30;
    int reconnectDelayMs_ = 1000;
    int maxReconnectDelayMs_ = 60000;
    bool shouldReconnect_ = false;
    bool stopping_ = false;
};
