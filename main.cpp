#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE, as well as most other syscalls

#ifndef PLATFORM_WINDOWS

#include <sys/mman.h>		// for mmap(), munmap() and posix_madvise() support
#include <sys/stat.h>		// for fstat() support
#include <fcntl.h>		// for posix_fadvise() support

#endif

#include <cstring>		// for std::strcmp()

#include <iostream>		// TODO: Remove.

#include "crossplatform_io.h"
#include "async_streamed_io.h"

// These (technically just stdout_stream) need to be located before meta_printf.h include.
using stdin_stream = asyncio::stdin_stream<65536>;
using stdout_stream = asyncio::stdout_stream<65536>;

#include "meta_printf.h"	// for compile-time printf

#include "meminfo_parser.h"	// for getting huge page size from /proc/meminfo

#ifndef PLATFORM_WINDOWS

const long pagesize = sysconf(_SC_PAGE_SIZE);

#endif

const char helpText[] = "usage: srcembed <--help> || ([--varname <variable name>] <language>)\n" \
			"\n" \
			"function: converts input byte stream into source file (output through stdout)\n" \
			"\n" \
			"arguments:\n" \
				"\t<--help>                      --> displays help text\n" \
				"\t[--varname <variable name>]   --> specifies the variable name by which the embedded file shall be referred to in code\n" \
				"\t<language>                    --> specifies the source language\n" \
			"\n" \
			"supported languages (possible inputs for <language> field):\n" \
				"\tc++\n" \
				"\tc\n";

[[noreturn]] void halt_program_no_cleanup(int exit_code) noexcept {
	// TODO: See about doing something with the hlt instruction and such. Look at abort.c source code.
	std::_Exit(exit_code);
}

template <size_t message_size>
void writeErrorAndExit(const char (&message)[message_size], int exitCode) noexcept {
	crossplatform_write(STDERR_FILENO, message, message_size - 1);
	halt_program_no_cleanup(exitCode);
	// NOTE: No cleanup is necessary because:
	//	1. Why should we waste our time cleaning things up, pretty pointless.
	//	2. The real reason is so that the thread destructors don't get called, since those
	//		call std::terminate on the program if the threads aren't joined, which they probably aren't.
	//	(std::terminate wouldn't necessarily be bad, but it prints an error message which is suboptimal)
	//		--> We could add a termination handler and override the exit message, but why do that when the better option is this?
}

#define REPORT_ERROR_AND_EXIT(message, exitCode) writeErrorAndExit("ERROR: " message "\n", exitCode)

/*
EPIPHANY:
	- the second best way to transmit data is to have super big buffers and read, then write with those buffers (unless you've got splice and such)
	- this'll save on syscalls, which is great.
	- there'll be big gaps in between sends where you refill your buffer, but that doesn't matter because with smaller buffers, there might be smaller gaps, but there'll be more of them, so it equals out
	- if the program after you is the bottleneck, your sends will take forever, but it doesn't matter because the speed of the write doesn't change anything about the gap equality law thing above.
	- same thing with reads

	- the absolute best way to make use of reads and writes is to double your buffer and read and write simultaeniously
	- you'll always be reading and writing at the same time, double the speed

	- if the goal is to make the buffers as big as possible within the constraints of your usage and system and such, that begs the question:
		- what the hell is BUFSIZ for?
			- Maybe that's just the recommended buffer size because they didn't want programs using too much RAM?
			- I can't see any other reason since it offers no performance benefits apparently.
			- TODO: Research this, maybe I've made a crucial mistake in my thought process.
*/

#ifndef PLATFORM_WINDOWS

		// TODO: Make all the error messages nice looking and descriptive.

ssize_t mmap_write_double_buffer_simple(char*& bufferA, char*& bufferB, size_t bufferSize) noexcept {
	bufferA = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (bufferA == MAP_FAILED) { return -1; }

	bufferB = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (bufferB == MAP_FAILED) {
		munmap(bufferA, bufferSize);
		// NOTE: We don't handle error here because the program still has a chance at
		// completing it's job even if the above call fails, we just end up leaking the memory.
		return -1;
	}

	return bufferSize;
}

