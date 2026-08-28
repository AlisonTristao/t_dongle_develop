#pragma once

#include <ctime>
#include <string>

namespace ClockText {

// Parses either the human-facing local calendar form
// "YYYY-MM-DD HH:MM:SS" or the unambiguous automatic-client form
// "@<Unix seconds>". The caller owns the process TZ used by mktime() for
// the calendar form; the epoch form is independent of it.
bool parse(const std::string& text, time_t& outEpoch) noexcept;

}  // namespace ClockText
