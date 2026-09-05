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
    : transportInit_(*this),
      slots_(),
      storage_(),
      storageViews_(makeStorageViews(storage_)),
      rxBuffer_(),
      node_(*this, slots_, storageViews_.data(), kSlotCount, kReassemblyTimeoutMs,
            rxBuffer_, kMaxPayloadSize,
            /*seal_scratch=*/nullptr, 0U, /*open_buffer=*/nullptr, 0U,
            /*scratch_buffer=*/nullptr, 0U) {}

Outcome Router::submit(const std::uint8_t mac[6],
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint64_t nowMs,
                       RoutedMessage* outMessage) noexcept {
    if (mac == nullptr || data == nullptr || size == 0U || outMessage == nullptr) {
        // btp::Node reports InvalidArgument (via receive_outcome()) on a
        // null datagram too, but it needs a non-null message_out to be
        // handed one, and it has no notion of mac; short-circuit.
        return Outcome::DroppedInvalidArgument;
    }

    btp::ReceivedMessage received{};
    const btp::NodeRx rx = node_.receive(data, size, nowMs, &received);

    if (rx == btp::NodeRx::Complete) {
        // mac and arrivalMs are caller metadata the library never sees; the
        // header and the reassembled payload come from btp::Node's own
        // rx_buffer_ (which released the reassembly slot already, if any) --
        // copied into outMessage's own storage, since that buffer stops
        // being valid at the next receive().
        std::memcpy(outMessage->mac, mac, 6U);
        outMessage->header = received.header;
        outMessage->payloadSize = received.payload.size;
        std::memcpy(outMessage->payload, received.payload.data, received.payload.size);
        outMessage->arrivalMs = nowMs;
    }
    // receive_outcome() is the exact btp::ReceiveOutcome NodeRx rounded off
    // (library 2.43.0, added for this call site): every value Router's own
    // Outcome distinguishes -- including which of the two Pending reasons or
    // three DroppedFrame reasons this was -- survives unchanged.
    return mapOutcome(node_.receive_outcome());
}

Stats Router::stats() const noexcept {
    const btp::Receiver::Stats s = node_.receiver().stats();
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
