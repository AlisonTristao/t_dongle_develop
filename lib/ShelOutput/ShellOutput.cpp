#include "ShellOutput.h"

#include <cstring>

namespace {

constexpr const char* kCommandPrefix = "$ ";
constexpr const char* kOutputPrefix = "! ";

String normalizeTag(const char* tag) {
    String out = (tag != nullptr) ? String(tag) : String("shell");
    out.trim();
    if (out.length() == 0) {
        out = "shell";
    }

    if (out.startsWith("[")) {
        out.remove(0, 1);
    }
    if (out.endsWith("]")) {
        out.remove(out.length() - 1, 1);
    }

    out.trim();
    if (out.length() == 0) {
        out = "shell";
    }

    return out;
}

bool isEspNowStructuredLine(const String& line) {
    String lower = line;
    lower.trim();
    lower.toLowerCase();
    return lower.startsWith("[espnow][");
}

String stripLeadingBracketTags(const String& input) {
    String out = input;
    out.trim();

    if (isEspNowStructuredLine(out)) {
        return out;
    }

    while (out.startsWith("[")) {
        const int32_t close = out.indexOf(']');
        if (close <= 0) {
            break;
        }

        out = out.substring(close + 1);
        out.trim();
    }

    return out;
}

bool hasVisualPrefix(const char* line) {
    if (line == nullptr) {
        return false;
    }

    if (std::strncmp(line, kOutputPrefix, std::strlen(kOutputPrefix)) == 0 ||
        std::strncmp(line, kCommandPrefix, std::strlen(kCommandPrefix)) == 0 ||
        std::strncmp(line, "[ESPNOW][", 9) == 0 ||
        std::strncmp(line, "[espnow][", 9) == 0) {
        return true;
    }

    return false;
}

String normalizeNewlines(const char* text) {
    if (text == nullptr) {
        return String("");
    }

    String normalized = String(text);
    //normalized.replace("\r\n", "\n");
    //normalized.replace('\r', '\n');
    return normalized;
}

void writeBufferWithNewLine(Stream& io, const char* line) {
    // Ensure output starts at the beginning of the current terminal line.
    io.write('\r');

    const char* safeLine = (line != nullptr) ? line : "";
    const uint8_t* data = reinterpret_cast<const uint8_t*>(safeLine);
    size_t remaining = std::strlen(safeLine);
    uint32_t retries = 0;

    while (remaining > 0) {
        const size_t sent = io.write(data, remaining);
        if (sent == 0) {
            ++retries;
            if (retries > 8) {
                break;
            }
            delay(1);
            continue;
        }

        data += sent;
        remaining -= sent;
    }

    io.write('\r');
    io.write('\n');
}

} // namespace

namespace ShellOutput {

const char* commandPrefix() {
    return kCommandPrefix;
}

String commandPrompt() {
    return String(kCommandPrefix);
}

void writeRawLine(Stream& io, const String& line) {
    writeRawLine(io, line.c_str());
}

void writeRawLine(Stream& io, const char* line) {
    writeBufferWithNewLine(io, line);
}

void writeLine(Stream& io, const String& line) {
    writeLine(io, line.c_str());
}

void writeLine(Stream& io, const char* line) {
    const char* safeLine = (line != nullptr) ? line : "";
    if (safeLine[0] == '\0') {
        writeBufferWithNewLine(io, safeLine);
        return;
    }

    if (hasVisualPrefix(safeLine)) {
        writeBufferWithNewLine(io, safeLine);
        return;
    }

    String prefixed = String(kOutputPrefix) + safeLine;
    writeBufferWithNewLine(io, prefixed.c_str());
}

void writeLines(Stream& io, const char* text) {
    String normalized = normalizeNewlines(text);
    int32_t start = 0;

    while (start <= static_cast<int32_t>(normalized.length())) {
        const int32_t newline = normalized.indexOf('\n', start);
        String line;

        if (newline < 0) {
            line = normalized.substring(start);
        } else {
            line = normalized.substring(start, newline);
        }

        line.trim();
        if (line.length() > 0) {
            writeLine(io, line.c_str());
        }

        if (newline < 0) {
            break;
        }

        start = newline + 1;
    }
}

void writeLines(Stream& io, const String& text) {
    writeLines(io, text.c_str());
}

void printTagged(Stream& io, const char* tag, const String& message) {
    const String safeTag = normalizeTag(tag);
    String line = message;
    line.trim();

    if (line.length() == 0) {
        return;
    }

    if (safeTag.equalsIgnoreCase("espnow") && isEspNowStructuredLine(line)) {
        writeRawLine(io, line);
        return;
    }

    line = stripLeadingBracketTags(line);
    if (line.length() == 0) {
        return;
    }

    writeLine(io, line);
}

void printTagged(Stream& io, const char* tag, const char* message) {
    printTagged(io, tag, (message != nullptr) ? String(message) : String(""));
}

void printResponse(Stream& io, const std::string& response) {
    String text = String(response.c_str());
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    text.trim();
    if (text.length() == 0) {
        return;
    }

    int32_t start = 0;
    while (start <= static_cast<int32_t>(text.length())) {
        const int32_t newline = text.indexOf('\n', start);
        String line;

        if (newline < 0) {
            line = text.substring(start);
        } else {
            line = text.substring(start, newline);
        }

        line.trim();
        if (line.length() > 0) {
            if (isEspNowStructuredLine(line)) {
                writeRawLine(io, line);
            } else {
                const String cleanLine = stripLeadingBracketTags(line);
                if (cleanLine.length() > 0) {
                    writeLine(io, cleanLine);
                }
            }
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
}

} // namespace ShellOutput