ssize_t mmapWriteDoubleBuffer(char*& bufferA, char*& bufferB, size_t bufferSize) noexcept {
	const size_t huge_page_size = parse_huge_page_size_from_meminfo_file();
	if (huge_page_size <= 0) { return mmap_write_double_buffer_simple(bufferA, bufferB, bufferSize); }

	// TODO: Consider picking the best huge page size for the job dynamically instead of just using the default one.

	bufferA = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (bufferA == MAP_FAILED) { return mmap_write_double_buffer_simple(bufferA, bufferB, bufferSize); }

	// Round up to nearest huge page boundary.
	const size_t rounded_bufferSize = bufferSize + huge_page_size - ((bufferSize - 1) % huge_page_size) - 1;
	// TODO: Is that the best way to do it? How would one do it in assembly?

	bufferB = (char*)mmap(nullptr, rounded_bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (bufferB == MAP_FAILED) {
		bufferB = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (bufferB == MAP_FAILED) {
			munmap(bufferA, rounded_bufferSize);
			return -1;
		}
	}

	return rounded_bufferSize;
}

// NOTE: UP TO HERE!

const unsigned char* mmapStdinFile(size_t stdinFileSize) noexcept {
	// NOTE: Can't see huge pages being beneficial here, so we're leaving them out.
	unsigned char* stdinFileData = (unsigned char*)mmap(nullptr, stdinFileSize, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_POPULATE, STDIN_FILENO, 0);
	// NOTE: We don't handle errors on purpose, the caller handles those.

	// NOTE: I'm pretty sure these don't overwrite each other, but just in case, I put the more important one second.
	posix_madvise(stdinFileData, stdinFileSize, POSIX_MADV_WILLNEED);
	posix_madvise(stdinFileData, stdinFileSize, POSIX_MADV_SEQUENTIAL);

	return stdinFileData;
}

enum class DataTransferExitCode {
	SUCCESS,
	NEEDS_FALLBACK,
	NEEDS_FALLBACK_FROM_MMAP,
	NO_INPUT_DATA
};

template <size_t pattern_size>
consteval size_t calculate_max_printf_write_length(const char (&pattern)[pattern_size]) {
	size_t result = pattern_size - 1;
	for (size_t i = 0; i < pattern_size; i++) {
		if (pattern[i] == '%') { result++; }
	}
	return result;
}

template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, unsigned char... chunk_indices>
DataTransferExitCode dataMode_mmap_vmsplice(size_t stdinFileSize) noexcept {
	constexpr size_t max_printf_write_length = calculate_max_printf_write_length(printf_pattern.data);
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
	if (stdoutPipeBufferSize == -1) { return DataTransferExitCode::NEEDS_FALLBACK; }

	struct iovec stdoutBufferMemorySpan_entireLength;
	stdoutBufferMemorySpan_entireLength.iov_len = stdoutPipeBufferSize;
	struct iovec stdoutBufferMemorySpan;

	// NOTE: We separate buffers to avoid cache contention. TODO: Figure out how that works.
	char* stdoutBuffers[2];

	const ssize_t actual_pipe_buffer_size = mmapWriteDoubleBuffer(stdoutBuffers[0], stdoutBuffers[1], stdoutPipeBufferSize);
	if (actual_pipe_buffer_size == -1) { return DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP; }

	bool stdoutBufferToggle = false;
	char* currentStdoutBuffer = stdoutBuffers[0];

	char tempBuffer[max_printf_write_length * 2];
	size_t tempBuffer_head = 0;
	size_t tempBuffer_tail = 0;

	const unsigned char* stdinFileData = mmapStdinFile(stdinFileSize);
	if (stdinFileData == MAP_FAILED) { return DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP; }
	size_t stdinFileDataCutoff = stdinFileSize - bytes_per_chunk;
	size_t stdinFileDataPosition = 1;

	int bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer, initial_printf_pattern.data, stdinFileData[0]);
	if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

	while (true) {
		while (amountOfBufferFilled <= stdoutPipeBufferSize - max_printf_write_length) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, single_printf_pattern.data, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				tempBuffer_head = amountOfBufferFilled % pagesize;
				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) { REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE); }

				if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
					REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
				}

				if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap input file", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }


				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, printf_pattern.data, stdinFileData[stdinFileDataPosition + chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			stdinFileDataPosition += 8;
			amountOfBufferFilled += bytesWritten;
		}

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, single_printf_pattern.data, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);
					amountOfBufferFilled += tempBuffer_head;

					tempBuffer_head = amountOfBufferFilled % pagesize;
					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) {
						REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
					}

					if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
						REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
					}

					if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap input file", EXIT_FAILURE); }
					if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
					if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_GIFT) == -1) {
					REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
				}

				if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap input file", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				// TODO: You're doing all this stuff with offsets, could you do it with pointers instead, we could rework stdout_stream to allow that.
				if (!stdout_stream::write(tempBuffer + tempBuffer_tail, amountOfBufferFilled)) {
					REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, printf_pattern.data, stdinFileData[stdinFileDataPosition + chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			stdinFileDataPosition += 8;
			tempBuffer_head += bytesWritten;
		}

		std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

		stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
		if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_MORE) == -1) {
			REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
		}

		currentStdoutBuffer = stdoutBuffers[stdoutBufferToggle = !stdoutBufferToggle];

		amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
		std::memcpy(currentStdoutBuffer, tempBuffer + tempBuffer_tail, amountOfBufferFilled);
	}
}

