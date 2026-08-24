#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

struct DeviceConfig {
    int deviceId = 0;
    QString deviceCode;
    QString deviceName;
    QString watchPath;
    QString fileType;
    int stableSeconds = 2;
    bool enabled = true;
    bool recursive = false;
    int maxDepth = 0;

    static DeviceConfig fromJson(const QJsonObject &object);
    bool isValid(QString *errorMessage = nullptr) const;
};

struct RuntimeConfig {
    int configVersion = 0;
    int heartbeatIntervalSeconds = 30;
    QVector<DeviceConfig> devices;

    static RuntimeConfig fromJson(const QJsonObject &object, QString *errorMessage = nullptr);
    QJsonArray toJsonArray() const;
};
