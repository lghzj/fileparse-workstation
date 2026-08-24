#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"

#include <QJsonObject>

class SystemDiagnostics final {
public:
    static QJsonObject run(const WorkstationSettings &settings, const RuntimeConfig &runtimeConfig, bool checkNetwork);

private:
    static QJsonObject ok(const QString &name, const QString &message);
    static QJsonObject fail(const QString &name, const QString &message);
    static QJsonObject skip(const QString &name, const QString &message);
    static QJsonObject checkHttp(const QString &baseUrl);
    static QString uploadDatabasePath();
};