template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, size_t... chunk_indices>
bool dataMode_mmap_write(size_t stdinFileSize) noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	const unsigned char* stdinFileData = mmapStdinFile(stdinFileSize);
	if (stdinFileData == MAP_FAILED) { return false; }

	if (meta_printf_no_terminator(initial_printf_pattern.data, stdinFileData[0]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
	size_t i;
	for (i = 1; i < stdinFileSize + 1 - bytes_per_chunk; i += bytes_per_chunk) {
		if (meta_printf_no_terminator(printf_pattern.data, stdinFileData[i + chunk_indices]...) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
	}
	for (; i < stdinFileSize; i++) {
		if (meta_printf_no_terminator(single_printf_pattern.data, stdinFileData[i]) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
	}

	if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap input file", EXIT_FAILURE); }
	return true;
}

template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, unsigned char... chunk_indices>
DataTransferExitCode dataMode_read_vmsplice() noexcept {
	constexpr size_t max_printf_write_length = calculate_max_printf_write_length(printf_pattern.data);
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
	if (stdoutPipeBufferSize == -1) { return DataTransferExitCode::NEEDS_FALLBACK; }

	struct iovec stdoutBufferMemorySpan_entireLength;
	stdoutBufferMemorySpan_entireLength.iov_len = stdoutPipeBufferSize;
	struct iovec stdoutBufferMemorySpan;

	// NOTE: We separate buffers to avoid cache contention. TODO: Figure out how that works.
	char* stdoutBuffers[2];

	const ssize_t actual_pipe_buffer_size = mmapWriteDoubleBuffer(stdoutBuffers[0], stdoutBuffers[1], stdoutPipeBufferSize);
	if (actual_pipe_buffer_size == -1) { return DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP; }

	bool stdoutBufferToggle = false;
	char* currentStdoutBuffer = stdoutBuffers[0];

	char tempBuffer[max_printf_write_length * 2];
	size_t tempBuffer_head = 0;
	size_t tempBuffer_tail = 0;

	unsigned char inputBuffer[bytes_per_chunk];
	// TODO: Data type gets smaller once you remove/constrain chunk_indices stuff since your using new output system.
	int16_t bytesRead = stdin_stream::read((char*)inputBuffer, 1);
	switch (bytesRead) {
	case -1:
		REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE);
	case 0:
		return DataTransferExitCode::NO_INPUT_DATA;
	}

	int bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer, initial_printf_pattern.data, inputBuffer[0]);
	if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

	while (true) {
		while (amountOfBufferFilled <= stdoutPipeBufferSize - max_printf_write_length) {
			bytesRead = stdin_stream::read((char*)inputBuffer, bytes_per_chunk);
			if (bytesRead == -1) {
				REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE);
			}
			if (bytesRead < bytes_per_chunk) {
				for (unsigned char i = 0; i < bytesRead; i++) {
					bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, single_printf_pattern.data, inputBuffer[i]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				tempBuffer_head = amountOfBufferFilled % pagesize;
				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) { REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE); }

				if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
					REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
				}

				if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, printf_pattern.data, inputBuffer[chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			amountOfBufferFilled += bytesWritten;
		}

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			bytesRead = stdin_stream::read((char*)inputBuffer, bytes_per_chunk);
			if (bytesRead == -1) {
				REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE);
			}
			if (bytesRead < bytes_per_chunk) {
				for (unsigned char i = 0; i < bytesRead; i++) {
					bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, single_printf_pattern.data, inputBuffer[i]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);
					amountOfBufferFilled += tempBuffer_head;

					tempBuffer_head = amountOfBufferFilled % pagesize;
					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) {
						REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
					}

					if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
						REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
					}

					if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
					if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_GIFT) == -1) {
					REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
				}

				if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				if (!stdout_stream::write(tempBuffer + tempBuffer_tail, amountOfBufferFilled)) {
					if (ferror(stdout)) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, printf_pattern.data, inputBuffer[chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			tempBuffer_head += bytesWritten;
		}

		std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

		stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
		if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_MORE) == -1) {
			REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
		}

		currentStdoutBuffer = stdoutBuffers[stdoutBufferToggle = !stdoutBufferToggle];

		amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
		std::memcpy(currentStdoutBuffer, tempBuffer + tempBuffer_tail, amountOfBufferFilled);
	}
}

