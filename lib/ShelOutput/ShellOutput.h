#pragma once

#include <Arduino.h>

#include <string>

namespace ShellOutput {

const char* commandPrefix();
String commandPrompt();

void writeRawLine(Stream& io, const String& line);
void writeRawLine(Stream& io, const char* line);

void writeLine(Stream& io, const String& line);
void writeLine(Stream& io, const char* line);
void writeLines(Stream& io, const String& text);
void writeLines(Stream& io, const char* text);

void printTagged(Stream& io, const char* tag, const String& message);
void printTagged(Stream& io, const char* tag, const char* message);

void printResponse(Stream& io, const std::string& response);

} // namespace ShellOutput
