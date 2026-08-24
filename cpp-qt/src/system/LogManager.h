#pragma once

#include <QString>

class LogManager final {
public:
    static bool appendLine(const QString &line, QString *errorMessage = nullptr);
    static QString logFilePath();
};