#endif

template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, unsigned char... chunk_indices>
bool dataMode_read_write() noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

#ifndef PLATFORM_WINDOWS
	if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_NOREUSE) == 0) {
		if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_WILLNEED) == 0) {
			posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL);
		}
	}
#endif

	unsigned char buffer[bytes_per_chunk];

	// NOTE: fread shouldn't ever return less than the wanted amount of bytes unless either:
	// a) EOF
	// b) an error occurred
	// In this way, it is very different to the raw I/O (read and write).
	std::cerr << "got to before reading\n";
	int16_t bytesRead = stdin_stream::read((char*)buffer, 1);
	std::cerr << "got to after reading\n";
	switch (bytesRead) {
	case -1:
		REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE);
	case 0:
		return false;
	}

	if (meta_printf_no_terminator(initial_printf_pattern.data, buffer[0]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }

	while (true) {
		bytesRead = stdin_stream::read((char*)buffer, bytes_per_chunk);

		if (bytesRead == bytes_per_chunk) {
			if (meta_printf_no_terminator(printf_pattern.data, buffer[chunk_indices]...) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
			continue;
		}

		if (bytesRead == -1) {
			REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE);
		}

		for (unsigned char i = 0; i < bytesRead; i++) {
			std::cerr << "iteration: " << (int)i << '\n';
			if (meta_printf_no_terminator(single_printf_pattern.data, buffer[i]) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
		}

		return true;
	}
}

template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, unsigned char... chunk_indices>
bool optimizedDataTransformationAndOutput_raw() noexcept {
	static_assert(sizeof...(chunk_indices) != 0, "parameter pack \"chunk_indices\" must contain at least 1 element");
	static_assert(sizeof...(chunk_indices) < 256, "parameter pack \"chunk_indices\" must contain less than 256 elements");

#ifndef PLATFORM_WINDOWS

	struct stat statusA;
	if (fstat(STDIN_FILENO, &statusA) == 0) {
		if (S_ISREG(statusA.st_mode)) {
			struct stat statusB;
			if (fstat(STDOUT_FILENO, &statusB) == 0) {
				if (S_ISFIFO(statusB.st_mode)) {
					if (statusA.st_size == 0) { return false; }
					if (sizeof(size_t) >= sizeof(off_t) || statusA.st_size <= (size_t)-1) {
						switch (dataMode_mmap_vmsplice<initial_printf_pattern, printf_pattern, single_printf_pattern, chunk_indices...>(statusA.st_size)) {
						case DataTransferExitCode::SUCCESS: return true;
						case DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP: goto use_data_mode_read_vmsplice;
						case DataTransferExitCode::NEEDS_FALLBACK: break;
						}
					}
				}
			}

			if (statusA.st_size == 0) { return false; }
			// NOTE: If size_t is 32-bit and linux large file extention is enabled (off_t is 64-bit),
			// only allow mmapping if file length can fit into size_t.
			if (sizeof(size_t) >= sizeof(off_t) && statusA.st_size <= (size_t)-1) {
				if (dataMode_mmap_write<initial_printf_pattern, printf_pattern, single_printf_pattern, chunk_indices...>(statusA.st_size)) { return true; }
			}

			return dataMode_read_write<initial_printf_pattern, printf_pattern, single_printf_pattern, chunk_indices...>();
		}
	}

	if (fstat(STDOUT_FILENO, &statusA) == 0) {
		if (S_ISFIFO(statusA.st_mode)) {
use_data_mode_read_vmsplice:
			switch (dataMode_read_vmsplice<initial_printf_pattern, printf_pattern, single_printf_pattern, chunk_indices...>()) {
			case DataTransferExitCode::SUCCESS: return true;
			case DataTransferExitCode::NO_INPUT_DATA: return false;
			case DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP: case DataTransferExitCode::NEEDS_FALLBACK: break;
			}
		}
	}

#endif

	return dataMode_read_write<initial_printf_pattern, printf_pattern, single_printf_pattern, chunk_indices...>();
}

#define optimizedDataTransformationAndOutput(initialPrintfPattern, printfPattern, singlePrintfPattern, ...) [&]() { static constexpr auto initial_printf_pattern = meta::construct_meta_array(initialPrintfPattern); static constexpr auto printf_pattern = meta::construct_meta_array(printfPattern); static constexpr auto single_printf_pattern = meta::construct_meta_array(singlePrintfPattern); return optimizedDataTransformationAndOutput_raw<initial_printf_pattern, printf_pattern, single_printf_pattern, __VA_ARGS__>(); }()

namespace flags {
	const char* varname = nullptr;
}

int manageArgs(int argc, const char* const * argv) noexcept {
	int normalArgIndex = 0;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
			case '-':
				{
					const char* flagContent = argv[i] + 2;
					if (std::strcmp(flagContent, "varname") == 0) {
						if (flags::varname != nullptr) {
							REPORT_ERROR_AND_EXIT("more than one instance of \"--varname\" flag illegal", EXIT_SUCCESS);
						}
						i++;
						if (i == argc) {
							REPORT_ERROR_AND_EXIT("\"--varname\" flag requires a value", EXIT_SUCCESS);
						}
						flags::varname = argv[i];
						continue;
					}
					if (std::strcmp(flagContent, "help") == 0) {
						if (argc != 2) { REPORT_ERROR_AND_EXIT("use of \"--help\" flag with other args is illegal", EXIT_SUCCESS); }
						if (crossplatform_write(STDOUT_FILENO, helpText, sizeof(helpText) - 1) == -1) {
							REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
						}
						std::exit(EXIT_SUCCESS);
					}
					REPORT_ERROR_AND_EXIT("one or more invalid flags specified", EXIT_SUCCESS);
				}
			default: REPORT_ERROR_AND_EXIT("one or more invalid flags specified", EXIT_SUCCESS);
			}
		}
		if (normalArgIndex != 0) { REPORT_ERROR_AND_EXIT("too many non-flag args", EXIT_SUCCESS); }
		normalArgIndex = i;
	}
	if (normalArgIndex == 0) { REPORT_ERROR_AND_EXIT("not enough non-flags args", EXIT_SUCCESS); }
	if (flags::varname == nullptr) { flags::varname = "data"; }
	return normalArgIndex;
}

