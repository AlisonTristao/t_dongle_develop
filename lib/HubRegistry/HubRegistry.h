#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief The binding table the downstream relay is addressed by:
 * `child_source_id -> peer_source_id`, eight entries, filled by an operator
 * (`hub -bind`) and never by anything on the wire.
 *
 * WHY A TABLE HAS TO EXIST AT ALL. A BTP header has no destination field --
 * only `source_id`, which says where a frame came from. For the upstream
 * direction that is enough, because the hub has exactly one place to send
 * anything (the cable). For the downstream direction it is not: several
 * children share one cable and each of them means a different robot. Some
 * message types carry the destination in their own payload
 * (COMMAND_REQUEST's `target_source_id`, MANIFEST_REQUEST's, SUBSCRIBE's),
 * but TELEMETRY and TERMINAL carry nothing of the sort, so for those the
 * dongle cannot derive the destination from the frame at all. It has to be
 * told, once, out of band. This is that "told".
 *
 * WHY IT IS NOT THE PEER TABLE. BtpTransport's peer cache
 * (rememberAuthenticatedPeer/lookupPeerMacBySourceId) answers a different
 * question --
 * "which MAC did I last hear source_id X from" -- and it is learned from
 * traffic. This one is an operator's declaration of intent and is learned
 * from a command. Merging them would make a robot able to redirect another
 * robot's downstream traffic by transmitting, which is exactly the property
 * the bind step exists to deny.
 *
 * A child_source_id is the identity of a device on the console side of the
 * hub -- one TraceView child process per robot, each with its own source_id
 * (topico 26). peer_source_id is the robot it was bound to. Both are plain
 * BTP source_ids; the display index a peer happens to have is never used
 * here (decision D8: "o canal e rotulo; o endereco e o source_id").
 *
 * Pure C++ over fixed-capacity static state, no dynamic allocation, same
 * shape as SubscriptionRegistry/ManifestCache/BtpTransport -- so it links and
 * is unit-testable under env:native (test/test_hub_relay).
 */
namespace HubRegistry {

// Eight is the console side's realistic ceiling: one child process per robot
// on one cable, against a peer table (BtpTransport::kPeerIdentityCapacity)
// twice that size because the radio also hears robots nobody bound yet.
constexpr std::size_t kMaxBindings = 8U;

struct Binding {
    std::uint32_t childSourceId;
    std::uint32_t peerSourceId;
};

/**
 * @brief Binds (or re-binds) one child to one peer. Re-binding an already
 * bound child replaces its peer in place rather than consuming a second slot:
 * an operator pointing a channel at a different robot is the normal way this
 * table changes, and it must not be able to exhaust it by repeating.
 *
 * Returns false when either id is zero (BTP reserves source_id 0) or when the
 * table is full and this child is not already in it.
 */
bool bind(std::uint32_t childSourceId, std::uint32_t peerSourceId) noexcept;

/** Removes a binding. False when that child was not bound. */
bool unbind(std::uint32_t childSourceId) noexcept;

/** Resolves the peer a child is bound to. False when unbound -- which is the
 * signal to NOT relay downstream, never a reason to guess a destination. */
bool lookup(std::uint32_t childSourceId, std::uint32_t* outPeerSourceId) noexcept;

/** Copies every binding into `out`, in slot order, returning how many were
 * written. Slot order is fill order and resets on every boot; it is a display
 * order, not an identity (same warning as BtpTransport::enumeratePeers). */
std::size_t enumerate(Binding* out, std::size_t maxOut) noexcept;

std::size_t count() noexcept;

/** Drops every binding. Exists for the test suite and for a future
 * "session ended, forget the console's children" caller. */
void clear() noexcept;

}  // namespace HubRegistry
