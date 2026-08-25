#pragma once

#include <Arduino.h>
#include <DonglePeripherals.h>

namespace StartupConfig{

/**
 * @brief Initializes LED/LCD and announces boot status. Does not wait for a
 * serial terminal/monitor to attach -- see the definition for why that wait
 * was removed.
 *
 * No interactive prompt follows: the clock is queried/corrected by the BTP
 * client via the "dongle clock" / "dongle set_clock" shell commands.
 */
void announceBoot(DonglePeripherals& peripherals);

} // namespace StartupConfig
