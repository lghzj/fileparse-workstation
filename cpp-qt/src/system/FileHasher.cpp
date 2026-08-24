#include "FileHasher.h"

#include <QCryptographicHash>
#include <QFile>

QString FileHasher::sha256(const QString &path, QString *errorMessage) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = QString("cannot open file for hash: %1").arg(path);
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *errorMessage = QString("cannot read file for hash: %1").arg(path);
        return {};
    }
    return "sha256:" + QString::fromLatin1(hash.result().toHex());
}
