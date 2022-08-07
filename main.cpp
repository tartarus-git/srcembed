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

void output_C_CPP_array_data() noexcept {
#ifndef PLATFORM_WINDOWS
	struct stat stdinStatus;
	if (fstat(STDIN_FILENO, &stdinStatus) == 0) {
		if (S_ISREG(stdinStatus.st_mode)) {
			if (stdinStatus.st_size == 0) {
				REPORT_ERROR_AND_EXIT("no data received, language requires data", EXIT_SUCCESS);
			}

			// NOTE: If size_t is 32-bit and linux large file extention is enabled (off_t is 64-bit),
			// only allow mmapping if file length can fit into size_t.
			if (sizeof(size_t) < sizeof(off_t) && stdinStatus.st_size > (size_t)-1) { goto use_normal_read_write_transfer; }

			unsigned char* stdinFileData = (unsigned char*)mmap(nullptr, 
							  stdinStatus.st_size, 
							  PROT_READ, 
							  MAP_PRIVATE | MAP_NORESERVE | MAP_POPULATE, 
							  STDIN_FILENO, 
							  0);
			if (stdinFileData == MAP_FAILED) { goto use_normal_read_write_transfer; }

			// NOTE: I'm pretty sure these don't overwrite each other, but just in case, I put the more important one second.
			posix_madvise(stdinFileData, stdinStatus.st_size, POSIX_MADV_WILLNEED);
			posix_madvise(stdinFileData, stdinStatus.st_size, POSIX_MADV_SEQUENTIAL);

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

					while (true) {
do_it_all_over_again:
						//write(STDERR_FILENO, "took\n", 5);

						// TODO: Make this use big pages for efficiency.
						char* buffer = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_PRIVATE | MAP_ANON,
										     -1,
										     0);
						if (buffer == MAP_FAILED) { REPORT_ERROR_AND_EXIT("testing123", EXIT_FAILURE); }

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
								if (vmsplice(STDOUT_FILENO, &memdata, 1, SPLICE_F_MORE | SPLICE_F_GIFT) == -1) {
									// TODO: Is that the actual err det method?
									REPORT_ERROR_AND_EXIT("error with vmsplice", EXIT_FAILURE);
								}

								munmap(buffer, stdoutPipeBufferSize);		// TODO: Is this allowed?

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
				
					while (true) {
do_it_all_over_again2:

						// TODO: Make this use big pages for efficiency.
						char* buffer = (char*)mmap(nullptr,
										     stdoutPipeBufferSize,
										     PROT_WRITE,
										     MAP_ANON,
										     0,
										     0);

						std::memcpy(buffer, tempBuffer + tempBufferEaten, tempBufferPtr - tempBufferEaten);

						size_t amountOfBufferFilled = tempBufferPtr - tempBufferEaten;
						amountOfBufferFilled += sprintf(buffer + amountOfBufferFilled, "%u", stdinFileData[0]) - 1;



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
								vmsplice(STDOUT_FILENO, &memdata, 1, SPLICE_F_MORE | SPLICE_F_GIFT);

								munmap(buffer, stdoutPipeBufferSize);		// TODO: Is this allowed?

								goto do_it_all_over_again2;
							}
						}

						for (; amountActuallyRead < 8; amountActuallyRead++) {

							memdata.iov_base = buffer;
							memdata.iov_len = amountOfBufferFilled;
							vmsplice(STDOUT_FILENO, &memdata, 1, 0);

							munmap(buffer, stdoutPipeBufferSize);



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
	}

use_normal_read_write_transfer:

	// NOTE: The stat stuff isn't necessary because posix_fadvise just fails when stdin is
	// a pipe, which is totally fine.
	/*{
		struct stat stdinStatus;
		if (fstat(STDIN_FILENO, &stdinStatus) == 0) {
			if (!S_ISFIFO(stdinStatus.st_mode)) {*/
				if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_SEQUENTIAL) == 0) {
					if (posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_NOREUSE) == 0) {
						posix_fadvise(STDIN_FILENO, 0, 0, POSIX_FADV_DONTNEED);
					}
				}
			/*}
		}
	}*/
#endif

	unsigned char buffer[8];

	// NOTE: fread shouldn't ever return less than the wanted amount of bytes unless either:
	// a) EOF
	// b) an error occurred
	// In this way, it is very different to the raw I/O (read and write).
	if (std::fread(buffer, sizeof(char), 1, stdin) == 0) {
		if (std::ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }
		REPORT_ERROR_AND_EXIT("no data received, language requires data", EXIT_SUCCESS);
	}
	if (std::printf("%u", buffer[0]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }

	while (true) {
		size_t bytesRead = std::fread(buffer, sizeof(char), sizeof(buffer), stdin);

		if (bytesRead >= sizeof(buffer)) {
			if (std::printf(", %u, %u, %u, %u, %u, %u, %u, %u", 
					buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]) < 0) {
				REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
			}
			continue;
		}

		if (std::ferror(stdin)) { REPORT_ERROR_AND_EXIT("failed to read from stdin", EXIT_FAILURE); }
		for (char i = 0; i < bytesRead; i++) {
			if (std::printf(", %u", buffer[i]) < 0) { REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE); }
		}
		return;
	}
}

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
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		if (std::printf("const char %s[] = { ", flags::varname) < 0) {
			REPORT_ERROR_AND_EXIT("failed to write to stdout", EXIT_FAILURE);
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
