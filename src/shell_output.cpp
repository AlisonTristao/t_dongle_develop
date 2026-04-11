#include "shell_output.h"

namespace {

const char* outputPrefix(const String& text) {
    String lower = text;
    lower.toLowerCase();

    if (
        lower.indexOf("falhou") >= 0 ||
        lower.indexOf("erro") >= 0 ||
        lower.indexOf("error") >= 0 ||
        lower.indexOf("invalid") >= 0
    ) {
        return "! ";
    }

    return "> ";
}

} // namespace

namespace ShellOutput {

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
            // Prefix each output line to clearly separate shell result from input.
            io.print(outputPrefix(line));
            io.println(line);
        }

        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
}

} // namespace ShellOutput
