#include "RuntimeConfig.h"

AccessSamplingConfig AccessSamplingConfig::fromJson(const QJsonObject &object) {
    AccessSamplingConfig config;
    config.enabled = object.value("enabled").toBool(false);
    config.timeColumn = object.value("timeColumn").toString().trimmed();
    config.intervalHours = object.value("intervalHours").toInt(0);
    config.strategy = object.value("strategy").toString().trimmed();
    return config;
}

QJsonObject AccessSamplingConfig::toJson() const {
    QJsonObject object;
    object["enabled"] = enabled;
    object["timeColumn"] = timeColumn;
    object["intervalHours"] = intervalHours;
    object["strategy"] = strategy;
    return object;
}

bool AccessSamplingConfig::isValid(QString *errorMessage) const {
    if (!enabled) {
        return true;
    }
    if (timeColumn.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "accessRule.sampling.timeColumn is required";
        return false;
    }
    if (intervalHours <= 0) {
        if (errorMessage) *errorMessage = "accessRule.sampling.intervalHours must be greater than zero";
        return false;
    }
    if (strategy != "last") {
        if (errorMessage) *errorMessage = "accessRule.sampling.strategy currently supports only last";
        return false;
    }
    return true;
}

AccessRuleConfig AccessRuleConfig::fromJson(const QJsonObject &object) {
    AccessRuleConfig config;
    config.tableName = object.value("tableName").toString().trimmed();
    const QJsonArray columns = object.value("monitorColumns").toArray();
    for (const QJsonValue &value : columns) {
        const QString column = value.toString().trimmed();
        if (!column.isEmpty()) {
            config.monitorColumns.append(column);
        }
    }
    config.maxRows = object.value("maxRows").toInt(1000);
    if (object.value("sampling").isObject()) {
        config.sampling = AccessSamplingConfig::fromJson(object.value("sampling").toObject());
    }
    return config;
}

QJsonObject AccessRuleConfig::toJson() const {
    QJsonObject object;
    object["tableName"] = tableName;
    QJsonArray columns;
    for (const QString &column : monitorColumns) {
        columns.append(column);
    }
    object["monitorColumns"] = columns;
    object["maxRows"] = maxRows;
    if (sampling.enabled) {
        object["sampling"] = sampling.toJson();
    }
    return object;
}

bool AccessRuleConfig::isValid(QString *errorMessage) const {
    if (tableName.trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "accessRule.tableName is required";
        return false;
    }
    if (monitorColumns.size() != 1 || monitorColumns.first().trimmed().isEmpty()) {
        if (errorMessage) *errorMessage = "accessRule.monitorColumns must contain exactly one column";
        return false;
    }
    if (maxRows <= 0) {
        if (errorMessage) *errorMessage = "accessRule.maxRows must be greater than zero";
        return false;
    }
    if (!sampling.isValid(errorMessage)) {
        return false;
    }
    return true;
}

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
    config.accessMode = object.value("accessMode").toString("table_delta");
    config.accessFirstRunPolicy = object.value("accessFirstRunPolicy").toString("export_all");
    const QJsonArray accessRule = object.value("accessRule").toArray();
    for (const QJsonValue &value : accessRule) {
        if (value.isObject()) {
            config.accessRules.append(AccessRuleConfig::fromJson(value.toObject()));
        }
    }
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
    if (fileType.compare("access", Qt::CaseInsensitive) == 0) {
        if (accessMode != "table_delta" && accessMode != "new_file") {
            if (errorMessage) *errorMessage = "accessMode must be table_delta or new_file";
            return false;
        }
        if (accessFirstRunPolicy != "export_all" && accessFirstRunPolicy != "start_from_latest") {
            if (errorMessage) *errorMessage = "accessFirstRunPolicy must be export_all or start_from_latest";
            return false;
        }
        if (accessRules.isEmpty()) {
            if (errorMessage) *errorMessage = "accessRule is required when fileType is access";
            return false;
        }
        for (int index = 0; index < accessRules.size(); ++index) {
            QString ruleError;
            if (!accessRules[index].isValid(&ruleError)) {
                if (errorMessage) *errorMessage = QString("invalid accessRule %1: %2").arg(index).arg(ruleError);
                return false;
            }
        }
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
        if (device.fileType.compare("access", Qt::CaseInsensitive) == 0) {
            object["accessMode"] = device.accessMode;
            object["accessFirstRunPolicy"] = device.accessFirstRunPolicy;
            QJsonArray accessRule;
            for (const AccessRuleConfig &rule : device.accessRules) {
                accessRule.append(rule.toJson());
            }
            object["accessRule"] = accessRule;
        }
        array.append(object);
    }
    return array;
}
