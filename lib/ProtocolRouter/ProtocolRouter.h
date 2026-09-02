#pragma once

#include <btp/codec.hpp>
#include <btp/receiver.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * @brief Decodes BTP datagrams (bytes + size) into routed, type-tagged
 * logical messages. The decode + CRC + reassembly core is btp::Receiver
 * (BTP >= 2.8.0); this class is the thin dongle-side wrapper that adds the
 * two pieces of caller metadata the library layer has no notion of -- the
 * sender's MAC and the local arrival instant -- and keeps the
 * ProtocolRouter:: API the rest of the firmware was written against.
 * Everything here is plain C++ with no Arduino/FreeRTOS dependency, so it
 * runs the same way under env:native against BTP's canonical test vectors as
 * it does on-device.
 *
 * Ownership: the "queue by type, drain from tasks/tick()" part (step 10 of
 * topico 12) is Arduino-only and lives one layer up, in EspNowConfig, which
 * calls submit() and pushes the resulting RoutedMessage into its own
 * FreeRTOS queues. This class only answers "what logical message, if any,
 * did this datagram complete" -- it has no queues or tasks of its own.
 */
namespace ProtocolRouter {

// Bounded to BtpTransport::kMaxLogicalPayloadSize's max real user, a
// fragmented COMMAND_REQUEST (20-byte prefix + up to 512 shell bytes), PLUS
// btp::aead's 16-octet tag: everything this router reassembles is channel C
// (dongle<->robot, key L) as of topico 30, so a message sealed at
// BtpTransport::kMaxLogicalPayloadSize (600) is 616 octets on the wire, and
// this buffer has to hold that whole, still-sealed message before
// RadioSeal::open() gets a chance to shrink it back down -- opening happens
// AFTER reassembly, never before (BTP/docs/encryption.md section 6). No real
// caller sends a 600-octet plaintext today (532 is the largest, a full-size
// shell command), but the ceiling is meant to be an honest upper bound, not
// one that only happens to work for today's traffic.
constexpr std::size_t kMaxPayloadSize = 616U;
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
    // storageViews_ is declared before receiver_ so member initialization
    // order hands the btp::Receiver constructor a fully built view table;
    // reordering these two would pass it uninitialized pointers.
    btp::ReassemblySlot slots_[kSlotCount];
    std::uint8_t storage_[kSlotCount][kMaxPayloadSize];
    std::array<btp::ReassemblyStorage, kSlotCount> storageViews_;
    btp::Receiver receiver_;
};

}  // namespace ProtocolRouter
