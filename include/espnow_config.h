#pragma once

#include <Arduino.h>
#include <EspNowManager.h>

namespace EspNowConfig {

void attachCallbacks(EspNowManager& manager, Stream& io);

} // namespace EspNowConfig
