#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE, as well as most other syscalls

#ifndef PLATFORM_WINDOWS

#include <sys/mman.h>		// for mmap(), munmap() and posix_madvise() support
#include <sys/stat.h>		// for fstat() support
#include <fcntl.h>		// for posix_fadvise() support

#endif

#include <cstring>		// for std::strcmp() and std::memcpy()

#include <cstdio>		// we use just a tiny bit of C stdio because we use normal printf in one or two places

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
	// NOTE: It would've been cool to do something with the halt instruction, even though it's not necessary, but that would be x86-specific.
	// Seems unnecessary to reduce cross-platformity for basically no reason, since std::_Exit shouldn't fail.
	std::_Exit(exit_code);
	while (true) { }		// NOTE: If it does fail for some crazy reason, loop forever to prevent returning.
	// NOTE: This will also keep CPU usage very high so someone looking at htop will know somethings wrong if the process was supposed to end but now it's at 100%.
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
	const size_t rounded_bufferSize = bufferSize + huge_page_size - (((bufferSize - 1) % huge_page_size) + 1);
	// NOTE: Sadly, the above rounding mechanism seems to be as efficient as we can get it. I reckon assembly could do it faster, the compiler will almost definitely optimize if that's the case.

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

// TODO: I can't find this anywhere online, are function parameters aligned to their natural alignment when they are passed (assuming they are passed on the stack)?
template <const auto& initial_printf_pattern, const auto& printf_pattern, const auto& single_printf_pattern, unsigned char... chunk_indices>
DataTransferExitCode dataMode_mmap_vmsplice(size_t stdinFileSize) noexcept {
	constexpr size_t max_printf_write_length = calculate_max_printf_write_length(printf_pattern.data);
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
	if (stdoutPipeBufferSize == -1) { return DataTransferExitCode::NEEDS_FALLBACK; }

	struct iovec stdoutBufferMemorySpan_entireLength;
	stdoutBufferMemorySpan_entireLength.iov_len = stdoutPipeBufferSize;
	struct iovec stdoutBufferMemorySpan;

	// NOTE: We separate buffers to avoid cache contention.
	/*
	   It works like this as far as I understand at the moment:
	   	- if we just did "char stdoutBuffers[stdoutPipeBufferSize * 2]", that wouldn't be as efficient since
			the stack-frame itself isn't guaranteed to be aligned to a cacheline (it's alignment is standardized most of the time I think, but it depends on the architecture and defo isn't 64 bytes).
			Even if it were, the previous variable declarations would mess it up anyway for us. Basically, what I'm saying is "stdoutBuffers" wouldn't be aligned to a cacheline most of the time.
				- That would cause more cache lines to be used than necessary, but it would also cause one cacheline to straddle the junction between the two buffers, which is bad.
				--> we give one buffer at a time to the kernel, which gets played with from a different thread I believe, so it wouldn't be uncommon that the straddling cacheline
					is pulled into two different caches at the same time ---> CACHE CONTENTION!
					--> Not good because now both threads have to do complex locking stuff just to avoid stepping on eachother's toes when writing to the cache line. At least that would be logical.

		- obviously this would not be a concern if char was 64 bytes big (size of cache line, at least thats what I'm assuming for this example), since then the alignment would be forced, but it isn't.
		- to avoid all this, we put two pointers on the stack and have mmap allocate our buffers for us. They will probably be right after one another in memory in the case of this program, but that doesn't matter.
			--> the point is that they will be aligned to at least the cache boundaries (since they will both start at page starts, which are definitely sufficiently aligned)
				--> this means efficient cache usage but ALSO THAT NO CACHELINES WILL STRADDLE THE BUFFERS, so we can rest easy.

		// NOTE: I admit, we could have just had one pointer in this case and allocate everything with one mmap call, since that would probably still have aligned to cachlines properly.
		// But I think this system is better anyway since if the user was ever stupid enough to make the buffers weird non-powers-of-two sizes, this would mitigate the damage by still preventing cache contention,
		// whereas the one pointer method would not.
		// I don't even know if the one pointer method would work since vmsplice might require page-aligned memory input or something.
	*/
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
	if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

	while (true) {
		while (amountOfBufferFilled <= stdoutPipeBufferSize - max_printf_write_length) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, single_printf_pattern.data, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				tempBuffer_head = amountOfBufferFilled % pagesize;
				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) { REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE); }

				if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
				}

				if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdin file", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, printf_pattern.data, stdinFileData[stdinFileDataPosition + chunk_indices]...);
			if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
			stdinFileDataPosition += bytes_per_chunk;
			amountOfBufferFilled += bytesWritten;
		}

		// NOTE: There is a possibility that changing all this code to work with more pointers instead of offsets into arrays would be better, but it looks fine to me right now.
		// I don't reckon we would change that much by doing that, I'm betting it would be pretty much exactly as fast in this case.
		// Anyway, I'm confident enough that I'm not going to try rewriting it with pointers in order to check my theory. That would be a pain if I'm being honest.

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, single_printf_pattern.data, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);
					amountOfBufferFilled += tempBuffer_head;

					tempBuffer_head = amountOfBufferFilled % pagesize;
					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) {
						REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
					}

					if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
						REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
					}

					if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdin file", EXIT_FAILURE); }
					if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
					if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_GIFT) == -1) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
				}

				if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdin file", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap((unsigned char*)stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				if (!stdout_stream::write(tempBuffer + tempBuffer_tail, amountOfBufferFilled)) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, printf_pattern.data, stdinFileData[stdinFileDataPosition + chunk_indices]...);
			if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
			stdinFileDataPosition += bytes_per_chunk;
			tempBuffer_head += bytesWritten;
		}

		std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

		stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
		// TODO: Future improvement possibility:
		// You could have a separate thread and have it run vmsplice when signalled by this thread.
		// By doing the vmsplice call asynchronously, this code doesn't have to wait for vmsplice to
		// finish translating vm to physical mem. That would make everything a little bit faster presumably (at least in situations where the entity
		// reading our stdout is less of a bottleneck than we are).
		// You would just have to replace each vmsplice call with a call to a custom function, not that hard.
		if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_MORE) == -1) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
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

	if (meta_printf_no_terminator(initial_printf_pattern.data, stdinFileData[0]) == -1) { REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE); }

	size_t i;
	for (i = 1; i < stdinFileSize + 1 - bytes_per_chunk; i += bytes_per_chunk) {
		if (meta_printf_no_terminator(printf_pattern.data, stdinFileData[i + chunk_indices]...) == -1) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE);
		}
	}
	for (; i < stdinFileSize; i++) {
		if (meta_printf_no_terminator(single_printf_pattern.data, stdinFileData[i]) == -1) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE);
		}
	}

	if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdin file", EXIT_FAILURE); }

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

	char* stdoutBuffers[2];

	const ssize_t actual_pipe_buffer_size = mmapWriteDoubleBuffer(stdoutBuffers[0], stdoutBuffers[1], stdoutPipeBufferSize);
	if (actual_pipe_buffer_size == -1) { return DataTransferExitCode::NEEDS_FALLBACK_FROM_MMAP; }

	bool stdoutBufferToggle = false;
	char* currentStdoutBuffer = stdoutBuffers[0];

	char tempBuffer[max_printf_write_length * 2];
	size_t tempBuffer_head = 0;
	size_t tempBuffer_tail = 0;

	char inputBuffer[bytes_per_chunk];
	stdin_stream::data_ptr_return_t data_ptr = stdin_stream::get_data_ptr(inputBuffer, 1);
	if (!data_ptr.data_ptr) { REPORT_ERROR_AND_EXIT("failed to read from stdin: stdin_stream::get_data_ptr failed", EXIT_FAILURE); }
	if (data_ptr.size == 0) { return DataTransferExitCode::NO_INPUT_DATA; }

	int bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer, initial_printf_pattern.data, (uint8_t)data_ptr.data_ptr[0]);
	if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

	while (true) {
		while (amountOfBufferFilled <= stdoutPipeBufferSize - max_printf_write_length) {
			data_ptr = stdin_stream::get_data_ptr(inputBuffer, bytes_per_chunk);
			if (!data_ptr.data_ptr) { REPORT_ERROR_AND_EXIT("failed to read from stdin: stdin_stream::get_data_ptr failed", EXIT_FAILURE); }

			if (data_ptr.size < bytes_per_chunk) {
				for (unsigned char i = 0; i < data_ptr.size; i++) {
					bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, single_printf_pattern.data, (uint8_t)data_ptr.data_ptr[i]);
					if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				tempBuffer_head = amountOfBufferFilled % pagesize;
				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) { REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE); }

				if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
				}

				if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(currentStdoutBuffer + amountOfBufferFilled, printf_pattern.data, (uint8_t)data_ptr.data_ptr[chunk_indices]...);
			if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
			amountOfBufferFilled += bytesWritten;
		}

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			data_ptr = stdin_stream::get_data_ptr(inputBuffer, bytes_per_chunk);
			if (!data_ptr.data_ptr) { REPORT_ERROR_AND_EXIT("failed to read from stdin: stdin_stream::get_data_ptr failed", EXIT_FAILURE); }

			if (data_ptr.size < bytes_per_chunk) {
				for (unsigned char i = 0; i < data_ptr.size; i++) {
					bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, single_printf_pattern.data, (uint8_t)data_ptr.data_ptr[i]);
					if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);
					amountOfBufferFilled += tempBuffer_head;

					tempBuffer_head = amountOfBufferFilled % pagesize;
					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled - tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, SPLICE_F_GIFT) == -1) {
						REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
					}

					if (!stdout_stream::write(currentStdoutBuffer + stdoutBufferMemorySpan.iov_len, tempBuffer_head)) {
						REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
					}

					if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
					if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_GIFT) == -1) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
				}

				// NOTE: munmap after SPLICE_F_GIFT is okay, don't worry.
				if (munmap(stdoutBuffers[0], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }
				if (munmap(stdoutBuffers[1], actual_pipe_buffer_size) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap stdout buffer", EXIT_FAILURE); }

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				if (!stdout_stream::write(tempBuffer + tempBuffer_tail, amountOfBufferFilled)) {
					REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = meta_sprintf_no_terminator(tempBuffer + tempBuffer_head, printf_pattern.data, (uint8_t)data_ptr.data_ptr[chunk_indices]...);
			if (bytesWritten == -1) { REPORT_ERROR_AND_EXIT("failed to process data: meta_sprintf_no_terminator failed", EXIT_FAILURE); }
			tempBuffer_head += bytesWritten;
		}

		std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

		stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
		if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, SPLICE_F_MORE) == -1) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: vmsplice failed", EXIT_FAILURE);
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

	char buffer[bytes_per_chunk];

	stdin_stream::data_ptr_return_t data_ptr = stdin_stream::get_data_ptr(buffer, 1);
	if (!data_ptr.data_ptr) { REPORT_ERROR_AND_EXIT("failed to read from stdin: stdin_stream:get_data_ptr failed", EXIT_FAILURE); }
	if (data_ptr.size == 0) { return false; }

	if (meta_printf_no_terminator(initial_printf_pattern.data, (unsigned char)data_ptr.data_ptr[0]) == -1) { REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE); }

	while (true) {
		stdin_stream::data_ptr_return_t data_ptr = stdin_stream::get_data_ptr(buffer, bytes_per_chunk);

		if (data_ptr.size == bytes_per_chunk) {
			if (meta_printf_no_terminator(printf_pattern.data, (unsigned char)data_ptr.data_ptr[chunk_indices]...) == -1) {
				REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE);
			}
			continue;
		}

		if (!data_ptr.data_ptr) {
			REPORT_ERROR_AND_EXIT("failed to read from stdin: stdin_stream:get_data_ptr failed", EXIT_FAILURE);
		}

		for (unsigned char i = 0; i < data_ptr.size; i++) {
			if (meta_printf_no_terminator(single_printf_pattern.data, (unsigned char)data_ptr.data_ptr[i]) == -1) {
				REPORT_ERROR_AND_EXIT("failed to output to stdout: meta_printf_no_terminator failed", EXIT_FAILURE);
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

// SIDE-NOTE: No reinterpret_cast's allowed in constant expressions, seems restrictive, and it is, but it's got a pretty reasonable explanation:
// 		--> reinterpretation relies on how the types are represented, which is implementation defined in a lot of cases AFAIK.
//		--> you could standardize the way they look when running consteval functions, but that would mean that they would look one way
//			in the rest of your code and another in the consteval functions, which is probably why they didn't do that.
//		--> not to mention it would complicate the writing of constexpr functions, since the behaviour could change between runtime and compile-time.
template <const auto& single_printf_pattern, unsigned char... chunk_indices>
consteval auto generate_chunked_printf_pattern() {
	meta::meta_string<sizeof...(chunk_indices) * (sizeof(single_printf_pattern) - 1) + 1> result;
	for (size_t i = 0; i < sizeof(result) - 1; i += sizeof(single_printf_pattern) - 1) {
		for (size_t j = 0; j < sizeof(single_printf_pattern) - 1; j++) {
			result[i + j] = single_printf_pattern[j];
		}
	}
	result[sizeof(result) - 1] = '\0';
	return result;
}

#define optimizedDataTransformationAndOutput(initialPrintfPattern, singlePrintfPattern, ...) [&]() { static constexpr auto initial_printf_pattern = meta::construct_meta_array(initialPrintfPattern); static constexpr auto single_printf_pattern = meta::construct_meta_array(singlePrintfPattern); static constexpr auto printf_pattern = generate_chunked_printf_pattern<single_printf_pattern, __VA_ARGS__>(); return optimizedDataTransformationAndOutput_raw<initial_printf_pattern, printf_pattern, single_printf_pattern, __VA_ARGS__>(); }()

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

#define COUNT_TO_31_FROM_0 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31

void output_C_CPP_array_data() noexcept {
	if (optimizedDataTransformationAndOutput("%u", ", %u", COUNT_TO_31_FROM_0) == false) {
		REPORT_ERROR_AND_EXIT("no data received, language requires data", EXIT_FAILURE);
	}
}

template <size_t output_size>
void writeOutput(const char (&output)[output_size]) noexcept {
	if (!stdout_stream::write(output, output_size - sizeof(char))) {
		REPORT_ERROR_AND_EXIT("failed to output to stdout: stdout_stream::write failed", EXIT_FAILURE);
	}
}

void initialize_streams() noexcept {
	if (!stdin_stream::initialize()) { REPORT_ERROR_AND_EXIT("failed to initialize stdin stream: stdin_stream::initialize failed", EXIT_FAILURE); }
	stdout_stream::initialize();
}

void outputSource(const char* language) noexcept {
	if (std::strcmp(language, "c++") == 0) {
		if (std::printf("const char %s[] { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: std::printf failed", EXIT_FAILURE);
		}
		if (fflush(stdout) == EOF) {
			REPORT_ERROR_AND_EXIT("failed to flush stdout: fflush failed", EXIT_FAILURE);
		}
		initialize_streams();
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		if (std::printf("const char %s[] = { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to output to stdout: std::printf failed", EXIT_FAILURE);
		}
		if (fflush(stdout) == EOF) {
			REPORT_ERROR_AND_EXIT("failed to flush stdout: fflush failed", EXIT_FAILURE);
		}
		initialize_streams();
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}

	REPORT_ERROR_AND_EXIT("invalid language", EXIT_SUCCESS);
}

int main(int argc, const char* const * argv) noexcept {
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

// TODO: Why is it that this pipeline: yes | cpipe -vt | ./bin/srcembed c++ | cat > /dev/null is faster than this pipeline: yes | cpipe -vt | ./bin/srcembed c++ > /dev/null?
