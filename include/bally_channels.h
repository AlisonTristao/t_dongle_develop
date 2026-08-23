#ifndef BALLY_CHANNELS_H
#define BALLY_CHANNELS_H

// The single table that answers "whose message is this, and which key opens
// it".
//
// THREE BYTE-IDENTICAL COPIES of this file exist -- one in bally_OS, one in
// bally_dongle, one in TraceView -- and each repository has a test that
// hashes the file and compares it against a committed constant. A copy that
// drifts therefore breaks all three builds at once, instead of breaking one
// channel in silence months later.
//
// EDITING RULE: change it in one repository, copy it verbatim into the other
// two, and update the expected hash in all three tests. There is no partial
// edit of this file that is correct.
//
// Why this file is product and not protocol: BTP declares key provisioning
// out of scope, and its CIPHER_ID sub-field picks the *cipher*, never the
// *key* -- there is no key-id field on the wire at all. "This source_id uses
// key L" is true only because the three ends agree on it. This file is that
// agreement written down once. Without it, the first device added to the
// fleet breaks in silence, which is the failure mode this whole file exists
// to prevent.

#include <btp/codec.hpp>

#include <cstdint>

namespace bally {

// Bumped whenever the meaning of anything below changes in a way the other
// two ends must be rebuilt for. Not a wire field -- nothing here travels.
static const std::uint8_t kChannelContractVersion = 2U;

// ---------------------------------------------------------------------------
// The three channels
// ---------------------------------------------------------------------------
// A channel is defined by WHICH TWO ENDS talk, never by what kind of message
// travels on it:
//
//   A_Console   TraceView <-> dongle    in the clear
//   B_Endpoint  TraceView <-> robot     key E, one per robot
//   C_Link      dongle    <-> robot     key L, one per fleet
//
// The question that decides any new message is "who are the two ends". There
// is no fourth combination, and no message may belong to two channels.
//
// Asking "what type of message is this" instead cannot work: once all three
// channels exist the same MessageType lives on two of them (a COMMAND can
// come from TraceView or from the dongle), so the type decides nothing.
// Identity decides, and source_id sits in the clear at a fixed header offset
// in every frame -- including a frame whose payload is sealed. That is what
// makes this rule survive: any message added later is already born with the
// right key, because whoever sends it already has an identity.
enum class Channel : std::uint8_t {
    A_Console = 0U,
    B_Endpoint = 1U,
    C_Link = 2U
};

// Which key opens a channel.
//
// Deliberately NOT the same enum as Channel: the mapping is many-to-one
// (A_Console takes no key at all) and the robot holds exactly two keys, so
// collapsing the two types would let "which channel" and "which key" be
// confused at exactly the call sites where the difference is the whole point.
//
// The two passwords behind Endpoint and Link are independent on purpose. If
// both keys were derived from a single password by domain separation, whoever
// held that password would hold both keys and the channels would collapse
// into one.
enum class KeyKind : std::uint8_t {
    None,      // A_Console: in the clear
    Endpoint,  // key E, one per robot -- protects data and commands
    Link       // key L, one per fleet -- administers the radio link only
};

constexpr KeyKind key_of_channel(Channel channel) noexcept {
    return channel == Channel::A_Console
               ? KeyKind::None
               : (channel == Channel::B_Endpoint ? KeyKind::Endpoint
                                                 : KeyKind::Link);
}

// ---------------------------------------------------------------------------
// Which channel a received frame belongs to
// ---------------------------------------------------------------------------
// The two ends that can answer from source_id alone. The hub cannot (see
// dongle_channel_of below), which is why it is not a third value here.
enum class Vantage : std::uint8_t {
    Console,  // TraceView: the other end is either the dongle or a robot
    Robot     // bally_OS: the other end is either the dongle or TraceView
};

// `dongle_source_id` is the hub's identity, which every end already learns
// without a new message: the console from the parent device it connected
// through, the robot from whichever peer administers its link, the dongle
// from itself.
//
// Note that a peer that is not the dongle is B_Endpoint from BOTH vantages,
// and that this is not a coincidence -- B is the one channel whose two ends
// are the console and a robot, so each of them sees the other across it.
constexpr Channel channel_of_peer(Vantage self, std::uint32_t peer_source_id,
                                  std::uint32_t dongle_source_id) noexcept {
    return peer_source_id == dongle_source_id
               ? (self == Vantage::Robot ? Channel::C_Link
                                         : Channel::A_Console)
               : Channel::B_Endpoint;
}

// Which physical link a frame reached the dongle on.
enum class Side : std::uint8_t { Cable, Radio };

// The hub's side of the same question. It needs the arrival side because
// source_id alone cannot answer it: a frame arriving from the radio is
// C_Link if the dongle consumes it and B_Endpoint merely passing through if
// it does not, and both carry the same robot as source. dongle_consumes()
// below is that decision, so the two functions compose:
//
//     dongle_channel_of(Side::Radio, dongle_consumes(...))
//
// B_Endpoint here means "the dongle relays this and cannot read it" -- the
// hub holds key L, never any robot's key E. Whoever holds the dongle
// administers the link but reads nothing.
constexpr Channel dongle_channel_of(Side side, bool consumed) noexcept {
    return side == Side::Cable
               ? Channel::A_Console
               : (consumed ? Channel::C_Link : Channel::B_Endpoint);
}

// ---------------------------------------------------------------------------
// What the dongle consumes, and therefore what it relays
// ---------------------------------------------------------------------------
// BTP wire constants (docs/commands.md section 1, "object_id namespaces").
// Redeclared here rather than included from any repository's own header,
// because this file has to stay byte-identical in three repositories and so
// cannot include a repo-local one. They are fixed wire values; if one of
// these ever disagrees with its counterpart in a repository's own header,
// that header is the one that is wrong.
static const std::uint16_t kCommandRequestObjectId = 0x0001U;
static const std::uint16_t kCommandResultObjectId = 0x0002U;
static const std::uint16_t kStatusObjectId = 0x0009U;

// The radio ingress rule, inverted from what a cable would do: EVERYTHING
// goes up to the console except this short, explicit list.
//
// Relaying by default is the right rule for a hub, and it also fixes a class
// of bug that predates the hub: a handler left empty swallows a whole
// MessageType in silence, and nobody notices until someone asks why the
// robot's terminal output never arrives. With this rule, forgetting to add
// something to the list makes it arrive at the console -- visible, and
// harmless -- instead of disappearing.
//
// The list has exactly two entries, and the second one is a single idea
// rather than a pair of cases: a COMMAND naming this dongle is this dongle's
// business, whichever direction it travels. A COMMAND_REQUEST addressed to it
// is one it should run; a COMMAND_RESULT answering a request it issued is one
// it asked for. Both stop here; a COMMAND naming anyone else is relayed.
//
// Writing it as "addressed to me" rather than as two separate rules is what
// keeps a capability from quietly disappearing: an earlier version listed only
// the result, and the consequence was that a command sent to this dongle over
// the radio stopped being executed while a desktop session was attached --
// with nothing reporting why.
//
// `command_peer_source_id` is the source_id the COMMAND names, and it sits at
// offset 0 of the payload either way: `target_source_id` for a request, and
// `request_source_id` for a result. Pass 0 for every other message type.
//
// heartbeat and presence are one entry, not two: in this firmware both ride
// CONTROL/STATUS (object_id 0x0009), which the dongle originates toward a
// peer and whose answer is what lights the link tile. If presence ever gets
// an object_id of its own, it belongs in this list and nowhere else.
constexpr bool dongle_consumes(btp::MessageType type, std::uint16_t object_id,
                               std::uint32_t command_peer_source_id,
                               std::uint32_t self_source_id) noexcept {
    return (type == btp::MessageType::Control &&
            object_id == kStatusObjectId) ||
           (type == btp::MessageType::Command &&
            (object_id == kCommandRequestObjectId ||
             object_id == kCommandResultObjectId) &&
            command_peer_source_id == self_source_id && self_source_id != 0U);
}

}  // namespace bally

#endif  // BALLY_CHANNELS_H
