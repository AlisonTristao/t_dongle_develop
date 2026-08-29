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

// Same per-line formatting printResponse() applies (CR+LF line endings, the
// "! " output prefix, leading-bracket-tag stripping, ESP-NOW structured-line
// passthrough), but returned as a string instead of written to a Stream --
// for the BTP terminal channel, whose "output" is chunked into TERMINAL_OUT
// frames rather than printed. Ends with a trailing CR+LF.
std::string renderResponse(const std::string& response);

} // namespace ShellOutput
