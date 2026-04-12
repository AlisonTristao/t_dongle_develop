#include "shell_output.h"

#include <cstring>

namespace {

bool isErrorLike(const String& text) {
    String lower = text;
    lower.toLowerCase();

    if (
        lower.indexOf("falhou") >= 0 ||
        lower.indexOf("erro") >= 0 ||
        lower.indexOf("error") >= 0 ||
        lower.indexOf("invalid") >= 0
    ) {
        return true;
    }

    return false;
}

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

} // namespace

namespace ShellOutput {

void writeLine(Stream& io, const String& line) {
    writeLine(io, line.c_str());
}

void writeLine(Stream& io, const char* line) {
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

void printTagged(Stream& io, const char* tag, const String& message) {
    const String safeTag = normalizeTag(tag);
    String line = "[" + safeTag + "]";

    if (message.length() > 0) {
        line += " ";
        line += message;
    }

    writeLine(io, line);
}

void printTagged(Stream& io, const char* tag, const char* message) {
    printTagged(io, tag, (message != nullptr) ? String(message) : String(""));
}

void printResponse(Stream& io, const std::string& response) {
    // Normalize response into clean single-line chunks for terminal rendering.
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
            if (line.startsWith("[")) {
                writeLine(io, line);
                continue;
            }

            const bool error = isErrorLike(line);
            const String tagged = error
                ? String("[shell][erro] ") + line
                : String("[shell][ok] ") + line;
            writeLine(io, tagged);
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
}

} // namespace ShellOutput
