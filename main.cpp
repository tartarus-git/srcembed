#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE, as well as every other syscall
#ifndef PLATFORM_WINDOWS
#include <unistd.h>		// for raw I/O
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>		// for posix_fadvise() support
#else
#include <io.h>			// for Windows raw I/O
#endif
#include <cstdio>		// for buffered I/O
#include <cstring>		// for std::strcmp()
#include <cerrno>
#include <cstdint>

#ifdef PLATFORM_WINDOWS
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define write(fd, buf, count) _write(fd, buf, count)
#endif

const char helpText[] = "usage: srcembed <--help> || ([--varname <variable name>] <language>)\n" \
			"\n" \
			"function: converts input byte stream into source file (output through stdout)\n" \
			"\n" \
			"arguments:\n" \
				"\t[--help]                      --> displays help text\n" \
				"\t[--varname <variable name>]   --> specifies the variable name by which the embedded file shall be referred to in code\n" \
				"\t<language>                    --> specifies the source language\n" \
			"\n" \
			"supported languages (possible inputs for <language> field):\n" \
				"\tc++\n" \
				"\tc\n";

template <size_t message_length>
void writeErrorAndExit(const char (&message)[message_length], int exitCode) noexcept {
	write(STDERR_FILENO, message, message_length);
	std::exit(exitCode);
}

#define REPORT_ERROR_AND_EXIT(message, exitCode) writeErrorAndExit("ERROR: " message "\n", exitCode)

bool mmapWriteDoubleBuffer(char*& bufferA, char*& bufferB, size_t bufferSize) noexcept {
	// TODO: Consider picking the best huge page size for the job dynamically.
	bufferA = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_HUGETLB, -1, 0);
	if (bufferA == MAP_FAILED) {
		bufferA = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (bufferA == MAP_FAILED) { return false; }

		bufferB = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (bufferB == MAP_FAILED) { return false; }

		return true;
	}

	bufferB = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_HUGETLB, -1, 0);
	if (bufferB == MAP_FAILED) {
		bufferB = (char*)mmap(nullptr, bufferSize, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (bufferB == MAP_FAILED) { return false; }
	}

	return true;
}

const unsigned char* mmapStdinFile(size_t stdinFileSize) noexcept {
	// TODO: Put huge pages in this. That would make sense right?
	unsigned char* stdinFileData = (unsigned char*)mmap(nullptr, stdinFileSize, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_POPULATE, STDIN_FILENO, 0);

	// NOTE: I'm pretty sure these don't overwrite each other, but just in case, I put the more important one second.
	posix_madvise(stdinFileData, stdinFileSize, POSIX_MADV_WILLNEED);
	posix_madvise(stdinFileData, stdinFileSize, POSIX_MADV_SEQUENTIAL);

	return stdinFileData;
}

enum class DataTransferExitCode {
	SUCCESS,
	NEEDS_FALLBACK,
	NO_INPUT_DATA
};

// TODO: Go through whole code and make sure you release resources when you exit gracefully (error exit doesn't have to I guess).

