#include "ConfigStore.h"

#include "system/SystemIdentity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVector>

namespace {
QVector<QString> legacyConfigCandidates() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(cwd).filePath("python/workstation/server.json"),
        QDir(cwd).filePath("workstation/server.json"),
        QDir(cwd).filePath("../python/workstation/server.json"),
        QDir(cwd).filePath("../workstation/server.json"),
        QDir(appDir).filePath("../python/workstation/server.json"),
        QDir(appDir).filePath("../workstation/server.json"),
        QDir(appDir).filePath("../../python/workstation/server.json"),
        QDir(appDir).filePath("../../workstation/server.json"),
        QDir(appDir).filePath("../../../python/workstation/server.json"),
        QDir(appDir).filePath("../../../workstation/server.json"),
    };
}

QJsonObject loadLegacyConfigObject() {
    QSet<QString> seen;
    for (const QString &candidate : legacyConfigCandidates()) {
        const QFileInfo info(candidate);
        const QString canonicalPath = info.canonicalFilePath();
        if (!info.exists() || !info.isFile() || seen.contains(canonicalPath)) {
            continue;
        }
        seen.insert(canonicalPath);

        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) {
            return document.object();
        }
    }
    return {};
}

QString jsonString(const QJsonObject &object, const QString &key) {
    return object.value(key).toString().trimmed();
}

void migrateLegacyBaseSettings(QSettings *settings) {
    if (settings == nullptr) {
        return;
    }
    const QJsonObject legacy = loadLegacyConfigObject();
    if (legacy.isEmpty()) {
        return;
    }

    const auto fillIfEmpty = [settings, &legacy](const QString &settingsKey, const QString &legacyKey) {
        if (!settings->value(settingsKey).toString().trimmed().isEmpty()) {
            return;
        }
        const QString value = jsonString(legacy, legacyKey);
        if (!value.isEmpty()) {
            settings->setValue(settingsKey, value);
        }
    };

    fillIfEmpty("server/baseUrl", "apiBaseUrl");
    fillIfEmpty("workstation/mac", "mac");
    fillIfEmpty("workstation/ip", "ip");
    fillIfEmpty("workstation/token", "workstationToken");
    fillIfEmpty("workstation/hostname", "hostname");
}

QJsonObject legacyRuntimeConfigObject() {
    const QJsonObject legacy = loadLegacyConfigObject();
    if (legacy.isEmpty() || !legacy.value("items").isArray()) {
        return {};
    }
    QJsonObject runtime;
    runtime["configVersion"] = legacy.value("configVersion").toInt(0);
    runtime["heartbeatIntervalSeconds"] = legacy.value("heartbeatIntervalSeconds").toInt(30);
    runtime["items"] = legacy.value("items").toArray();
    return runtime;
}
}

WorkstationSettings ConfigStore::load() const {
    QSettings settings;
    migrateLegacyBaseSettings(&settings);
    const SystemIdentityInfo identity = SystemIdentity::detect();
    WorkstationSettings result;
    result.baseUrl = settings.value("server/baseUrl", "http://127.0.0.1:8080").toString();
    result.mac = settings.value("workstation/mac").toString().trimmed();
    if (result.mac.isEmpty()) {
        result.mac = identity.macAddress;
    }
    result.ip = settings.value("workstation/ip").toString().trimmed();
    if (result.ip.isEmpty()) {
        result.ip = identity.ipv4Address;
    }
    result.token = settings.value("workstation/token").toString();
    result.hostname = settings.value("workstation/hostname").toString().trimmed();
    if (result.hostname.isEmpty() || result.hostname == "qt-workstation") {
        result.hostname = identity.hostname;
    }
    return result;
}

void ConfigStore::save(const WorkstationSettings &value) const {
    QSettings settings;
    settings.setValue("server/baseUrl", value.baseUrl);
    settings.setValue("workstation/mac", value.mac);
    settings.setValue("workstation/ip", value.ip);
    settings.setValue("workstation/token", value.token);
    settings.setValue("workstation/hostname", value.hostname);
}

RuntimeConfig ConfigStore::loadRuntimeConfig(QString *errorMessage) const {
    QSettings settings;
    QByteArray json = settings.value("runtime/configJson").toByteArray();
    if (json.isEmpty()) {
        const QJsonObject legacy = legacyRuntimeConfigObject();
        if (!legacy.isEmpty()) {
            json = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
            settings.setValue("runtime/configJson", json);
        }
    }
    if (json.isEmpty()) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        if (errorMessage) *errorMessage = "cached runtime config is not valid json";
        return {};
    }
    return RuntimeConfig::fromJson(document.object(), errorMessage);
}

void ConfigStore::saveRuntimeConfig(const RuntimeConfig &config) const {
    QJsonObject object;
    object["configVersion"] = config.configVersion;
    object["heartbeatIntervalSeconds"] = config.heartbeatIntervalSeconds;
    object["items"] = config.toJsonArray();
    QSettings settings;
    settings.setValue("runtime/configJson", QJsonDocument(object).toJson(QJsonDocument::Compact));
}
