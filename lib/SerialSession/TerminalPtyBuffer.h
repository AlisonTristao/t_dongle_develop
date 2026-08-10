#pragma once

#include <cstddef>
#include <cstdint>

namespace SerialSession {

/**
 * @brief Byte-oriented, fixed-capacity FIFO ring buffer. Pure C++ so it
 * compiles and is unit-tested under env:native like the rest of this lib.
 *
 * Used twice by TerminalPtyBuffer below (one instance per direction); kept
 * as a small private template instead of duplicating the same loop twice
 * inline (CONTRIBUTING.md section 1 -- extraction only once a pattern
 * already repeats for real, which it does here within the same file).
 */
template <std::size_t Capacity>
class ByteRing {
public:
    void reset() noexcept {
        head_ = 0U;
        count_ = 0U;
    }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0U; }

    // Appends up to len bytes, silently dropping whatever does not fit;
    // returns the number of bytes actually accepted. A full ring is a
    // defensive ceiling only -- ShellSerial::MAX_INPUT_LENGTH (512) and
    // this dongle's outbound payload cap (700, SerialMux.cpp) already keep
    // normal traffic well under Capacity for both directions.
    std::size_t push(const std::uint8_t* data, std::size_t len) noexcept {
        std::size_t accepted = 0U;
        while (accepted < len && count_ < Capacity) {
            const std::size_t tail = (head_ + count_) % Capacity;
            buffer_[tail] = data[accepted];
            ++accepted;
            ++count_;
        }
        return accepted;
    }

    int peek() const noexcept {
        if (count_ == 0U) {
            return -1;
        }
        return buffer_[head_];
    }

    int pop() noexcept {
        if (count_ == 0U) {
            return -1;
        }
        const std::uint8_t value = buffer_[head_];
        head_ = (head_ + 1U) % Capacity;
        --count_;
        return static_cast<int>(value);
    }

    // Copies up to cap bytes out in FIFO order and removes them from the
    // ring; returns the number copied.
    std::size_t drain(std::uint8_t* out, std::size_t cap) noexcept {
        std::size_t copied = 0U;
        while (copied < cap && count_ > 0U) {
            out[copied] = buffer_[head_];
            head_ = (head_ + 1U) % Capacity;
            --count_;
            ++copied;
        }
        return copied;
    }

private:
    std::uint8_t buffer_[Capacity];
    std::size_t head_ = 0U;
    std::size_t count_ = 0U;
};

// RX comfortably covers ShellSerial::MAX_INPUT_LENGTH (512) plus a bit of
// escape-sequence overhead for a pasted chunk. TX covers several full-line
// redraws plus a decent command result before SerialMux's tick()-driven
// drain (kOutboundPayloadCap-sized TERMINAL_OUT frames, SerialMux.cpp)
// catches up; both are generous multiples of what one tick() normally
// produces, since tick() runs roughly every loop iteration (~1ms).
constexpr std::size_t kTerminalPtyRxCapacity = 1024U;
constexpr std::size_t kTerminalPtyTxCapacity = 4096U;

/**
 * @brief Portable FIFO pair standing in for the real UART while a BTP v1
 * session is Protocolled (topico 19 PASSO 1/2: line editing stays on the
 * dongle, so ShellSerial keeps running unmodified against something that
 * behaves like a Stream). RX carries TERMINAL_IN payload bytes toward
 * ShellSerial; TX carries everything ShellSerial writes back (echo,
 * prompt, redraws) until SerialMux chunks it into TERMINAL_OUT frame(s).
 *
 * Both directions are pure FIFOs -- nothing here ever reorders or
 * overwrites an already-buffered byte -- so end-to-end ordering is
 * preserved (CRITERIO 1: "texto digitado nao e perdido ou deslocado").
 * SerialMux's Arduino::Stream adapter (TerminalPtyStream.h, lib/SerialMux)
 * is a thin wrapper with no logic of its own beyond forwarding to this
 * class, which is why the interesting behavior is tested here under
 * env:native instead of only on hardware.
 */
class TerminalPtyBuffer {
public:
    void reset() noexcept;

    // TERMINAL_IN payload bytes arriving off the wire. Returns the number
    // of bytes actually accepted (see ByteRing::push).
    std::size_t feedInput(const std::uint8_t* data, std::size_t len) noexcept;
    std::size_t availableInput() const noexcept;
    int readInput() noexcept;
    int peekInput() const noexcept;

    // Bytes ShellSerial writes back (echo/prompt/redraw).
    std::size_t writeOutput(const std::uint8_t* data, std::size_t len) noexcept;
    bool hasOutput() const noexcept;
    std::size_t takeOutput(std::uint8_t* out, std::size_t cap) noexcept;

private:
    ByteRing<kTerminalPtyRxCapacity> rx_;
    ByteRing<kTerminalPtyTxCapacity> tx_;
};

}  // namespace SerialSession
