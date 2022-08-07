#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE, as well as every other syscall
#ifndef PLATFORM_WINDOWS
#include <unistd.h>		// for raw I/O
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

	// NOTE: This should do exactly what we want, but it totally doesn't for some reason.
	// NOTE: In fact, it kind of makes the performance worse.
	// TODO: Why?
	//setvbuf(stdout, nullptr, _IOFBF, BUFSIZ);
	//setvbuf(stdin, nullptr, _IOFBF, BUFSIZ);

	// NOTE: So instead, we're doing it like this.
	char stdout_buffer[BUFSIZ];
	setbuffer(stdout, stdout_buffer, BUFSIZ);
	char stdin_buffer[BUFSIZ];
	setbuffer(stdin, stdin_buffer, BUFSIZ);

	// NOTE: One would think that BUFSIZ is the default buffer size for C buffered I/O.
	// Apparently it isn't, because the above yields performance improvements.

	int normalArgIndex = manageArgs(argc, argv);
	outputSource(argv[normalArgIndex]);
}
