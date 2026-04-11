#pragma once

#include <Arduino.h>
#include <EspNowManager.h>
#include <DatabaseStore.h>

namespace EspNowConfig {

void attachCallbacks(EspNowManager& manager, Stream& io, DatabaseStore* database = nullptr);

} // namespace EspNowConfig
