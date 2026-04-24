#pragma once

#include <stdint.h>

// Keep this file synchronized with bally_software/include/SharedMessageTypes.h.
enum class logType : uint8_t {
    NONE = 0,
    INFO,
    CMDO,
    TELE,
    ERRO,
    DEBG
};
