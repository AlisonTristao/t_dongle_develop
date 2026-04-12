#include "ShellSerial.h"

// Build the shell parser and history state with bounded storage.
ShellSerial::ShellSerial(size_t logCapacity)
	: serial_(nullptr),
	  renderedLength_(0),
	cursorIndex_(0),
	  escState_(EscState::None),
	  ignoreNextLf_(false),
	  capacity_(logCapacity),
	  count_(0),
	  firstIndex_(0),
	  historyCursor_(-1) {
	if (capacity_ == 0) {
		capacity_ = 1;
	}
	if (capacity_ > MAX_LOG_STORAGE) {
		capacity_ = MAX_LOG_STORAGE;
	}
}

// Attach the stream and reset runtime state.
void ShellSerial::begin(Stream& serialPort) {
	serial_ = &serialPort;

	inputBuffer_ = "";
	draftBeforeHistory_ = "";
	renderedLength_ = 0;
	cursorIndex_ = 0;
	escState_ = EscState::None;
	ignoreNextLf_ = false;
	historyCursor_ = -1;
}

// Consume all available bytes until a full line is ready.
bool ShellSerial::readInputLine(String& outLine) {
	outLine = "";

	if (serial_ == nullptr) {
		return false;
	}

	bool lineReady = false;
	while (serial_->available() > 0) {
		const char c = static_cast<char>(serial_->read());
		processChar(c, lineReady, outLine);
		if (lineReady) {
			break;
		}
	}

	return lineReady;
}

// Apply local backspace behavior and redraw terminal line.
void ShellSerial::eraseLastChar() {
	if (inputBuffer_.length() == 0 || cursorIndex_ == 0) {
		return;
	}

	const bool removeAtTail = (cursorIndex_ == inputBuffer_.length());
	if (removeAtTail && serial_ != nullptr && fitsSingleRenderLine()) {
		inputBuffer_.remove(cursorIndex_ - 1, 1);
		--cursorIndex_;
		historyCursor_ = -1;

		serial_->print("\b \b");
		if (renderedLength_ > 0) {
			--renderedLength_;
		}
		return;
	}

	inputBuffer_.remove(cursorIndex_ - 1, 1);
	--cursorIndex_;
	historyCursor_ = -1;
	redrawInput();
}

// Reset editable text and leave history untouched.
void ShellSerial::clearInput() {
	inputBuffer_ = "";
	draftBeforeHistory_ = "";
	cursorIndex_ = 0;
	historyCursor_ = -1;
	redrawInput();
}

// Public helper to append text to history buffer.
void ShellSerial::addLog(const String& message) {
	pushLog(message);
}

// Remove all command history entries.
void ShellSerial::clearLogs() {
	count_ = 0;
	firstIndex_ = 0;
	historyCursor_ = -1;
}

// Return current number of stored entries.
size_t ShellSerial::logCount() const {
	return count_;
}

// Read one entry from logical history position.
bool ShellSerial::logAt(size_t index, String& outMessage) const {
	if (index >= count_) {
		outMessage = "";
		return false;
	}

	outMessage = getLogByOffset(index);
	return true;
}

// Set prompt and immediately update visual line.
void ShellSerial::setPrompt(const String& text) {
	prompt_ = text;
	redrawInput();
}

void ShellSerial::refreshLine() {
	redrawInput();
}

bool ShellSerial::hasUnclosedDoubleQuote() const {
	bool inDoubleQuote = false;
	bool escaped = false;

	for (size_t i = 0; i < inputBuffer_.length(); ++i) {
		const char ch = inputBuffer_[i];

		if (escaped) {
			escaped = false;
			continue;
		}

		if (ch == '\\') {
			escaped = true;
			continue;
		}

		if (ch == '"') {
			inDoubleQuote = !inDoubleQuote;
		}
	}

	return inDoubleQuote;
}

bool ShellSerial::fitsSingleRenderLine() const {
	return (prompt_.length() + inputBuffer_.length()) <= MAX_RENDER_COLUMNS;
}

