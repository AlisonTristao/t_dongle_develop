#pragma once

#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

/**
 * @brief The two pure decisions the hub's relay is built from: what a radio
 * datagram is (consumed or passed through), and how a frame is put back on
 * the wire without being re-originated.
 *
 * Neither function touches Arduino, FreeRTOS, a queue or a radio -- the glue
 * that does lives in EspNowConfig (ingress) and SerialMux (egress). What is
 * here is what has to be provable on a host: the ingress rule and, above all,
 * the promise that a relayed frame keeps the producer's identity triple.
 *
 * D5 (topico 28): THE RELAY REASSEMBLES NOTHING. A datagram is passed on
 * fragment by fragment, exactly as it arrived; the ends reassemble
 * (TraceView's BtpSession already has btp::Reassembler, the robot gets one in
 * this same phase). ProtocolRouter leaves the relay path entirely and stays
 * only for what the dongle actually consumes.
 */
namespace HubRelay {

/**
 * @brief What one raw radio datagram turned out to be.
 *
 * Only the 36-octet envelope is read. That is enough for every decision
 * below and stays true for a sealed frame, because BTP encrypts the payload
 * and never the header (docs/encryption.md section 5: the header is the AAD,
 * authenticated in the clear).
 */
struct RadioIngress {
    /** btp::decode()'s verdict. Anything other than Ok means the other fields
     * are meaningless and the datagram is a drop, not a relay. */
    btp::Error error;
    btp::Header header;
    /** COMMAND_RESULT only: the source_id of the request being answered, read
     * out of the result's own reference prefix. Zero for every other type,
     * and zero as well for a COMMAND_RESULT that arrived as a non-first
     * fragment (the prefix is not in this fragment) -- which resolves to
     * "relay", the safe default under D6. */
    std::uint32_t requestSourceId;
    /** bally::dongle_consumes()' verdict. False means relay upstream. */
    bool consume;
};

/**
 * @brief Reads the envelope of one raw ESP-NOW datagram and applies the
 * ingress rule of bally_channels.h.
 *
 * The rule is inverted from what a cable would do: EVERYTHING goes up to the
 * console except the short, explicit list in
 * bally::dongle_consumes(). This function never writes a second copy of that
 * list -- it only supplies the four arguments the one true list asks for.
 */
RadioIngress classifyRadio(const std::uint8_t* datagram,
                           std::size_t size,
                           std::uint32_t selfSourceId) noexcept;

/**
 * @brief Serializes an already-decoded frame back onto a transport profile
 * with every header field copied from the producer, including the three that
 * make up the AEAD nonce.
 *
 * THIS IS THE FUNCTION THE HOUSE STYLE WOULD GET WRONG. Everything else in
 * this firmware originates its messages through BtpTransport::sendLogical,
 * which reserves a fresh sequence and stamps this dongle's own
 * source_id/boot_id. Doing that to a frame merely passing through rewrites
 * `source_id || boot_id || sequence`, and that triple IS the AEAD nonce
 * (docs/encryption.md section 4). The seal then fails to open at the far end,
 * two repositories away from the line that broke it.
 *
 * Re-fragmenting, by contrast, would be SAFE for the seal, and it is worth
 * being precise about that because the opposite belief leads to bad decisions
 * later: the AAD is the canonicalized LOGICAL header -- FRAGMENTED cleared,
 * fragment_index 0, fragment_count 1, payload_size of the whole message -- so
 * fragment_index, fragment_count and the FRAGMENTED flag are deliberately
 * outside the tag, precisely so a gateway can re-cut a message it cannot read
 * (see the `description` field of BTP's aead_fragmented_gcm_0 vector). The
 * dongle still never re-fragments, but the reason is throughput and
 * retransmission, not the seal: a 4056-octet serial message re-cut to the
 * radio's 210 would become 20 fragments with no retransmission behind them,
 * so the child encodes on the EspNow profile from the origin instead (D4).
 *
 * Because every header field and the payload are copied verbatim and the CRC
 * is a function of both, the output is octet-for-octet the frame that
 * arrived, as long as the target profile admits its size.
 */
bool reencodeVerbatim(const btp::DecodedFrame& decoded,
                      btp::TransportProfile transport,
                      std::uint8_t* out,
                      std::size_t outCapacity,
                      std::size_t* bytesWritten) noexcept;

}  // namespace HubRelay
