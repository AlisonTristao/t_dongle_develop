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

Outcome mapOutcome(btp::ReceiveOutcome outcome) noexcept {
    switch (outcome) {
        case btp::ReceiveOutcome::Complete: return Outcome::Routed;
        case btp::ReceiveOutcome::FragmentAccepted: return Outcome::FragmentAccepted;
        case btp::ReceiveOutcome::DuplicateFragment: return Outcome::DuplicateFragment;
        case btp::ReceiveOutcome::DroppedCrc: return Outcome::DroppedCrc;
        case btp::ReceiveOutcome::DroppedDecode: return Outcome::DroppedDecode;
        case btp::ReceiveOutcome::DroppedReassembly: return Outcome::DroppedReassembly;
        case btp::ReceiveOutcome::InvalidArgument: return Outcome::DroppedInvalidArgument;
    }
    return Outcome::DroppedInvalidArgument;
}

}  // namespace

Router::Router() noexcept
    : slots_(),
      storage_(),
      storageViews_(makeStorageViews(storage_)),
      receiver_(slots_, storageViews_.data(), kSlotCount, kReassemblyTimeoutMs,
                btp::TransportProfile::EspNow) {}

Outcome Router::submit(const std::uint8_t mac[6],
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint64_t nowMs,
                       RoutedMessage* outMessage) noexcept {
    if (mac == nullptr || data == nullptr || size == 0U || outMessage == nullptr) {
        // btp::Receiver reports InvalidArgument on a null datagram too, but it
        // needs a non-null message_out to be handed one, and it has no notion
        // of mac; short-circuit.
        return Outcome::DroppedInvalidArgument;
    }

    btp::ReceivedMessage received{};
    const btp::ReceiveOutcome outcome = receiver_.submit(
        data, size, nowMs, outMessage->payload, kMaxPayloadSize, &received);

    if (outcome == btp::ReceiveOutcome::Complete) {
        // mac and arrivalMs are caller metadata the library never sees; the
        // header and the reassembled payload copy come from btp::Receiver
        // (which released the slot already). arrivalMs is stored here only,
        // never merged into header.timestamp_us.
        std::memcpy(outMessage->mac, mac, 6U);
        outMessage->header = received.header;
        outMessage->payloadSize = received.payload.size;
        outMessage->arrivalMs = nowMs;
    }
    return mapOutcome(outcome);
}

Stats Router::stats() const noexcept {
    const btp::Receiver::Stats s = receiver_.stats();
    Stats snapshot{};
    snapshot.routed = s.completed;
    snapshot.fragmentsAccepted = s.fragments_accepted;
    snapshot.duplicateFragments = s.duplicate_fragments;
    snapshot.droppedDecode = s.dropped_decode;
    snapshot.droppedCrc = s.dropped_crc;
    snapshot.droppedReassembly = s.dropped_reassembly;
    snapshot.droppedInvalidArgument = s.invalid_argument;
    return snapshot;
}

}  // namespace ProtocolRouter
