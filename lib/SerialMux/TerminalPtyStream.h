#pragma once

#include <Arduino.h>
#include <TerminalPtyBuffer.h>

#include <cstddef>
#include <cstdint>

/**
 * @brief Stands in for the real UART Stream while a BTP v1 session is
 * Protocolled (topico 19, PASSO 1/2: the dongle keeps ShellSerial as the
 * line editor instead of teaching TraceView a VT100 line editor). ShellSerial
 * only ever calls Stream::available()/read()/peek() and Print::write() --
 * this class implements exactly those against a SerialSession::
 * TerminalPtyBuffer, which does the actual FIFO bookkeeping and is the part
 * that gets a real unit test under env:native (Arduino::Stream itself
 * cannot be built there).
 */
class TerminalPtyStream final : public Stream {
public:
    void reset() noexcept { buffer_.reset(); }

    // Called by SerialMux::handleTerminalIn() with a decoded TERMINAL_IN
    // frame's payload bytes.
    void feedInput(const std::uint8_t* data, std::size_t len) noexcept { buffer_.feedInput(data, len); }

    // Called by SerialMux every tick() to drain whatever ShellSerial wrote
    // back, for chunking into TERMINAL_OUT frame(s).
    bool hasOutput() const noexcept { return buffer_.hasOutput(); }
    std::size_t takeOutput(std::uint8_t* out, std::size_t cap) noexcept { return buffer_.takeOutput(out, cap); }

    // Stream
    int available() override { return static_cast<int>(buffer_.availableInput()); }
    int read() override { return buffer_.readInput(); }
    int peek() override { return buffer_.peekInput(); }

    // Print
    size_t write(uint8_t byte) override { return buffer_.writeOutput(&byte, 1U); }
    size_t write(const uint8_t* data, size_t len) override { return buffer_.writeOutput(data, len); }
    using Print::write;

private:
    SerialSession::TerminalPtyBuffer buffer_;
};