template <size_t max_printf_write_length, unsigned char... chunk_indices>
DataTransferExitCode dataMode_mmap_vmsplice(const char* initialPrintfPattern, const char* printfPattern, const char* singlePrintfPattern, size_t stdinFileSize) noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
	if (stdoutPipeBufferSize == -1) { return DataTransferExitCode::NEEDS_FALLBACK; }
	int stdoutBufferSize = stdoutPipeBufferSize + 1;

	struct iovec stdoutBufferMemorySpan_entireLength;
	stdoutBufferMemorySpan_entireLength.iov_len = stdoutPipeBufferSize;
	struct iovec stdoutBufferMemorySpan;

	// NOTE: We separate buffers to avoid cache contention. TODO: Figure out how that works.
	char* stdoutBuffers[2];

	if (mmapWriteDoubleBuffer(stdoutBuffers[0], stdoutBuffers[1], stdoutBufferSize) == false) { return DataTransferExitCode::NEEDS_FALLBACK; }

	bool stdoutBufferToggle = false;
	char* currentStdoutBuffer = stdoutBuffers[0];

	char tempBuffer[max_printf_write_length * 2];
	size_t tempBuffer_head = 0;
	size_t tempBuffer_tail = 0;

	const unsigned char* stdinFileData = mmapStdinFile(stdinFileSize);
	if (stdinFileData == MAP_FAILED) { return DataTransferExitCode::NEEDS_FALLBACK; }
	size_t stdinFileDataCutoff = stdinFileSize - bytes_per_chunk;
	size_t stdinFileDataPosition = 1;

	int bytesWritten = sprintf(currentStdoutBuffer, initialPrintfPattern, stdinFileData[0]);
	if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

	while (true) {
		while (amountOfBufferFilled < stdoutBufferSize - max_printf_write_length) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = sprintf(currentStdoutBuffer + amountOfBufferFilled, singlePrintfPattern, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled;	// TODO: Make sure this doesn't mess things up when it's 0.
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, 0) == -1) { REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE); }

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = sprintf(currentStdoutBuffer + amountOfBufferFilled, printfPattern, stdinFileData[stdinFileDataPosition + chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			stdinFileDataPosition += 8;
			amountOfBufferFilled += bytesWritten;
		}

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			if (stdinFileDataPosition > stdinFileDataCutoff) {
				for (; stdinFileDataPosition < stdinFileSize; stdinFileDataPosition++) {
					bytesWritten = sprintf(tempBuffer + tempBuffer_head, singlePrintfPattern, stdinFileData[stdinFileDataPosition]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);

					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled + tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, 0) == -1) {
						REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
					}

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, 0) == -1) {
					REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
				}

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				if (fwrite(tempBuffer + tempBuffer_tail, sizeof(char), amountOfBufferFilled, stdout) < amountOfBufferFilled) {
					if (ferror(stdout)) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = sprintf(tempBuffer + tempBuffer_head, printfPattern, stdinFileData[stdinFileDataPosition + chunk_indices]...);
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

// TODO: You could make this one return bool as well since it only has two states.
template <size_t max_printf_write_length, size_t... chunk_indices>
DataTransferExitCode dataMode_mmap_write(const char* initialPrintfPattern, const char* printfPattern, const char* singlePrintfPattern, size_t stdinFileSize) noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	const unsigned char* stdinFileData = mmapStdinFile(stdinFileSize);
	if (stdinFileData == MAP_FAILED) { return DataTransferExitCode::NEEDS_FALLBACK; }

	if (printf(initialPrintfPattern, stdinFileData[0]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
	size_t i;
	for (i = 1; i < stdinFileSize + 1 - bytes_per_chunk; i += bytes_per_chunk) {
		if (printf(printfPattern, stdinFileData[i + chunk_indices]...) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
	}
	for (; i < stdinFileSize; i++) {
		if (printf(singlePrintfPattern, stdinFileData[i]) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
	}

	if (munmap((unsigned char*)stdinFileData, stdinFileSize) == -1) { REPORT_ERROR_AND_EXIT("failed to munmap input file", EXIT_FAILURE); }
	return DataTransferExitCode::SUCCESS;
}

template <size_t max_printf_write_length, unsigned char... chunk_indices>
DataTransferExitCode dataMode_read_vmsplice(const char* initialPrintfPattern, const char* printfPattern, const char* singlePrintfPattern) noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
	if (stdoutPipeBufferSize == -1) { return DataTransferExitCode::NEEDS_FALLBACK; }
	int stdoutBufferSize = stdoutPipeBufferSize + 1;

	struct iovec stdoutBufferMemorySpan_entireLength;
	stdoutBufferMemorySpan_entireLength.iov_len = stdoutPipeBufferSize;
	struct iovec stdoutBufferMemorySpan;

	// NOTE: We separate buffers to avoid cache contention. TODO: Figure out how that works.
	char* stdoutBuffers[2];

	if (mmapWriteDoubleBuffer(stdoutBuffers[0], stdoutBuffers[1], stdoutBufferSize) == false) { return DataTransferExitCode::NEEDS_FALLBACK; }

	bool stdoutBufferToggle = false;
	char* currentStdoutBuffer = stdoutBuffers[0];

	char tempBuffer[max_printf_write_length * 2];
	size_t tempBuffer_head = 0;
	size_t tempBuffer_tail = 0;

	unsigned char inputBuffer[bytes_per_chunk];
	if (fread(inputBuffer, sizeof(char), 1, stdin) == 0) {
		if (ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }
		return DataTransferExitCode::NO_INPUT_DATA;
	}

	int bytesWritten = sprintf(currentStdoutBuffer, initialPrintfPattern, inputBuffer[0]);
	if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
	size_t amountOfBufferFilled = bytesWritten;

#define DEBUG(string) fprintf(stderr, string "\n")

	while (true) {
		while (amountOfBufferFilled < stdoutBufferSize - max_printf_write_length) {
			unsigned char bytesRead = fread(inputBuffer, sizeof(char), bytes_per_chunk, stdin);
			if (bytesRead < bytes_per_chunk) {
				if (ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }

				for (unsigned char i = 0; i < bytesRead; i++) {
					bytesWritten = sprintf(currentStdoutBuffer + amountOfBufferFilled, singlePrintfPattern, inputBuffer[i]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					amountOfBufferFilled += bytesWritten;
				}

				stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
				stdoutBufferMemorySpan.iov_len = amountOfBufferFilled;	// TODO: Make sure this doesn't mess things up when it's 0.
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, 0) == -1) { REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE); }

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = sprintf(currentStdoutBuffer + amountOfBufferFilled, printfPattern, inputBuffer[chunk_indices]...);
			if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
			amountOfBufferFilled += bytesWritten;
		}

		tempBuffer_tail = stdoutPipeBufferSize - amountOfBufferFilled;
		for (tempBuffer_head = 0; tempBuffer_head < tempBuffer_tail;) {
			unsigned char bytesRead = fread(inputBuffer, sizeof(char), bytes_per_chunk, stdin);
			if (bytesRead < bytes_per_chunk) {
				if (ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }

				for (unsigned char i = 0; i < bytesRead; i++) {
					bytesWritten = sprintf(tempBuffer + tempBuffer_head, singlePrintfPattern, inputBuffer[i]);
					if (bytesWritten < 0) { REPORT_ERROR_AND_EXIT("sprintf failed", EXIT_FAILURE); }
					tempBuffer_head += bytesWritten;
				}

				if (tempBuffer_head <= tempBuffer_tail) {
					std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_head);

					stdoutBufferMemorySpan.iov_base = currentStdoutBuffer;
					stdoutBufferMemorySpan.iov_len = amountOfBufferFilled + tempBuffer_head;
					if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan, 1, 0) == -1) {
						REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
					}

					return DataTransferExitCode::SUCCESS;
				}

				std::memcpy(currentStdoutBuffer + amountOfBufferFilled, tempBuffer, tempBuffer_tail);

				stdoutBufferMemorySpan_entireLength.iov_base = currentStdoutBuffer;
				if (vmsplice(STDOUT_FILENO, &stdoutBufferMemorySpan_entireLength, 1, 0) == -1) {
					REPORT_ERROR_AND_EXIT("vmsplice failed", EXIT_FAILURE);
				}

				amountOfBufferFilled = tempBuffer_head - tempBuffer_tail;
				if (fwrite(tempBuffer + tempBuffer_tail, sizeof(char), amountOfBufferFilled, stdout) < amountOfBufferFilled) {
					if (ferror(stdout)) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
				}

				return DataTransferExitCode::SUCCESS;
			}

			bytesWritten = sprintf(tempBuffer + tempBuffer_head, printfPattern, inputBuffer[chunk_indices]...);
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

template <unsigned char... chunk_indices>
DataTransferExitCode dataMode_read_write(const char* initialPrintfPattern, const char* printfPattern, const char* singlePrintfPattern) noexcept {
	constexpr unsigned char bytes_per_chunk = sizeof...(chunk_indices);

	if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_NOREUSE) == 0) {
		if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_WILLNEED) == 0) {
			posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL);
		}
	}

	unsigned char buffer[bytes_per_chunk];

	// NOTE: fread shouldn't ever return less than the wanted amount of bytes unless either:
	// a) EOF
	// b) an error occurred
	// In this way, it is very different to the raw I/O (read and write).
	if (std::fread(buffer, sizeof(char), 1, stdin) == 0) {
		if (std::ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }
		return DataTransferExitCode::NO_INPUT_DATA;
	}
	if (std::printf(initialPrintfPattern, buffer[0]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }

	while (true) {
		unsigned char bytesRead = std::fread(buffer, sizeof(char), bytes_per_chunk, stdin);

		if (bytesRead == bytes_per_chunk) {
			if (std::printf(printfPattern, buffer[chunk_indices]...) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
			continue;
		}

		if (std::ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }

		for (unsigned char i = 0; i < bytesRead; i++) {
			if (std::printf(singlePrintfPattern, buffer[i]) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
		}
		return DataTransferExitCode::SUCCESS;
	}
}

template <size_t max_printf_write_length, unsigned char... chunk_indices>
bool optimizedDataTransformationAndOutput_raw(const char* initialPrintfPattern, const char* printfPattern, const char* singlePrintfPattern) noexcept {
	static_assert(sizeof...(chunk_indices) != 0, "parameter pack \"chunk_indices\" must contain at least 1 element");
	static_assert(sizeof...(chunk_indices) < 256, "parameter pack \"chunk_indices\" must contain less than 256 elements");

	struct stat statusA;
	if (fstat(STDIN_FILENO, &statusA) == 0) {
		if (S_ISREG(statusA.st_mode)) {
			struct stat statusB;
			if (fstat(STDOUT_FILENO, &statusB) == 0) {
				if (S_ISFIFO(statusB.st_mode)) {
					if (statusA.st_size == 0) { return false; }
					if (sizeof(size_t) >= sizeof(off_t) || statusA.st_size <= (size_t)-1) {
						switch (dataMode_mmap_vmsplice<max_printf_write_length, chunk_indices...>(initialPrintfPattern, printfPattern, singlePrintfPattern, statusA.st_size)) {
						case DataTransferExitCode::SUCCESS: return true;
						case DataTransferExitCode::NEEDS_FALLBACK: break;	// TODO: Make sure it makes sense to fallback into another mmap. Where do the fallback requests come from in the function code?
						}
					}
				}
			}

			if (statusA.st_size == 0) { return false; }
			// NOTE: If size_t is 32-bit and linux large file extention is enabled (off_t is 64-bit),
			// only allow mmapping if file length can fit into size_t.
			if (sizeof(size_t) >= sizeof(off_t) && statusA.st_size <= (size_t)-1) {
				switch (dataMode_mmap_write<chunk_indices...>(initialPrintfPattern, printfPattern, singlePrintfPattern, statusA.st_size)) {
				case DataTransferExitCode::SUCCESS: return true;
				case DataTransferExitCode::NEEDS_FALLBACK: break;
				}
			}

			switch (dataMode_read_write<chunk_indices...>(initialPrintfPattern, printfPattern, singlePrintfPattern)) {
			case DataTransferExitCode::SUCCESS: return true;
			case DataTransferExitCode::NO_INPUT_DATA: return false;
			}
			// TODO: You could replace the above with bool return, since it only ever has two states.
		}
	}

	if (fstat(STDOUT_FILENO, &statusA) == 0) {
		if (S_ISFIFO(statusA.st_mode)) {
			switch (dataMode_read_vmsplice<max_printf_write_length, chunk_indices...>(initialPrintfPattern, printfPattern, singlePrintfPattern)) {
			case DataTransferExitCode::SUCCESS: return true;
			case DataTransferExitCode::NO_INPUT_DATA: return false;
			case DataTransferExitCode::NEEDS_FALLBACK: break;
			}
		}
	}

	switch (dataMode_read_write<chunk_indices...>(initialPrintfPattern, printfPattern, singlePrintfPattern)) {
	case DataTransferExitCode::SUCCESS: return true;
	case DataTransferExitCode::NO_INPUT_DATA: return false;
	}
}

template <size_t pattern_size>
consteval size_t calculate_max_printf_write_length(const char (&pattern)[pattern_size]) {
	size_t result = pattern_size - 1;
	for (size_t i = 0; i < pattern_size; i++) {
		if (pattern[i] == '%') { result++; }
	}
	return result;
}

#define optimizedDataTransformationAndOutput(initialPrintfPattern, printfPattern, singlePrintfPattern, ...) optimizedDataTransformationAndOutput_raw<calculate_max_printf_write_length(printfPattern), __VA_ARGS__>(initialPrintfPattern, printfPattern, singlePrintfPattern)

void output_C_CPP_array_data() noexcept {
	optimizedDataTransformationAndOutput("%u", ", %u, %u, %u, %u, %u, %u, %u, %u", ", %u", 0, 1, 2, 3, 4, 5, 6, 7);
}


/*
void output_C_CPP_array_data() noexcept {
#ifndef PLATFORM_WINDOWS
	struct stat stdinStatus;
	if (fstat(STDIN_FILENO, &stdinStatus) == 0) {
		if (S_ISREG(stdinStatus.st_mode)) {
			struct stat stdoutStatus;
			if (fstat(STDOUT_FILENO, &stdoutStatus) == 0) {
				if (S_ISFIFO(stdoutStatus.st_mode)) {
					int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
					fprintf(stderr, "pipe buffer size: %i\n", stdoutPipeBufferSize);
					struct iovec memdata;

#define MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT ((3 + 2) * 8)		// , 255, 255, 255, 255 .... 8x = (3 + 2) * 8

					char tempBuffer[MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT * 2];
					size_t tempBufferPtr = 0;
					size_t tempBufferEaten = 0;
				
					off_t i = 0;

						// TODO: Make this use big pages for efficiency.
						char* buffer1 = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_PRIVATE | MAP_ANON, // NO support for: | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT),
										     -1,
										     0);
						if (buffer1 == MAP_FAILED) {
							fprintf(stderr, "%i", errno);
							REPORT_ERROR_AND_EXIT("testing1231", EXIT_FAILURE); }

						char* buffer2 = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_PRIVATE | MAP_ANON, // NO support for:  | MAP_HUGETLB,
										     -1,
										     0);
						if (buffer2 == MAP_FAILED) { REPORT_ERROR_AND_EXIT("testing123", EXIT_FAILURE); }

						char* buffer;

						bool counter = false;

					while (true) {
do_it_all_over_again:

						counter = !counter;
						if (counter) {
							buffer = buffer1;
						} else {
							buffer = buffer2;
						}



						//write(STDERR_FILENO, "took\n", 5);


						std::memcpy(buffer, tempBuffer + tempBufferEaten, tempBufferPtr - tempBufferEaten);

						size_t amountOfBufferFilled = tempBufferPtr - tempBufferEaten;
						amountOfBufferFilled += sprintf(buffer + amountOfBufferFilled, "%u", stdinFileData[i]) - 1;
						i++;

						tempBufferPtr = 0;
						tempBufferEaten = 0;



						for (; i < stdinStatus.st_size - 8; i += 8) {
							//fprintf(stderr, "took for loop iteration\n");


							amountOfBufferFilled += sprintf(buffer + amountOfBufferFilled, ", %u, %u, %u, %u, %u, %u, %u, %u", 
					   			stdinFileData[i], stdinFileData[i + 1], stdinFileData[i + 2], stdinFileData[i + 3], 
					   			stdinFileData[i + 4], stdinFileData[i + 5], stdinFileData[i + 6], stdinFileData[i + 7]) - 1;

							if (amountOfBufferFilled > stdoutPipeBufferSize - MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT) {
					
								while (true) {
									tempBufferPtr += sprintf(tempBuffer + tempBufferPtr, ", %u, %u, %u, %u, %u, %u, %u, %u", 
					   				stdinFileData[i], stdinFileData[i + 1], stdinFileData[i + 2], stdinFileData[i + 3], 
					   				stdinFileData[i + 4], stdinFileData[i + 5], stdinFileData[i + 6], stdinFileData[i + 7]) - 1;
									
									if (tempBufferPtr >= stdoutPipeBufferSize - amountOfBufferFilled) { break; }
								}				

								memcpy(buffer + amountOfBufferFilled, tempBuffer, stdoutPipeBufferSize - amountOfBufferFilled);
								tempBufferEaten = stdoutPipeBufferSize - amountOfBufferFilled;

								memdata.iov_base = buffer;
								memdata.iov_len = stdoutPipeBufferSize;
								if (vmsplice(STDOUT_FILENO, &memdata, 1, SPLICE_F_MORE) == -1) {
									// TODO: Is that the actual err det method?
									// YES.
									REPORT_ERROR_AND_EXIT("error with vmsplice", EXIT_FAILURE);
								}

								goto do_it_all_over_again;
							}
						}

						REPORT_ERROR_AND_EXIT("took forbidden branch 2", EXIT_FAILURE);

						for (; i < stdinStatus.st_size; i++) {
							// TODO: Come up with a better way for this one.
							if (printf(", %u", stdinFileData[i]) < 0) {
								REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
							}
						}

						munmap(stdinFileData, stdinStatus.st_size);
						return;
					}
				}

			if (printf("%u", stdinFileData[0]) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
			off_t i;
			for (i = 1; i < stdinStatus.st_size - 8; i += 8) {
				if (printf(", %u, %u, %u, %u, %u, %u, %u, %u", 
					   stdinFileData[i], stdinFileData[i + 1], stdinFileData[i + 2], stdinFileData[i + 3], 
					   stdinFileData[i + 4], stdinFileData[i + 5], stdinFileData[i + 6], stdinFileData[i + 7]) < 0) {
					REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
				}
			}
			for (; i < stdinStatus.st_size; i++) {
				if (printf(", %u", stdinFileData[i]) < 0) {
					REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
				}
			}

			munmap(stdinFileData, stdinStatus.st_size);
			return;
		}
		}

	struct stat stdoutStatus;
	if (fstat(STDOUT_FILENO, &stdoutStatus) == 0) {
		if (S_ISFIFO(stdoutStatus.st_mode)) {

			char stdinFileData[8];
			fread(stdinFileData, 1, 8, stdin);

					int stdoutPipeBufferSize = fcntl(STDOUT_FILENO, F_GETPIPE_SZ);

					struct iovec memdata;

#define MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT ((3 + 2) * 8)		// , 255, 255, 255, 255 .... 8x = (3 + 2) * 8

					char tempBuffer[MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT * 2];
					size_t tempBufferPtr = 0;
					size_t tempBufferEaten = 0;
				
						// TODO: Make this use big pages for efficiency.
						char* buffer1 = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_PRIVATE | MAP_ANON,
										     -1,
										     0);
						char* buffer2 = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_PRIVATE | MAP_ANON,
										     -1,
										     0);
						bool counter = false;
						char* buffer;
					while (true) {
do_it_all_over_again2:
						counter = !counter;
						if (counter) {
							buffer = buffer1;
						}
							else {
								buffer = buffer2;
							}


						std::memcpy(buffer, tempBuffer + tempBufferEaten, tempBufferPtr - tempBufferEaten);

						size_t amountOfBufferFilled = tempBufferPtr - tempBufferEaten;
						amountOfBufferFilled += sprintf(buffer + amountOfBufferFilled, "%u", stdinFileData[0]) - 1;


						tempBufferPtr = 0;
						tempBufferEaten = 0;


						ssize_t amountActuallyRead;
						while (true) {
							amountOfBufferFilled += sprintf(buffer + amountOfBufferFilled, ", %u, %u, %u, %u, %u, %u, %u, %u", 
					   			stdinFileData[0], stdinFileData[1], stdinFileData[2], stdinFileData[3], 
					   			stdinFileData[4], stdinFileData[5], stdinFileData[6], stdinFileData[7]) - 1;

							amountActuallyRead = fread(stdinFileData, 1, 8, stdin);
							if (amountActuallyRead < 8) { break; }

							if (amountOfBufferFilled > stdoutPipeBufferSize - MAX_POSSIBLE_PRINTF_MEMORY_OUTPUT && amountOfBufferFilled < stdoutPipeBufferSize) {
					
								while (true) {
									tempBufferPtr += sprintf(tempBuffer + tempBufferPtr, ", %u, %u, %u, %u, %u, %u, %u, %u", 
					   				stdinFileData[0], stdinFileData[1], stdinFileData[2], stdinFileData[3], 
					   				stdinFileData[4], stdinFileData[5], stdinFileData[6], stdinFileData[7]) - 1;

									fread(stdinFileData, 1, 8, stdin);
									
									if (tempBufferPtr >= stdoutPipeBufferSize - amountOfBufferFilled) { break; }
								}				

								memdata.iov_base = buffer;
								memdata.iov_len = stdoutPipeBufferSize;
								vmsplice(STDOUT_FILENO, &memdata, 1, SPLICE_F_MORE);

								goto do_it_all_over_again2;
							}
						}

						for (; amountActuallyRead < 8; amountActuallyRead++) {

							memdata.iov_base = buffer;
							memdata.iov_len = amountOfBufferFilled;
							vmsplice(STDOUT_FILENO, &memdata, 1, 0);




							// TODO: Come up with a better way for this one.
							if (printf(", %u", stdinFileData[amountActuallyRead]) < 0) {
								REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
							}
						}

						return;
					}
				}

		}
	}


use_normal_read_write_transfer:

}
*/

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
						if (write(STDOUT_FILENO, helpText, sizeof(helpText) - 1) == -1) {
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

template <size_t output_size>
void writeOutput(const char (&output)[output_size]) noexcept {
	if (fwrite(output, sizeof(char), output_size - sizeof(char), stdout) == -1) {
		REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
	}
}

void outputSource(const char* language) noexcept {
	if (std::strcmp(language, "c++") == 0) {
		if (std::printf("const char %s[] { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
		}
		if (fflush(stdout) == EOF) {
			REPORT_ERROR_AND_EXIT("failed to flush stdout", EXIT_FAILURE);
		}
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
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}

	REPORT_ERROR_AND_EXIT("invalid language", EXIT_SUCCESS);
}

int main(int argc, const char* const * argv) noexcept {
	// C++ standard I/O can suck it, it's super slow. We're using C standard I/O. Maybe I'll make a wrapper library for C++ eventually.

	//char stdout_buffer[BUFSIZ];
	//setbuffer(stdout, stdout_buffer, BUFSIZ);
	//char stdin_buffer[BUFSIZ];
	//setbuffer(stdin, stdin_buffer, BUFSIZ);

	// NOTE: The above works, but it isn't a standard part of the standard library and isn't supported in
	// Windows. We do it like this instead.
	// NOTE: Isn't a problem since setbuffer is just an alias for setvbuf(fd, buf, buf ? _IOFBF : _IONBF, size)
	// anyway.
	char stdin_buffer[BUFSIZ];
	std::setvbuf(stdin, stdin_buffer, _IOFBF, BUFSIZ);
	char stdout_buffer[BUFSIZ];
	std::setvbuf(stdout, stdout_buffer, _IOFBF, BUFSIZ);

	// NOTE: One would think that BUFSIZ is the default buffer size for C buffered I/O.
	// Apparently it isn't, because the above yields performance improvements.

	int normalArgIndex = manageArgs(argc, argv);
	outputSource(argv[normalArgIndex]);

	// NOTE: We have to explicitly close stdin and stdout because we can't let them automatically close
	// after stdin_buffer and stdout_buffer have already been freed. fclose will try to flush remaining
	// data and that's very iffy if the buffers have already been released. Potential seg fault and all that.
	// That's why we explicitly close them here.
	// The standard says that we don't have to worry about other code closing them after we've already closed them
	// here, which would be bad. I assume whatever code comes after us checks if they are open before trying to close
	// them.
	// TODO: Look up how buffered I/O is implemented in C.
	fclose(stdout);
	fclose(stdin);
}
