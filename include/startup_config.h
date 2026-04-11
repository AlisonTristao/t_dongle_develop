#pragma once

#include <Arduino.h>
#include <DonglePeripherals.h>

namespace StartupConfig {

/**
 * @brief Waits for serial monitor attach while updating startup visuals.
 *
 * Visual behavior:
 * - RGB LED carousel while disconnected.
 * - LCD status text for disconnected/connected states.
 */
void waitForSerialAndAnimateLed(DonglePeripherals& peripherals);

/**
 * @brief Prompts date/time and applies it to ESP RTC clock.
 * @param io Stream used for prompt and feedback output.
 * @param timeoutMs Max wait for user input.
 */
void promptAndSetDateTime(Stream& io, uint32_t timeoutMs = 30000);

} // namespace StartupConfig
