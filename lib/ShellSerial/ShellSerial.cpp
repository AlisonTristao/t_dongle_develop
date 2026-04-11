#include "ShellSerial.h"

ShellSerial::ShellSerial(size_t logCapacity)
	: serial_(nullptr),
	  renderedLength_(0),
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

void ShellSerial::begin(Stream& serialPort) {
	serial_ = &serialPort;

	inputBuffer_ = "";
	draftBeforeHistory_ = "";
	renderedLength_ = 0;
	escState_ = EscState::None;
	ignoreNextLf_ = false;
	historyCursor_ = -1;
}

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

void ShellSerial::eraseLastChar() {
	if (inputBuffer_.length() == 0) {
		return;
	}

	inputBuffer_.remove(inputBuffer_.length() - 1);
	historyCursor_ = -1;
	redrawInput();
}

void ShellSerial::clearInput() {
	inputBuffer_ = "";
	draftBeforeHistory_ = "";
	historyCursor_ = -1;
	redrawInput();
}

void ShellSerial::addLog(const String& message) {
	pushLog(message);
}

void ShellSerial::clearLogs() {
	count_ = 0;
	firstIndex_ = 0;
	historyCursor_ = -1;
}

size_t ShellSerial::logCount() const {
	return count_;
}

bool ShellSerial::logAt(size_t index, String& outMessage) const {
	if (index >= count_) {
		outMessage = "";
		return false;
	}

	outMessage = getLogByOffset(index);
	return true;
}

void ShellSerial::setPrompt(const String& text) {
	prompt_ = text;
	redrawInput();
}

void ShellSerial::processChar(char c, bool& lineReady, String& outLine) {
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
			}
			escState_ = EscState::None;
			return;
		}
	}

	if (c == 27) {
		escState_ = EscState::Esc;
		return;
	}

	if (c == '\r' || c == '\n') {
		if (c == '\n' && ignoreNextLf_) {
			ignoreNextLf_ = false;
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

		if (line.length() > 0) {
			pushLog(line);
			outLine = line;
			lineReady = true;
		}
		return;
	}

	ignoreNextLf_ = false;

	if (c == 8 || c == 127) {
		eraseLastChar();
		return;
	}

	if (static_cast<uint8_t>(c) >= 32) {
		if (historyCursor_ >= 0) {
			historyCursor_ = -1;
			draftBeforeHistory_ = "";
		}

		inputBuffer_ += c;
		redrawInput();
	}
}

void ShellSerial::redrawInput() {
	if (serial_ == nullptr) {
		return;
	}

	const String visibleLine = prompt_ + inputBuffer_;

	serial_->print('\r');
	serial_->print(visibleLine);

	if (renderedLength_ > visibleLine.length()) {
		const size_t extra = renderedLength_ - visibleLine.length();
		for (size_t i = 0; i < extra; ++i) {
			serial_->print(' ');
		}
	}

	serial_->print('\r');
	serial_->print(visibleLine);
	renderedLength_ = visibleLine.length();
}

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
	redrawInput();
}

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

	redrawInput();
}

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

String ShellSerial::getLogByOffset(size_t offset) const {
	if (offset >= count_) {
		return "";
	}

	const size_t index = (firstIndex_ + offset) % capacity_;
	return logStorage_[index];
}