void ShellSerial::buildVisibleInput(String& visibleInput, size_t& cursorInVisible) const {
	const size_t promptLen = prompt_.length();
	size_t maxInputColumns = 8;
	if (MAX_RENDER_COLUMNS > promptLen) {
		maxInputColumns = MAX_RENDER_COLUMNS - promptLen;
	}

	const size_t inputLen = inputBuffer_.length();
	if (inputLen <= maxInputColumns) {
		visibleInput = inputBuffer_;
		cursorInVisible = cursorIndex_;
		return;
	}

	const size_t coreColumns = (maxInputColumns >= 3) ? (maxInputColumns - 2) : 1;
	size_t start = 0;

	if (cursorIndex_ > coreColumns / 2) {
		start = cursorIndex_ - (coreColumns / 2);
	}

	if (start + coreColumns > inputLen) {
		start = inputLen - coreColumns;
	}

	const size_t end = start + coreColumns;
	const bool leftTruncated = (start > 0);
	const bool rightTruncated = (end < inputLen);

	visibleInput = "";
	if (leftTruncated) {
		visibleInput += "<";
	}
	visibleInput += inputBuffer_.substring(start, end);
	if (rightTruncated) {
		visibleInput += ">";
	}

	cursorInVisible = (leftTruncated ? 1U : 0U) + (cursorIndex_ - start);
	if (cursorInVisible > visibleInput.length()) {
		cursorInVisible = visibleInput.length();
	}
}

void ShellSerial::processChar(char c, bool& lineReady, String& outLine) {
	// Parse ANSI escape sequences, especially arrow keys.
	if (escState_ != EscState::None) {
		if (escState_ == EscState::Esc) {
			escState_ = (c == '[') ? EscState::Bracket : EscState::None;
			return;
		}

		if (escState_ == EscState::Bracket) {
			if (c == 'A') {
				onArrowUp();
			} else if (c == 'B') {
				onArrowDown();
			    } else if (c == 'C') {
				    onArrowRight();
			    } else if (c == 'D') {
				    onArrowLeft();
			}
			escState_ = EscState::None;
			return;
		}
	}

	// Escape sequence introducer.
	if (c == 27) {
		escState_ = EscState::Esc;
		return;
	}

	// Finish command line on CR/LF and push non-empty command to history.
	if (c == '\r' || c == '\n') {
		if (c == '\n' && ignoreNextLf_) {
			ignoreNextLf_ = false;
			return;
		}

		if (hasUnclosedDoubleQuote()) {
			ignoreNextLf_ = (c == '\r');

			if (inputBuffer_.length() < MAX_INPUT_LENGTH && !inputBuffer_.endsWith(" ")) {
				inputBuffer_ += ' ';
			}

			cursorIndex_ = inputBuffer_.length();
			renderedLength_ = prompt_.length() + inputBuffer_.length();
			return;
		}

		ignoreNextLf_ = (c == '\r');

		String line = inputBuffer_;
		line.trim();

		if (serial_ != nullptr) {
			serial_->print("\r\n");
		}

		inputBuffer_ = "";
		draftBeforeHistory_ = "";
		historyCursor_ = -1;
		renderedLength_ = 0;
		    cursorIndex_ = 0;

		if (line.length() > 0) {
			pushLog(line);
			outLine = line;
			lineReady = true;
		}
		return;
	}

	ignoreNextLf_ = false;

	// Handle both backspace variants from terminals.
	if (c == 8 || c == 127) {
		eraseLastChar();
		return;
	}

	// Append printable characters.
	if (static_cast<uint8_t>(c) >= 32) {
		if (inputBuffer_.length() >= MAX_INPUT_LENGTH) {
			return;
		}

		if (historyCursor_ >= 0) {
			historyCursor_ = -1;
			draftBeforeHistory_ = "";
		}

		const bool appendAtTail = (cursorIndex_ >= inputBuffer_.length());
		if (appendAtTail) {
			inputBuffer_ += c;
			++cursorIndex_;

			if (serial_ != nullptr && fitsSingleRenderLine()) {
				serial_->print(c);
				renderedLength_ = prompt_.length() + inputBuffer_.length();
				return;
			}

			redrawInput();
			return;
		}

		const String left = inputBuffer_.substring(0, cursorIndex_);
		const String right = inputBuffer_.substring(cursorIndex_);
		inputBuffer_ = left + String(c) + right;
		++cursorIndex_;
		redrawInput();
	}
}

