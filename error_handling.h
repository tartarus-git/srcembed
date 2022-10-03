#pragma once

template <size_t message_size>
void writeErrorAndExit(const char (&message)[message_size], int exitCode) noexcept {
	write(STDERR_FILENO, message, message_size - 1);
	std::exit(exitCode);
}

#define REPORT_ERROR_AND_EXIT(message, exitCode) writeErrorAndExit("ERROR: " message "\n", exitCode)
