#include "ShellOutput.h"

#include <cstring>

namespace {

constexpr const char* kCommandPrefix = "$ ";
constexpr const char* kOutputPrefix = "! ";

bool isEspNowTypeToken(const String& token) {
    return token.equalsIgnoreCase("info") ||
           token.equalsIgnoreCase("cmdo") ||
           token.equalsIgnoreCase("tele") ||
           token.equalsIgnoreCase("erro") ||
           token.equalsIgnoreCase("debg") ||
           token.equalsIgnoreCase("none");
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

bool isEspNowStructuredLine(const String& line) {
    String lower = line;
    lower.trim();

    if (!lower.startsWith("[")) {
        return false;
    }

    const int32_t firstClose = lower.indexOf(']');
    if (firstClose <= 1) {
        return false;
    }

    const String firstToken = lower.substring(1, firstClose);
    if (!isEspNowTypeToken(firstToken)) {
        return false;
    }

    return (firstClose + 1 < static_cast<int32_t>(lower.length())) && lower[firstClose + 1] == '[';
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
        isEspNowStructuredLine(String(line))) {
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

void writeTextWithPrefix(Stream& io, const char* prefix, const char* text, bool prefixEmptyLine) {
    const char* safeText = (text != nullptr) ? text : "";
    const char* safePrefix = (prefix != nullptr) ? prefix : "";
    const size_t textLen = std::strlen(safeText);
    const size_t prefixLen = std::strlen(safePrefix);

    auto writeChunk = [&](const char* data, size_t len) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = len;
        uint32_t retries = 0;

        while (remaining > 0) {
            const size_t sent = io.write(bytes, remaining);
            if (sent == 0) {
                ++retries;
                if (retries > 8) {
                    break;
                }
                delay(1);
                continue;
            }

            bytes += sent;
            remaining -= sent;
        }
    };

    if (textLen == 0) {
        if (prefixLen > 0 && prefixEmptyLine) {
            io.write('\r');
            writeChunk(safePrefix, prefixLen);
        }
        io.write('\r');
        io.write('\n');
        return;
    }

    io.write('\r');
    if (prefixLen > 0) {
        writeChunk(safePrefix, prefixLen);
    }

    bool lineOpen = true;
    for (size_t i = 0; i < textLen; ++i) {
        const char ch = safeText[i];
        if (ch == '\0') {
            break;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && (i + 1) < textLen && safeText[i + 1] == '\n') {
                ++i;
            }

            io.write('\r');
            io.write('\n');
            lineOpen = false;

            if ((i + 1) < textLen) {
                io.write('\r');
                if (prefixLen > 0) {
                    writeChunk(safePrefix, prefixLen);
                }
                lineOpen = true;
            }
            continue;
        }

        io.write(ch);
    }

    if (lineOpen) {
        io.write('\r');
        io.write('\n');
    }
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
    writeTextWithPrefix(io, nullptr, line, false);
}

void writeLine(Stream& io, const String& line) {
    writeLine(io, line.c_str());
}

void writeLine(Stream& io, const char* line) {
    const char* safeLine = (line != nullptr) ? line : "";
    if (safeLine[0] == '\0') {
        writeTextWithPrefix(io, kOutputPrefix, safeLine, true);
        return;
    }

    if (hasVisualPrefix(safeLine)) {
        writeTextWithPrefix(io, nullptr, safeLine, false);
        return;
    }

    writeTextWithPrefix(io, kOutputPrefix, safeLine, true);
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
