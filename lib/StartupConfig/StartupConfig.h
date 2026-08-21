#pragma once

#include <Arduino.h>
#include <DonglePeripherals.h>

namespace StartupConfig{

/**
 * @brief Waits for serial monitor attach while updating startup visuals.
 *
 * Visual behavior:
 * - RGB LED carousel while disconnected.
 * - LCD status text for disconnected/connected states.
 *
 * No interactive prompt follows: the clock is queried/corrected by the BTP
 * client via the "dongle clock" / "dongle set_clock" shell commands.
 */
void waitForSerialAndAnimateLed(DonglePeripherals& peripherals);

} // namespace StartupConfig
