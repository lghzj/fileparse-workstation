#include "SystemDiagnostics.h"

#include "system/LogManager.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

QJsonObject SystemDiagnostics::run(const WorkstationSettings &settings, const RuntimeConfig &runtimeConfig, bool checkNetwork) {
    QJsonArray checks;

    const QUrl baseUrl(settings.baseUrl.trimmed());
    const bool validBaseUrl = baseUrl.isValid() && (baseUrl.scheme() == "http" || baseUrl.scheme() == "https") && !baseUrl.host().isEmpty();
    if (validBaseUrl && !settings.mac.trimmed().isEmpty()) {
        checks.append(ok("config", settings.baseUrl.trimmed()));
    } else {
        checks.append(fail("config", "平台地址或 MAC 未配置"));
    }

    checks.append(settings.token.trimmed().isEmpty() ? fail("token", "missing") : ok("token", "configured"));

    if (runtimeConfig.devices.isEmpty()) {
        checks.append(fail("watchPath", "未加载监听目录配置"));
    } else {
        for (const DeviceConfig &device : runtimeConfig.devices) {
            if (!device.enabled) {
                continue;
            }
            const QFileInfo info(device.watchPath);
            if (!info.exists() || !info.isDir()) {
                checks.append(fail("watchPath", "not found: " + device.watchPath));
                continue;
            }
            const QDir dir(device.watchPath);
            checks.append(dir.isReadable() ? ok("watchPath", device.watchPath) : fail("watchPath", "not readable: " + device.watchPath));
        }
    }

    if (checkNetwork) {
        checks.append(checkHttp(settings.baseUrl));
    } else {
        checks.append(skip("api", "network check skipped"));
    }

    const QFileInfo dbInfo(uploadDatabasePath());
    if (dbInfo.dir().exists() || QDir().mkpath(dbInfo.dir().absolutePath())) {
        checks.append(ok("stateDb", dbInfo.absoluteFilePath()));
    } else {
        checks.append(fail("stateDb", "cannot access: " + dbInfo.dir().absolutePath()));
    }

    const QFileInfo logInfo(LogManager::logFilePath());
    QDir logDir = logInfo.dir();
    if (logDir.exists() || logDir.mkpath(".")) {
        const QString probePath = logDir.filePath(".write-test");
        QSaveFile probe(probePath);
        if (probe.open(QIODevice::WriteOnly | QIODevice::Text) && probe.write("ok") == 2 && probe.commit()) {
            QFile::remove(probePath);
            checks.append(ok("logDir", logDir.absolutePath()));
        } else {
            checks.append(fail("logDir", "not writable: " + logDir.absolutePath()));
        }
    } else {
        checks.append(fail("logDir", "cannot create: " + logDir.absolutePath()));
    }

    bool failed = false;
    for (const QJsonValue &value : checks) {
        const QString status = value.toObject().value("status").toString();
        if (status != "ok" && status != "skipped") {
            failed = true;
            break;
        }
    }

    QJsonObject result;
    result["status"] = failed ? "failed" : "ok";
    result["checks"] = checks;
    return result;
}

QJsonObject SystemDiagnostics::ok(const QString &name, const QString &message) {
    return QJsonObject{{"name", name}, {"status", "ok"}, {"message", message}};
}

QJsonObject SystemDiagnostics::fail(const QString &name, const QString &message) {
    return QJsonObject{{"name", name}, {"status", "failed"}, {"message", message}};
}

QJsonObject SystemDiagnostics::skip(const QString &name, const QString &message) {
    return QJsonObject{{"name", name}, {"status", "skipped"}, {"message", message}};
}

QJsonObject SystemDiagnostics::checkHttp(const QString &baseUrl) {
    QUrl url(baseUrl.trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        return fail("api", "invalid base url");
    }
    QString path = url.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setPath(path + "/api/fileparse/health");
    url.setQuery(QString());

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = reply->errorString();
        reply->deleteLater();
        if (statusCode > 0 && statusCode < 400) {
            return ok("api", url.toString());
        }
        return fail("api", statusCode > 0 ? QString("HTTP %1: %2").arg(statusCode).arg(url.toString()) : error);
    }

    reply->abort();
    reply->deleteLater();
    return fail("api", "timeout: " + url.toString());
}

QString SystemDiagnostics::uploadDatabasePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/workstation.db";
}
