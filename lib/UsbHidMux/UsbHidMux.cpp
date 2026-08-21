#include "UsbHidMux.h"

#include <Arduino.h>
#include <USBHIDVendor.h>

namespace UsbHidMux {
namespace {

// Default report_size (63) already matches BTP_USB_HID_MAX_FRAME_SIZE --
// see bally_protocol/docs/TRANSPORT_USB_HID.md section 2 (64-byte HID
// report minus 1 byte for the Report ID).
USBHIDVendor vendor;
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
    const int available = vendor.available();
    if (available <= 0) {
        return;
    }
    uint8_t buffer[64];
    const size_t toRead = available > static_cast<int>(sizeof(buffer))
                              ? sizeof(buffer)
                              : static_cast<size_t>(available);
    const size_t got = vendor.readBytes(buffer, toRead);
    if (got > 0) {
        vendor.write(buffer, got);
    }
}

} // namespace UsbHidMux
