#include "TerminalPtyBuffer.h"

namespace SerialSession {

void TerminalPtyBuffer::reset() noexcept {
    rx_.reset();
    tx_.reset();
}

std::size_t TerminalPtyBuffer::feedInput(const std::uint8_t* data, std::size_t len) noexcept {
    if (data == nullptr || len == 0U) {
        return 0U;
    }
    return rx_.push(data, len);
}

std::size_t TerminalPtyBuffer::availableInput() const noexcept {
    return rx_.size();
}

int TerminalPtyBuffer::readInput() noexcept {
    return rx_.pop();
}

int TerminalPtyBuffer::peekInput() const noexcept {
    return rx_.peek();
}

std::size_t TerminalPtyBuffer::writeOutput(const std::uint8_t* data, std::size_t len) noexcept {
    if (data == nullptr || len == 0U) {
        return 0U;
    }
    return tx_.push(data, len);
}

bool TerminalPtyBuffer::hasOutput() const noexcept {
    return !tx_.empty();
}

std::size_t TerminalPtyBuffer::takeOutput(std::uint8_t* out, std::size_t cap) noexcept {
    return tx_.drain(out, cap);
}

}  // namespace SerialSession
