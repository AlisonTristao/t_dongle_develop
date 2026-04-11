#pragma once

#include <Arduino.h>

class ShellSerial final {
public:
	static constexpr size_t DEFAULT_LOG_CAPACITY = 64;

	explicit ShellSerial(size_t logCapacity = DEFAULT_LOG_CAPACITY);

	void begin(Stream& serialPort);

	template <typename TSerial>
	void begin(TSerial& serialPort, uint32_t baudRate) {
		serialPort.begin(baudRate);
		begin(static_cast<Stream&>(serialPort));
	}

	bool readInputLine(String& outLine);

	void eraseLastChar();
	void clearInput();

	void addLog(const String& message);
	void clearLogs();
	size_t logCount() const;
	bool logAt(size_t index, String& outMessage) const;

	void setPrompt(const String& text);

private:
	static constexpr size_t MAX_LOG_STORAGE = 128;

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
	EscState escState_;
	bool ignoreNextLf_;

	String logStorage_[MAX_LOG_STORAGE];
	size_t capacity_;
	size_t count_;
	size_t firstIndex_;
	int historyCursor_;

	void processChar(char c, bool& lineReady, String& outLine);
	void redrawInput();
	void onArrowUp();
	void onArrowDown();
	void pushLog(const String& message);
	String getLogByOffset(size_t offset) const;
};
