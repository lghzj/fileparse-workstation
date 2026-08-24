#pragma once

#include <QString>

struct SystemIdentityInfo {
    QString macAddress;
    QString ipv4Address;
    QString hostname;
    QString interfaceName;
};

class SystemIdentity final {
public:
    static SystemIdentityInfo detect();

private:
    static bool usableMacAddress(const QString &macAddress);
    static bool virtualInterfaceName(const QString &name);
};