void output_C_CPP_array_data() noexcept {
	if (optimizedDataTransformationAndOutput("%u", ", %u, %u, %u, %u, %u, %u, %u, %u", ", %u", 0, 1, 2, 3, 4, 5, 6, 7) == false) {
		REPORT_ERROR_AND_EXIT("no data received, language requires data", EXIT_FAILURE);
	}
}

template <size_t output_size>
void writeOutput(const char (&output)[output_size]) noexcept {
	if (!stdout_stream::write(output, output_size - sizeof(char))) {
		REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
	}
}

void initialize_streams() noexcept {
	if (!stdin_stream::initialize()) { REPORT_ERROR_AND_EXIT("failed to initialize stdin stream", EXIT_FAILURE); }
	stdout_stream::initialize();
}

void outputSource(const char* language) noexcept {
	if (std::strcmp(language, "c++") == 0) {
		if (std::printf("const char %s[] { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
		if (fflush(stdout) == EOF) {
			REPORT_ERROR_AND_EXIT("failed to flush stdout", EXIT_FAILURE);
		}
		initialize_streams();
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		if (std::printf("const char %s[] = { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
		if (fflush(stdout) == EOF) {
			REPORT_ERROR_AND_EXIT("failed to flush stdout", EXIT_FAILURE);
		}
		initialize_streams();
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}

	REPORT_ERROR_AND_EXIT("invalid language", EXIT_SUCCESS);
}

// TODO: I think the main source of slowness is fwrite and fread.
// We need a custom streamed I/O implementation does the refilling of the buffer asynchronously.
// That way, we can write and do calculations at the same time, we'll be condensing the timeline and making everything a lot faster.
// Same with input. I don't want a straight circular buffer that writes and reads every chance it gets since that's a lot of unnecessary syscalls, which doesn't sound like a good idea.
// Maybe each buffer has two segments, each refills while the other is being used for reading/writing. When seg 2 is done and seg 1 gets selected again, the selector needs to check if reading/writing is done on seg 1.
// Same with transition from seg 1 to seg 2.

// TODO: Actually, consider making vm splice asynchronous as well. That virtual to physical mapping that it does is also kind of slow, we could smush that together with other timings if we do it in parallel with twice the buffer space to compensate for it.

int main(int argc, const char* const * argv) noexcept {
	std::cerr << parse_huge_page_size_from_meminfo_file() << '\n';

	// C++ standard I/O can suck it, it's super slow.
	// We were using C standard I/O, while that's super fast, it's not fast enough, so we're using a custom I/O system now.

	// The following was part of the previous system with C standard I/O.
		//char stdout_buffer[BUFSIZ];
		//setbuffer(stdout, stdout_buffer, BUFSIZ);
		//char stdin_buffer[BUFSIZ];
		//setbuffer(stdin, stdin_buffer, BUFSIZ);

		// NOTE: The above works, but it isn't a standard part of the standard library and isn't supported in
		// Windows. We do it like this instead.
		// NOTE: Isn't a problem since setbuffer is just an alias for setvbuf(fd, buf, buf ? _IOFBF : _IONBF, size)
		// anyway.
		//char stdin_buffer[std::numeric_limits<uint16_t>::max()];
		//std::setvbuf(stdin, stdin_buffer, _IOFBF, sizeof(stdin_buffer));
		//char stdout_buffer[std::numeric_limits<uint16_t>::max()];
		//std::setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer));

		// NOTE: One would think that BUFSIZ is the default buffer size for C buffered I/O.
		// Apparently it isn't, because the above yields performance improvements when explicitly setting to BUFSIZ.
		// NOTE: Right now, we are however setting it 65536, because that's a better number.
		// There are no downsides to making the number larger for this use-case, and 65536 works a lot better because
		// most pipes are that big.

	int normalArgIndex = manageArgs(argc, argv);
	outputSource(argv[normalArgIndex]);

	// The following was part of the previous system with C standard I/O.
		// NOTE: We have to explicitly close stdin and stdout because we can't let them automatically close
		// after stdin_buffer and stdout_buffer have already been freed. fclose will try to flush remaining
		// data and that's very iffy if the buffers have already been released. Potential seg fault and all that.
		// That's why we explicitly close them here.
		// The standard says that we don't have to worry about other code closing them after we've already closed them
		// here, which would be bad. I assume whatever code comes after us checks if they are open before trying to close
		// them.
		// TODO: Look up how buffered I/O is implemented in C.
		//fclose(stdout);
		//fclose(stdin);

	stdin_stream::dispose();
	stdout_stream::dispose();
}
