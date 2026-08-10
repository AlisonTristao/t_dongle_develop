#pragma once

#include <btp/codec.hpp>
#include <btp/fragmentation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * @brief Decodes BTP datagrams (bytes + size) into routed, type-tagged
 * logical messages: btp::decode() (envelope + CRC-32) then, only for
 * fragmented frames, btp::Reassembler. Everything here is plain C++ with no
 * Arduino/FreeRTOS dependency, so it runs the same way under env:native
 * against bally_protocol's canonical test vectors as it does on-device.
 *
 * Ownership: the "queue by type, drain from tasks/tick()" part (step 10 of
 * topico 12) is Arduino-only and lives one layer up, in EspNowConfig, which
 * calls submit() and pushes the resulting RoutedMessage into its own
 * FreeRTOS queues. This class only answers "what logical message, if any,
 * did this datagram complete" -- it has no queues or tasks of its own.
 */
namespace ProtocolRouter {

// Bounded to BtpTransport::kMaxLogicalPayloadSize's max real user, a
// fragmented COMMAND_REQUEST (20-byte prefix + up to 512 shell bytes).
constexpr std::size_t kMaxPayloadSize = 600U;
constexpr std::size_t kSlotCount = 4U;
constexpr std::uint64_t kReassemblyTimeoutMs = 4000U;

enum class Outcome : std::uint8_t {
    Routed,           // A complete logical message is in *outMessage.
    FragmentAccepted, // One fragment of a still-incomplete message was stored.
    DuplicateFragment,
    DroppedDecode,      // btp::decode() failed for a reason other than CRC.
    DroppedCrc,
    DroppedReassembly,  // Conflict/InvalidFragment/MessageTooLarge/NoSlot.
    DroppedInvalidArgument,
};

struct RoutedMessage {
    std::uint8_t mac[6];
    btp::Header header;
    std::uint8_t payload[kMaxPayloadSize];
    std::size_t payloadSize;
    std::uint64_t arrivalMs;
};

struct Stats {
    std::uint32_t routed;
    std::uint32_t fragmentsAccepted;
    std::uint32_t duplicateFragments;
    std::uint32_t droppedDecode;
    std::uint32_t droppedCrc;
    std::uint32_t droppedReassembly;
    std::uint32_t droppedInvalidArgument;
};

/**
 * @brief One receiver's worth of decode+reassembly state. Multiple sources
 * fragmenting concurrently (two robots at once) share the slot pool below
 * without stepping on each other, since btp::Reassembler keys slots by
 * (source_id, boot_id, sequence); each Router instance is single-consumer
 * (call submit() from one task/context).
 */
class Router {
public:
    Router() noexcept;

    /**
     * @brief Feeds one received datagram. On Outcome::Routed, *outMessage is
     * populated with the complete logical message (payload copied out and,
     * if it came from reassembly, the slot released immediately -- so a
     * queue-full condition downstream never starves the small reassembly
     * pool). mac is copied verbatim into the result; arrivalMs is stored as
     * local metadata only, never merged into header.timestamp_us.
     */
    Outcome submit(const std::uint8_t mac[6],
                  const std::uint8_t* data,
                  std::size_t size,
                  std::uint64_t nowMs,
                  RoutedMessage* outMessage) noexcept;

    Stats stats() const noexcept;

private:
    btp::ReassemblySlot slots_[kSlotCount];
    std::uint8_t storage_[kSlotCount][kMaxPayloadSize];
    std::array<btp::ReassemblyStorage, kSlotCount> storageViews_;
    btp::Reassembler reassembler_;

    Stats stats_{};
};

}  // namespace ProtocolRouter
