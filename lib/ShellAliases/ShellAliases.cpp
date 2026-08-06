#include "ShellAliases.h"

#include <cctype>

namespace {

using std::string;

// Add new short aliases here; each maps to a full "<module> -<command>" prefix.
constexpr ShellAliases::Entry kAliases[] = {
    {"es", "espnow -send_to"},
};

constexpr size_t kAliasCount = sizeof(kAliases) / sizeof(kAliases[0]);

size_t firstTokenLength(const string& line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0) {
        ++i;
    }
    return i;
}

} // namespace

namespace ShellAliases {

std::string resolve(const std::string& line) {
    const size_t tokenLen = firstTokenLength(line);
    if (tokenLen == 0) {
        return line;
    }

    const string token = line.substr(0, tokenLen);
    for (size_t i = 0; i < kAliasCount; ++i) {
        if (token == kAliases[i].alias) {
            return string(kAliases[i].expandsTo) + line.substr(tokenLen);
        }
    }

    return line;
}

const Entry* entries() {
    return kAliases;
}

size_t count() {
    return kAliasCount;
}

} // namespace ShellAliases
