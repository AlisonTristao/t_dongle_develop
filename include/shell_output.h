#pragma once

#include <Arduino.h>

#include <string>

namespace ShellOutput {

void writeLine(Stream& io, const String& line);

void writeLine(Stream& io, const char* line);

void printTagged(Stream& io, const char* tag, const String& message);

void printTagged(Stream& io, const char* tag, const char* message);

void printResponse(Stream& io, const std::string& response);

} // namespace ShellOutput
