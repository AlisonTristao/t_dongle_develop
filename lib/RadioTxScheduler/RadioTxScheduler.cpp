#include "RadioTxScheduler.h"

namespace RadioTxScheduler {

Selection choose(const bool available[kPriorityCount], std::size_t cursor) noexcept {
    if (available == nullptr) {
        return {false, Priority::Critical, 0U};
    }

    constexpr Priority pattern[kScheduleLength] = {
        Priority::Critical, Priority::Critical, Priority::Control,
        Priority::Critical, Priority::Control, Priority::Critical,
        Priority::Data,
    };

    cursor %= kScheduleLength;
    for (std::size_t attempt = 0U; attempt < kScheduleLength; ++attempt) {
        const Priority candidate = pattern[cursor];
        cursor = (cursor + 1U) % kScheduleLength;
        if (available[static_cast<std::size_t>(candidate)]) {
            return {true, candidate, cursor};
        }
    }

    return {false, Priority::Critical, cursor};
}

} // namespace RadioTxScheduler
