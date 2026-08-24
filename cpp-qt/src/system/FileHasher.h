#pragma once

#include <QString>

class FileHasher final {
public:
    static QString sha256(const QString &path, QString *errorMessage);
};
