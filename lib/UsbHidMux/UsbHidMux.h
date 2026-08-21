#pragma once

/**
 * @brief Hardware bring-up spike for the dongle's second USB interface (BTP
 * v1.1.0 "usb_hid" transport profile, bally_protocol/docs/TRANSPORT_USB_HID.md).
 *
 * This validates the composite CDC+HID device on the ESP32-S3 native USB OTG
 * peripheral (ARDUINO_USB_MODE=0) before any BTP framing is wired in:
 * begin()/tick() only echo whatever bytes the host writes to the HID vendor
 * report back out. That is enough to confirm on real hardware that:
 *
 * - Windows enumerates both the CDC port and the HID vendor interface
 *   simultaneously;
 * - the existing Serial/BTP-COBS console flow (SerialMux, StartupConfig's
 *   port-open wait, reset-to-bootloader via DTR) survives the
 *   ARDUINO_USB_MODE=1 -> 0 switch unregressed;
 * - sustained simultaneous CDC+HID traffic does not trip the known
 *   arduino-esp32 core 2.x stall bugs (issues #9582/#10307/#11600).
 *
 * Once validated, this module grows into the real transport: BTP session
 * wiring (reusing or trimming SerialSession, see its own header), a new
 * SudoManager/SubscriptionRegistry client identity prefix, and TX/RX
 * priority queues matching SerialMux's shape. No BTP dependency yet on
 * purpose, so this spike can be flashed and tested in isolation.
 */
namespace UsbHidMux {

/** Starts the HID vendor interface. Call once from AppRuntime::begin(),
 * alongside SerialMux::begin() -- both USB interfaces stay active together. */
void begin();

/** Echoes any bytes received on the HID vendor report back out. Call once
 * per AppRuntime::tick(); a fast no-op when nothing was received. */
void tick();

} // namespace UsbHidMux
