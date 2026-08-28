#include "HubRelay.h"

#include "bally_channels.h"

namespace HubRelay {
RadioIngress classifyRadio(const std::uint8_t* datagram, std::size_t size) noexcept {
    RadioIngress ingress{};
    ingress.error = btp::Error::InvalidArgument;

    btp::DecodedFrame decoded{};
    const btp::Error error = btp::decode(datagram, size, btp::TransportProfile::EspNow, &decoded);
    ingress.error = error;
    if (error != btp::Error::Ok) {
        return ingress;
    }

    ingress.header = decoded.header;

    ingress.mayConsume = bally::dongle_may_consume(decoded.header.type, decoded.header.object_id);
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
