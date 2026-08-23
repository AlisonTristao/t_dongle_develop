#include "HubRelay.h"

#include "bally_channels.h"

namespace HubRelay {
namespace {

// COMMAND_RESULT's reference prefix starts with request_source_id, LE u32
// (BTP/docs/commands.md section 1; BtpTransport::btp_command::build_result
// writes it at offset 0 of kResultPrefixSize). Redeclared here as an offset
// rather than included, so this library stays free of BtpTransport (which
// would drag the identity/sequence state into a module whose whole point is
// not to touch it).
constexpr std::size_t kResultRequestSourceIdOffset = 0U;
constexpr std::size_t kResultRequestSourceIdSize = 4U;

std::uint32_t readU32Le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

}  // namespace

RadioIngress classifyRadio(const std::uint8_t* datagram,
                           std::size_t size,
                           std::uint32_t selfSourceId) noexcept {
    RadioIngress ingress{};
    ingress.error = btp::Error::InvalidArgument;

    btp::DecodedFrame decoded{};
    const btp::Error error = btp::decode(datagram, size, btp::TransportProfile::EspNow, &decoded);
    ingress.error = error;
    if (error != btp::Error::Ok) {
        return ingress;
    }

    ingress.header = decoded.header;

    // The source_id a COMMAND names sits at offset 0 of its payload in both
    // directions -- target_source_id for a request, request_source_id for a
    // result (BtpTransport::btp_command::parse_request/parse_result). One read
    // therefore serves both, which is what lets the ingress rule be "a COMMAND
    // addressed to me" rather than two separate cases.
    //
    // Only a first fragment (or an unfragmented frame) carries that prefix; a
    // later fragment of a long command leaves this at zero and is therefore
    // relayed. That asymmetry is harmless in practice -- the commands and
    // results this dongle exchanges are short -- and it fails toward the
    // console, which is the direction D6 chose to fail in.
    if (decoded.header.type == btp::MessageType::Command &&
        (decoded.header.object_id == bally::kCommandRequestObjectId ||
         decoded.header.object_id == bally::kCommandResultObjectId) &&
        decoded.header.fragment_index == 0U &&
        decoded.payload.data != nullptr &&
        decoded.payload.size >= kResultRequestSourceIdOffset + kResultRequestSourceIdSize) {
        ingress.requestSourceId = readU32Le(decoded.payload.data + kResultRequestSourceIdOffset);
    }

    ingress.consume = bally::dongle_consumes(decoded.header.type, decoded.header.object_id,
                                             ingress.requestSourceId, selfSourceId);
    return ingress;
}

bool reencodeVerbatim(const btp::DecodedFrame& decoded,
                      btp::TransportProfile transport,
                      std::uint8_t* out,
                      std::size_t outCapacity,
                      std::size_t* bytesWritten) noexcept {
    // decoded.header is copied whole -- type, flags (FRAGMENTED, ENCRYPTED
    // and CIPHER_ID included), source_id, boot_id, sequence, timestamp_us,
    // object_id, fragment_index, fragment_count. Nothing is recomputed except
    // the CRC, which is a function of the octets themselves.
    const btp::Frame frame{decoded.header, decoded.payload};
    return btp::encode(frame, transport, out, outCapacity, bytesWritten) == btp::Error::Ok;
}

}  // namespace HubRelay
