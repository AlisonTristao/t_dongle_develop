#include "ClockText.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace ClockText {

bool parse(const std::string& text, time_t& outEpoch) noexcept {
    if (text.size() > 1U && text.front() == '@') {
        errno = 0;
        char* end = nullptr;
        const long long raw = std::strtoll(text.c_str() + 1, &end, 10);
        const time_t parsed = static_cast<time_t>(raw);
        if (errno != 0 || end == text.c_str() + 1 || *end != '\0' || raw <= 0 ||
            static_cast<long long>(parsed) != raw) {
            return false;
        }
        outEpoch = parsed;
        return true;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour,
                    &minute, &second) != 6) {
        return false;
    }
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    std::tm tmValue = {};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;
    tmValue.tm_isdst = -1;

    const time_t epoch = std::mktime(&tmValue);
    if (epoch <= 0) {
        return false;
    }
    outEpoch = epoch;
    return true;
}

}  // namespace ClockText
