#pragma once

#include <Arduino.h>

#include <string>

namespace ShellOutput {

void printResponse(Stream& io, const std::string& response);

} // namespace ShellOutput
