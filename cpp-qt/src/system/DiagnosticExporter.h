#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"
#include "storage/LocalDatabase.h"

#include <QString>

class DiagnosticExporter final {
public:
    static bool exportBundle(
        const QString &parentDirectory,
        const WorkstationSettings &settings,
        const RuntimeConfig &runtimeConfig,
        const QVector<StoredUpload> &uploads,
        QString *resultPath,
        QString *errorMessage
    );

private:
    static QString maskedToken(const QString &token);
    static bool writeJsonFile(const QString &path, const QJsonObject &object, QString *errorMessage);
    static bool copyLogFile(const QString &bundleDirectory, QString *errorMessage);
    static bool tryZipBundle(const QString &bundleDirectory, QString *zipPath, QString *errorMessage);
};
