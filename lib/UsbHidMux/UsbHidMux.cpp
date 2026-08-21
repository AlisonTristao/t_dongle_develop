#include "UsbHidMux.h"

#include <Arduino.h>
#include <USBHIDVendor.h>

namespace UsbHidMux {
namespace {

// One report's worth of data octets (excludes the Report ID byte, which
// USBHIDVendor/hidapi handle on their own side of the wire).
constexpr size_t kReportDataSize = 63;

// prepend_size=true reserves buffer[0] of every report as an explicit valid-
// length prefix -- a fixed-size HID report always transmits all
// kReportDataSize octets, zero-padded by the USB stack when a write is
// shorter, so without this the receiver has no way to tell real data from
// padding. USBHIDVendor::write() applies this automatically on the way out;
// _onOutput()/read() do not do the equivalent on the way in (see below), so
// this module has to. See bally_protocol/docs/TRANSPORT_USB_HID.md section 2
// and BTP's ADR 0011 -- the real BTP integration will rely on this same
// convention, so the bring-up spike already exercises it end to end instead
// of a differently-framed byte echo.
USBHIDVendor vendor(kReportDataSize, /*prepend_size=*/true);
bool started = false;

} // namespace

void begin() {
    vendor.begin();
    started = true;
}

void tick() {
    if (!started) {
        return;
    }
    // _onOutput() pushes a received report's octets into a flat byte queue
    // with no report-boundary marker of its own -- reading exactly one
    // report's worth at a time keeps this aligned, relying on each report's
    // bytes landing in the queue atomically (a single, uninterrupted
    // _onOutput() call per report; nothing else drains this queue).
    if (static_cast<size_t>(vendor.available()) < kReportDataSize) {
        return;
    }
    uint8_t buffer[kReportDataSize];
    const size_t got = vendor.readBytes(buffer, sizeof(buffer));
    if (got != sizeof(buffer)) {
        return;
    }
    // buffer[0] is the host's own valid-length prefix (see kReportDataSize
    // above); clamp defensively instead of trusting an out-of-range value.
    const uint8_t validLength = buffer[0] > (kReportDataSize - 1) ? (kReportDataSize - 1) : buffer[0];
    if (validLength > 0) {
        // prepend_size=true re-adds the length prefix on the way out --
        // pass only the real payload, not buffer[0] itself.
        vendor.write(buffer + 1, validLength);
    }
}

} // namespace UsbHidMux
