#pragma once

#include <cstddef>
#include <cstdint>

/** Pure scheduling policy shared by the ESP32 worker and native tests.
 * Queue storage/wakeup remains FreeRTOS-specific in EspNowManager; this file
 * only decides which non-empty priority class owns the next driver send. */
namespace RadioTxScheduler {

enum class Priority : std::uint8_t {
    Critical = 0U, // heartbeat, COMMAND and CONTROL
    Control = 1U,  // LOG/TERMINAL and ordinary management
    Data = 2U,     // TELEMETRY; first class dropped under saturation
    Count = 3U,
};

constexpr std::size_t kPriorityCount = static_cast<std::size_t>(Priority::Count);
constexpr std::size_t kScheduleLength = 7U;

struct Selection {
    bool found;
    Priority priority;
    std::size_t nextCursor;
};

/** Weighted fair choice: Critical:Control:Data = 4:2:1 when all queues are
 * busy, while immediately using any lower class when higher ones are empty. */
Selection choose(const bool available[kPriorityCount], std::size_t cursor) noexcept;

} // namespace RadioTxScheduler