// Repaint current terminal line and clear leftover chars from previous frame.
void ShellSerial::redrawInput() {
	if (serial_ == nullptr) {
		return;
	}

	String visibleInput;
	size_t cursorInVisible = 0;
	buildVisibleInput(visibleInput, cursorInVisible);

	const String visibleLine = prompt_ + visibleInput;
	const size_t cursorColumn = prompt_.length() + cursorInVisible;

	serial_->print('\r');
	serial_->print(visibleLine);

	if (renderedLength_ > visibleLine.length()) {
		const size_t extra = renderedLength_ - visibleLine.length();
		for (size_t i = 0; i < extra; ++i) {
			serial_->print(' ');
		}
	}

	serial_->print('\r');
	for (size_t i = 0; i < cursorColumn && i < visibleLine.length(); ++i) {
		serial_->print(visibleLine[i]);
	}

	renderedLength_ = visibleLine.length();
}

// Move to older command in history.
void ShellSerial::onArrowUp() {
	if (count_ == 0) {
		return;
	}

	if (historyCursor_ < 0) {
		draftBeforeHistory_ = inputBuffer_;
		historyCursor_ = static_cast<int>(count_) - 1;
	} else if (historyCursor_ > 0) {
		--historyCursor_;
	}

	inputBuffer_ = getLogByOffset(static_cast<size_t>(historyCursor_));
	cursorIndex_ = inputBuffer_.length();
	redrawInput();
}

// Move to newer command in history or restore unsent draft.
void ShellSerial::onArrowDown() {
	if (count_ == 0 || historyCursor_ < 0) {
		return;
	}

	if (historyCursor_ < static_cast<int>(count_) - 1) {
		++historyCursor_;
		inputBuffer_ = getLogByOffset(static_cast<size_t>(historyCursor_));
	} else {
		historyCursor_ = -1;
		inputBuffer_ = draftBeforeHistory_;
	}

	cursorIndex_ = inputBuffer_.length();
	redrawInput();
}

// Move cursor one position to the left.
void ShellSerial::onArrowLeft() {
	if (cursorIndex_ == 0) {
		return;
	}

	--cursorIndex_;

	if (serial_ != nullptr && fitsSingleRenderLine()) {
		// Move one column left without repainting the full line.
		serial_->print('\b');
	} else {
		redrawInput();
	}
}

// Move cursor one position to the right.
void ShellSerial::onArrowRight() {
	if (cursorIndex_ >= inputBuffer_.length()) {
		return;
	}

	if (serial_ != nullptr && fitsSingleRenderLine()) {
		// Advance by re-printing the character under the logical cursor.
		serial_->print(inputBuffer_[cursorIndex_]);
		++cursorIndex_;
	} else {
		++cursorIndex_;
		redrawInput();
	}
}

// Store normalized text in circular buffer and overwrite oldest on overflow.
void ShellSerial::pushLog(const String& message) {
	String normalized = message;
	normalized.trim();
	if (normalized.length() == 0) {
		return;
	}

	if (count_ < capacity_) {
		const size_t insertIndex = (firstIndex_ + count_) % capacity_;
		logStorage_[insertIndex] = normalized;
		++count_;
		return;
	}

	logStorage_[firstIndex_] = normalized;
	firstIndex_ = (firstIndex_ + 1) % capacity_;
}

// Convert logical offset (oldest..newest) to circular index.
String ShellSerial::getLogByOffset(size_t offset) const {
	if (offset >= count_) {
		return "";
	}

	const size_t index = (firstIndex_ + offset) % capacity_;
	return logStorage_[index];
}
