#include "HubRegistry.h"

namespace HubRegistry {
namespace {

// childSourceId == 0 marks a free slot, which is free of ambiguity because
// BTP reserves source_id 0 and bind() refuses it.
Binding g_bindings[kMaxBindings] = {};

int findSlot(std::uint32_t childSourceId) noexcept {
    for (std::size_t i = 0U; i < kMaxBindings; ++i) {
        if (g_bindings[i].childSourceId == childSourceId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

bool bind(std::uint32_t childSourceId, std::uint32_t peerSourceId) noexcept {
    if (childSourceId == 0U || peerSourceId == 0U) {
        return false;
    }

    const int existing = findSlot(childSourceId);
    if (existing >= 0) {
        g_bindings[existing].peerSourceId = peerSourceId;
        return true;
    }

    const int free = findSlot(0U);
    if (free < 0) {
        return false;
    }
    g_bindings[free].childSourceId = childSourceId;
    g_bindings[free].peerSourceId = peerSourceId;
    return true;
}

bool unbind(std::uint32_t childSourceId) noexcept {
    if (childSourceId == 0U) {
        return false;
    }
    const int slot = findSlot(childSourceId);
    if (slot < 0) {
        return false;
    }
    g_bindings[slot].childSourceId = 0U;
    g_bindings[slot].peerSourceId = 0U;
    return true;
}

bool lookup(std::uint32_t childSourceId, std::uint32_t* outPeerSourceId) noexcept {
    if (childSourceId == 0U || outPeerSourceId == nullptr) {
        return false;
    }
    const int slot = findSlot(childSourceId);
    if (slot < 0) {
        return false;
    }
    *outPeerSourceId = g_bindings[slot].peerSourceId;
    return true;
}

std::size_t enumerate(Binding* out, std::size_t maxOut) noexcept {
    if (out == nullptr) {
        return 0U;
    }
    std::size_t written = 0U;
    for (std::size_t i = 0U; i < kMaxBindings && written < maxOut; ++i) {
        if (g_bindings[i].childSourceId != 0U) {
            out[written++] = g_bindings[i];
        }
    }
    return written;
}

std::size_t count() noexcept {
    std::size_t total = 0U;
    for (std::size_t i = 0U; i < kMaxBindings; ++i) {
        if (g_bindings[i].childSourceId != 0U) {
            ++total;
        }
    }
    return total;
}

void clear() noexcept {
    for (std::size_t i = 0U; i < kMaxBindings; ++i) {
        g_bindings[i].childSourceId = 0U;
        g_bindings[i].peerSourceId = 0U;
    }
}

}  // namespace HubRegistry
