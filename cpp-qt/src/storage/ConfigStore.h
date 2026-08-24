#pragma once

#include "api/ApiClient.h"
#include "app/RuntimeConfig.h"

#include <QSettings>

class ConfigStore final {
public:
    WorkstationSettings load() const;
    void save(const WorkstationSettings &settings) const;
    RuntimeConfig loadRuntimeConfig(QString *errorMessage = nullptr) const;
    void saveRuntimeConfig(const RuntimeConfig &config) const;
};
