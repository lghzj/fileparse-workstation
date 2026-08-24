#include "LogManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

bool LogManager::appendLine(const QString &line, QString *errorMessage) {
    const QString path = logFilePath();
    QDir dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot create log directory: " + dir.absolutePath();
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (errorMessage != nullptr) {
            *errorMessage = "cannot open log file: " + path;
        }
        return false;
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << line << '\n';
    return true;
}

QString LogManager::logFilePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    return dir + "/workstation.log";
}
