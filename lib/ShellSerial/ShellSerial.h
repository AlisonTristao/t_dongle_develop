#pragma once

#include <Arduino.h>

/**
 * @brief ShellSerial provides line-based serial input with editable history.
 *
 * Features:
 * - line reading from Stream/Serial
 * - backspace and clear input helpers
 * - left/right cursor movement with in-line editing
 * - in-memory circular log/history
 * - arrow up/down history recall using ESC sequences
 */
class ShellSerial final {
public:
	/**
	 * @brief Default number of lines stored in command history.
	 */
	static constexpr size_t DEFAULT_LOG_CAPACITY = 64;

	/**
	 * @brief Creates a ShellSerial instance.
	 * @param logCapacity Number of history entries to keep.
	 */
	explicit ShellSerial(size_t logCapacity = DEFAULT_LOG_CAPACITY);

	/**
	 * @brief Attaches an already initialized Stream object.
	 * @param serialPort Stream used for reading and terminal echo.
	 */
	void begin(Stream& serialPort);

	/**
	 * @brief Initializes a serial-like object with baud and attaches it.
	 * @tparam TSerial Serial-like type exposing begin(uint32_t).
	 * @param serialPort Serial object to initialize.
	 * @param baudRate Baudrate used in begin.
	 */
	template <typename TSerial>
	void begin(TSerial& serialPort, uint32_t baudRate) {
		serialPort.begin(baudRate);
		begin(static_cast<Stream&>(serialPort));
	}

	/**
	 * @brief Processes pending serial bytes and returns a complete line when available.
	 * @param outLine Receives the trimmed command line.
	 * @return true when a full line was received, otherwise false.
	 */
	bool readInputLine(String& outLine);

	/**
	 * @brief Removes one character from the current input buffer.
	 */
	void eraseLastChar();

	/**
	 * @brief Clears the entire current input buffer.
	 */
	void clearInput();

	/**
	 * @brief Adds a message to the circular history log.
	 * @param message Text entry to store.
	 */
	void addLog(const String& message);

	/**
	 * @brief Removes all stored history messages.
	 */
	void clearLogs();

	/**
	 * @brief Returns number of items currently stored in history.
	 */
	size_t logCount() const;

	/**
	 * @brief Reads a history item by logical index (oldest to newest).
	 * @param index Item index.
	 * @param outMessage Receives the selected entry.
	 * @return true if index exists, otherwise false.
	 */
	bool logAt(size_t index, String& outMessage) const;

	/**
	 * @brief Sets terminal prompt text shown before the editable input.
	 * @param text Prompt string.
	 */
	void setPrompt(const String& text);

	/**
	 * @brief Redraws current prompt and input buffer.
	 */
	void refreshLine();

private:
	/**
	 * @brief Internal hard limit for static log storage array.
	 */
	static constexpr size_t MAX_LOG_STORAGE = 128;
	static constexpr size_t MAX_INPUT_LENGTH = 512;
	static constexpr size_t MAX_RENDER_COLUMNS = 72;

	/**
	 * @brief Parser state for ANSI escape sequence handling.
	 */
	enum class EscState : uint8_t {
		None,
		Esc,
		Bracket
	};

	Stream* serial_;
	String inputBuffer_;
	String prompt_;
	String draftBeforeHistory_;
	size_t renderedLength_;
	size_t cursorIndex_;
	EscState escState_;
	bool ignoreNextLf_;

	String logStorage_[MAX_LOG_STORAGE];
	size_t capacity_;
	size_t count_;
	size_t firstIndex_;
	int historyCursor_;

	/**
	 * @brief Processes one received byte and updates parsing state.
	 */
	void processChar(char c, bool& lineReady, String& outLine);

	/**
	 * @brief Returns true when the current line has an unclosed double quote.
	 */
	bool hasUnclosedDoubleQuote() const;

	/**
	 * @brief Returns true when prompt+input fits in a single render line.
	 */
	bool fitsSingleRenderLine() const;

	/**
	 * @brief Builds the text window shown on screen and cursor position in that window.
	 */
	void buildVisibleInput(String& visibleInput, size_t& cursorInVisible) const;

	/**
	 * @brief Re-renders the current prompt and input line.
	 */
	void redrawInput();

	/**
	 * @brief Moves history cursor to older entry.
	 */
	void onArrowUp();

	/**
	 * @brief Moves history cursor to newer entry.
	 */
	void onArrowDown();

	/**
	 * @brief Moves cursor one position to the left.
	 */
	void onArrowLeft();

	/**
	 * @brief Moves cursor one position to the right.
	 */
	void onArrowRight();

	/**
	 * @brief Inserts a message into the circular history buffer.
	 */
	void pushLog(const String& message);

	/**
	 * @brief Gets a history message by logical offset from oldest entry.
	 */
	String getLogByOffset(size_t offset) const;
};
