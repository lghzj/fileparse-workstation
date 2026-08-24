#include "RuntimeConfig.h"

DeviceConfig DeviceConfig::fromJson(const QJsonObject &object) {
    DeviceConfig config;
    config.deviceId = object.value("deviceId").toInt();
    config.deviceCode = object.value("deviceCode").toString();
    config.deviceName = object.value("deviceName").toString();
    config.watchPath = object.value("watchPath").toString();
    config.fileType = object.value("fileType").toString();
    config.stableSeconds = object.value("stableSeconds").toInt(2);
    config.enabled = object.value("enabled").toBool(true);
    config.recursive = object.value("recursive").toBool(false);
    config.maxDepth = object.value("maxDepth").toInt(0);
    return config;
}

bool DeviceConfig::isValid(QString *errorMessage) const {
    if (deviceId <= 0) {
        if (errorMessage) *errorMessage = "deviceId must be greater than zero";
        return false;
    }
    if (watchPath.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "watchPath is required";
        return false;
    }
    if (fileType.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "fileType is required";
        return false;
    }
    if (stableSeconds <= 0) {
        if (errorMessage) *errorMessage = "stableSeconds must be greater than zero";
        return false;
    }
    if (maxDepth < 0) {
        if (errorMessage) *errorMessage = "maxDepth must be greater than or equal to zero";
        return false;
    }
    return true;
}

RuntimeConfig RuntimeConfig::fromJson(const QJsonObject &object, QString *errorMessage) {
    RuntimeConfig config;
    config.configVersion = object.value("configVersion").toInt();
    config.heartbeatIntervalSeconds = object.value("heartbeatIntervalSeconds").toInt(30);
    if (config.heartbeatIntervalSeconds <= 0) {
        config.heartbeatIntervalSeconds = 30;
    }

    const QJsonArray items = object.value("items").toArray();
    for (int index = 0; index < items.size(); ++index) {
        DeviceConfig device = DeviceConfig::fromJson(items[index].toObject());
        QString deviceError;
        if (!device.isValid(&deviceError)) {
            if (errorMessage) {
                *errorMessage = QString("invalid config item %1: %2").arg(index).arg(deviceError);
            }
            return {};
        }
        config.devices.append(device);
    }
    return config;
}

QJsonArray RuntimeConfig::toJsonArray() const {
    QJsonArray array;
    for (const DeviceConfig &device : devices) {
        QJsonObject object;
        object["deviceId"] = device.deviceId;
        object["deviceCode"] = device.deviceCode;
        object["deviceName"] = device.deviceName;
        object["watchPath"] = device.watchPath;
        object["fileType"] = device.fileType;
        object["stableSeconds"] = device.stableSeconds;
        object["enabled"] = device.enabled;
        object["recursive"] = device.recursive;
        object["maxDepth"] = device.maxDepth;
        array.append(object);
    }
    return array;
}
