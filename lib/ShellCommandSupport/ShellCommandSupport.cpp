#include "ShellCommandSupport.h"

#include "ClockText.h"
#include "SerialMux.h"
#include "ShellOutput.h"

#include <ShellStyle.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

using std::string;

ShellCommandSupport::Context g_ctx = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
string g_commandOutputBuffer;
string g_shellResponseBuffer;
string g_currentUserId = "serial";

bool isTagToken(const string& token) {
    if (token.empty()) {
        return false;
    }

    for (const char ch : token) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!(std::isalpha(uch) != 0 || ch == '_' || ch == '-')) {
            return false;
        }
    }

    return true;
}

bool isEspNowStructuredLine(const string& text) {
    string lower = ShellCommandSupport::trimCopy(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return lower.rfind("[espnow][", 0) == 0;
}

string stripLeadingModuleTags(const string& text) {
    string out = ShellCommandSupport::trimCopy(text);
    if (out.empty() || isEspNowStructuredLine(out)) {
        return out;
    }

    while (!out.empty() && out.front() == '[') {
        const size_t close = out.find(']');
        if (close == string::npos || close <= 1) {
            break;
        }

        const string tag = out.substr(1, close - 1);
        if (!isTagToken(tag)) {
            break;
        }

        out = ShellCommandSupport::trimCopy(out.substr(close + 1));
    }

    return out;
}

string normalizeOutputTag(const string& text) {
    string s = ShellCommandSupport::trimCopy(text);
    if (s.empty()) {
        return s;
    }

    if (isEspNowStructuredLine(s)) {
        return s;
    }

    const string stripped = stripLeadingModuleTags(s);
    if (!stripped.empty()) {
        return stripped;
    }

    return s;
}

uint16_t lcdColorForLine(const string& text) {
    // This panel is currently running with inverted visual polarity,
    // so semantic colors are compensated before drawing.
    constexpr bool kPanelInvertedColors = true;
    constexpr bool kPanelSwapRedBlue = true;
    const auto toPanelColor = [](uint16_t desiredColor) -> uint16_t {
        auto swapRedBlue565 = [](uint16_t color) -> uint16_t {
            const uint16_t r = static_cast<uint16_t>((color >> 11) & 0x1F);
            const uint16_t g = static_cast<uint16_t>((color >> 5) & 0x3F);
            const uint16_t b = static_cast<uint16_t>(color & 0x1F);
            return static_cast<uint16_t>((b << 11) | (g << 5) | r);
        };

        uint16_t panelColor = desiredColor;
        if (kPanelInvertedColors) {
            panelColor = static_cast<uint16_t>(~panelColor);
        }
        if (kPanelSwapRedBlue) {
            panelColor = swapRedBlue565(panelColor);
        }

        return panelColor;
    };

    // Same classification the BTP terminal uses for its ANSI colours
    // (TinyShell ShellStyle), mapped onto the four panel colours -- so the LCD
    // and the terminal always agree on what a line "is".
    switch (shell_classify_line(text)) {
        case ShellMsgClass::Error:
            return toPanelColor(ST77XX_RED);
        case ShellMsgClass::Warning:
        case ShellMsgClass::Help:
            return toPanelColor(ST77XX_YELLOW);
        case ShellMsgClass::Success:
            return toPanelColor(ST77XX_GREEN);
        case ShellMsgClass::Output:
        case ShellMsgClass::Muted:
        default:
            return toPanelColor(ST77XX_WHITE);
    }
}

} // namespace

namespace ShellCommandSupport {

void setContext(const Context& context) {
    g_ctx = context;
}

const Context& context() {
    return g_ctx;
}

void setCurrentUserId(const string& userId) {
    g_currentUserId = userId;
}

const string& currentUserId() {
    return g_currentUserId;
}

void resetBuffers() {
    g_commandOutputBuffer.clear();
    g_shellResponseBuffer.clear();
}

void appendShellResponse(const string& text) {
    if (text.empty()) {
        return;
    }

    if (!g_shellResponseBuffer.empty()) {
        const char last = g_shellResponseBuffer.back();
        const char first = text.front();
        if (last != '\n' && last != '\r' && first != '\n' && first != '\r') {
            g_shellResponseBuffer += '\n';
        }
    }

    g_shellResponseBuffer += text;
}

const string& commandOutputBuffer() {
    return g_commandOutputBuffer;
}

const string& shellResponseBuffer() {
    return g_shellResponseBuffer;
}

string trimCopy(const string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });

    if (first == text.end()) {
        return "";
    }

    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    return string(first, last);
}

string stripOuterQuotes(const string& text) {
    string s = trimCopy(text);
    if (s.length() >= 2) {
        const bool doubleQuoted = (s.front() == '"' && s.back() == '"');
        const bool singleQuoted = (s.front() == '\'' && s.back() == '\'');
        if (doubleQuoted || singleQuoted) {
            s = s.substr(1, s.length() - 2);
        }
    }
    return s;
}

bool parseMacAddress(const string& text, uint8_t outMac[6]) {
    unsigned int bytes[6] = {0, 0, 0, 0, 0, 0};
    const string clean = stripOuterQuotes(text);

    if (std::sscanf(
            clean.c_str(),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            &bytes[0], &bytes[1], &bytes[2],
            &bytes[3], &bytes[4], &bytes[5]
        ) != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        outMac[i] = static_cast<uint8_t>(bytes[i]);
    }
    return true;
}

bool parseDateTimeText(const string& text, time_t& outEpoch) {
    const string clean = stripOuterQuotes(text);
    return ClockText::parse(clean, outEpoch);
}

uint8_t clampByte(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

void printLine(const string& text) {
    const string normalized = normalizeOutputTag(text);

    g_commandOutputBuffer += normalized;
    g_commandOutputBuffer += "\n";

    // In a BTP-protocolled session SerialMux is the port's only writer
    // (BTP/docs/session-and-terminal.md sections 3-4): a raw console
    // print here would interleave with COBS frames on the wire. Command
    // output during that session already reaches the caller through
    // ShellConfig::runLine's captured text (see SerialMux's COMMAND_REQUEST/
    // TERMINAL_IN handling), so it is simply not echoed to the port here.
    if (g_ctx.io != nullptr && SerialMux::isConsoleOwned()) {
        // Limpa a linha onde está o cursor (geralmente o prompt) antes de imprimir
        g_ctx.io->print("\r\033[K");
        ShellOutput::writeLine(*g_ctx.io, normalized.c_str());
    }

    if (g_ctx.lcdDashboard != nullptr && g_ctx.lcdDashboard->isReady()) {
        g_ctx.lcdDashboard->showMessage(String(normalized.c_str()), lcdColorForLine(normalized));
    }
}

uint8_t failWithCode(AppError::Code code, const string& detail) {
    char line[320] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "erro(%u/%s) %s",
        static_cast<unsigned>(AppError::value(code)),
        AppError::name(code),
        detail.c_str()
    );
    printLine(line);
    return RESULT_ERROR;
}

void warnWithCode(AppError::Code code, const string& detail) {
    char line[320] = {0};
    std::snprintf(
        line,
        sizeof(line),
        "aviso(%u/%s) %s",
        static_cast<unsigned>(AppError::value(code)),
        AppError::name(code),
        detail.c_str()
    );
    printLine(line);
}

} // namespace ShellCommandSupport
