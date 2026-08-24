#include "SystemIdentity.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace {
int interfaceScore(const QNetworkInterface &iface, const QString &ipv4Address) {
    const QString name = iface.name().toLower();
    const QString humanName = iface.humanReadableName().toLower();
    int score = 0;

    if (!ipv4Address.isEmpty()) score += 40;
    if (name.startsWith("en")) score += 30;
    if (humanName.contains("wi-fi") || humanName.contains("wifi") || humanName.contains("ethernet")) score += 20;
    if (name.startsWith("eth")) score += 20;
    if (name.startsWith("wlan")) score += 20;
    if (name.startsWith("bridge") || humanName.contains("bridge")) score -= 50;
    if (humanName.contains("virtual") || humanName.contains("docker") || humanName.contains("orbstack")) score -= 80;

    return score;
}

QString firstUsableIPv4(const QNetworkInterface &iface) {
    for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
        const QHostAddress address = entry.ip();
        if (address.protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        if (address.isLoopback()) {
            continue;
        }
        const QString text = address.toString();
        if (text.startsWith("169.254.")) {
            continue;
        }
        return text;
    }
    return {};
}
}

SystemIdentityInfo SystemIdentity::detect() {
    SystemIdentityInfo result;
    result.hostname = QHostInfo::localHostName().trimmed();
    if (result.hostname.isEmpty()) {
        result.hostname = QHostInfo::localDomainName().trimmed();
    }
    if (result.hostname.isEmpty()) {
        result.hostname = "qt-workstation";
    }

    struct Candidate {
        QString macAddress;
        QString ipv4Address;
        QString interfaceName;
        int score = 0;
    };
    QVector<Candidate> candidates;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)) {
            continue;
        }
        if (flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString name = iface.name();
        const QString humanName = iface.humanReadableName();
        if (virtualInterfaceName(name) || virtualInterfaceName(humanName)) {
            continue;
        }
        const QString macAddress = iface.hardwareAddress().trimmed().toUpper();
        if (!usableMacAddress(macAddress)) {
            continue;
        }

        Candidate candidate;
        candidate.macAddress = macAddress;
        candidate.ipv4Address = firstUsableIPv4(iface);
        candidate.interfaceName = !humanName.isEmpty() ? humanName : name;
        candidate.score = interfaceScore(iface, candidate.ipv4Address);
        candidates.append(candidate);
    }

    if (!candidates.isEmpty()) {
        const auto best = std::max_element(candidates.cbegin(), candidates.cend(), [](const Candidate &left, const Candidate &right) {
            return left.score < right.score;
        });
        result.macAddress = best->macAddress;
        result.ipv4Address = best->ipv4Address;
        result.interfaceName = best->interfaceName;
    }
    return result;
}

bool SystemIdentity::usableMacAddress(const QString &macAddress) {
    if (macAddress.isEmpty()) {
        return false;
    }
    const QString normalized = macAddress.trimmed().toUpper();
    if (normalized == "00:00:00:00:00:00" || normalized == "FF:FF:FF:FF:FF:FF") {
        return false;
    }
    return normalized.count(':') == 5;
}

bool SystemIdentity::virtualInterfaceName(const QString &name) {
    const QString normalized = name.trimmed().toLower();
    if (normalized.isEmpty()) {
        return false;
    }
    static const QStringList blocked = {
        "lo", "loopback", "utun", "awdl", "llw", "bridge", "p2p", "gif", "stf", "ipsec",
        "docker", "orbstack", "vbox", "vmnet", "zt", "tailscale"
    };
    for (const QString &prefix : blocked) {
        if (normalized.startsWith(prefix) || normalized.contains(prefix)) {
            return true;
        }
    }
    return false;
}
