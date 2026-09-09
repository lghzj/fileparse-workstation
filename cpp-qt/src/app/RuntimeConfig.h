#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

struct AccessSamplingConfig {
    bool enabled = false;
    QString timeColumn;
    int intervalHours = 0;
    QString strategy;

    static AccessSamplingConfig fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
    bool isValid(QString *errorMessage = nullptr) const;
};

struct AccessRuleConfig {
    QString tableName;
    QStringList monitorColumns;
    int maxRows = 1000;
    AccessSamplingConfig sampling;

    static AccessRuleConfig fromJson(const QJsonObject &object);
    QJsonObject toJson() const;
    bool isValid(QString *errorMessage = nullptr) const;
};

struct DeviceConfig {
    int deviceId = 0;
    QString deviceCode;
    QString deviceName;
    QString watchPath;
    QString watchFilePattern;
    QString fileType;
    int stableSeconds = 2;
    bool enabled = true;
    bool recursive = false;
    int maxDepth = 0;
    QString accessMode;
    QString accessFirstRunPolicy;
    QVector<AccessRuleConfig> accessRules;

    static DeviceConfig fromJson(const QJsonObject &object);
    bool isValid(QString *errorMessage = nullptr) const;
};

Q_DECLARE_METATYPE(DeviceConfig)

struct RuntimeConfig {
    int configVersion = 0;
    int heartbeatIntervalSeconds = 30;
    QVector<DeviceConfig> devices;

    static RuntimeConfig fromJson(const QJsonObject &object, QString *errorMessage = nullptr);
    QJsonArray toJsonArray() const;
};
