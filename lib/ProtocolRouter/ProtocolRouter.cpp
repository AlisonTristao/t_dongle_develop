#include "ProtocolRouter.h"

#include <cstring>

namespace ProtocolRouter {
namespace {

std::array<btp::ReassemblyStorage, kSlotCount> makeStorageViews(
    std::uint8_t (&storage)[kSlotCount][kMaxPayloadSize]) noexcept {
    std::array<btp::ReassemblyStorage, kSlotCount> views{};
    for (std::size_t index = 0U; index < kSlotCount; ++index) {
        views[index] = btp::ReassemblyStorage{storage[index], kMaxPayloadSize};
    }
    return views;
}

void fillRouted(const std::uint8_t mac[6],
                const btp::Header& header,
                btp::ByteView payload,
                std::uint64_t nowMs,
                RoutedMessage* out) noexcept {
    std::memcpy(out->mac, mac, 6U);
    out->header = header;
    out->payloadSize = payload.size;
    if (payload.size > 0U) {
        std::memcpy(out->payload, payload.data, payload.size);
    }
    out->arrivalMs = nowMs;
}

}  // namespace

Router::Router() noexcept
    : slots_(),
      storage_(),
      storageViews_(makeStorageViews(storage_)),
      reassembler_(slots_, storageViews_.data(), kSlotCount, kReassemblyTimeoutMs) {}

Outcome Router::submit(const std::uint8_t mac[6],
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint64_t nowMs,
                       RoutedMessage* outMessage) noexcept {
    if (mac == nullptr || data == nullptr || size == 0U || outMessage == nullptr) {
        ++stats_.droppedInvalidArgument;
        return Outcome::DroppedInvalidArgument;
    }

    btp::DecodedFrame decoded{};
    const btp::Error decodeError = btp::decode(data, size, btp::TransportProfile::EspNow, &decoded);
    if (decodeError != btp::Error::Ok) {
        if (decodeError == btp::Error::CrcMismatch) {
            ++stats_.droppedCrc;
            return Outcome::DroppedCrc;
        }
        ++stats_.droppedDecode;
        return Outcome::DroppedDecode;
    }

    const bool fragmented = (decoded.header.flags & btp::kFlagFragmented) != 0U;
    if (!fragmented) {
        // decode()'s validate_header() already requires fragment_index==0 and
        // fragment_count==1 here, so this datagram already is the whole
        // logical message; its payload only points into the caller's
        // transient RX buffer, so copy it out now.
        fillRouted(mac, decoded.header, decoded.payload, nowMs, outMessage);
        ++stats_.routed;
        return Outcome::Routed;
    }

    btp::ReassembledMessage completed{};
    const btp::ReassemblyEvent event = reassembler_.push(decoded, nowMs, &completed);
    switch (event) {
        case btp::ReassemblyEvent::Accepted:
            ++stats_.fragmentsAccepted;
            return Outcome::FragmentAccepted;
        case btp::ReassemblyEvent::Duplicate:
            ++stats_.duplicateFragments;
            return Outcome::DuplicateFragment;
        case btp::ReassemblyEvent::Complete:
            fillRouted(mac, completed.header, completed.payload, nowMs, outMessage);
            // Release right away: queueing the *copy* downstream, not the
            // slot, keeps the small reassembly pool free for other sources
            // (e.g. two robots fragmenting at once) even if a per-type queue
            // is momentarily full.
            reassembler_.release(completed.slot_index);
            ++stats_.routed;
            return Outcome::Routed;
        case btp::ReassemblyEvent::InvalidFragment:
        case btp::ReassemblyEvent::Conflict:
        case btp::ReassemblyEvent::MessageTooLarge:
        case btp::ReassemblyEvent::NoSlot:
            ++stats_.droppedReassembly;
            return Outcome::DroppedReassembly;
        case btp::ReassemblyEvent::InvalidArgument:
            ++stats_.droppedInvalidArgument;
            return Outcome::DroppedInvalidArgument;
    }
    ++stats_.droppedInvalidArgument;
    return Outcome::DroppedInvalidArgument;
}

Stats Router::stats() const noexcept {
    return stats_;
}

}  // namespace ProtocolRouter
