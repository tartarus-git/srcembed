#include <cstdio>		// for I/O
#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE
#include <cstring>		// for std::strcmp()

// NOTE: Linux says to use BUFSIZ here, because that's supposed to be optimized.
// NOTE: Instead of that, we're using the default buffer size for pipes on Linux, since the I/O
// is going to be piped most of the time. I've had way better luck with this number than with BUFSIZ, at least for this application.
#define BUFFER_SIZE 65536

const char helpText[] = "usage: srcembed [--help] || ([--varname <variable name>] <language>)\n" \
			"\n" \
			"function: Converts input byte stream into source file (output through stdout).\n" \
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
void writeError(const char (&message)[message_length]) noexcept {
	if (std::fwrite(message, sizeof(char), message_length, stderr) == 0) { std::exit(EXIT_FAILURE); }
}

template <size_t message_length>
void writeOutput(const char (&message)[message_length]) noexcept {
	if (std::fwrite(message, sizeof(char), message_length, stdout) == 0) {
		writeError("ERROR: failed to write to stdout\n");
		std::exit(EXIT_FAILURE);
	}
}

void output_C_CPP_array_data() noexcept {
	unsigned char buffer[8];

	if (std::fread(buffer, sizeof(char), 1, stdin) == 0) {
		if (std::ferror(stdin)) { writeError("ERROR: failed to read from stdin\n"); std::exit(EXIT_FAILURE); }
		writeError("ERROR: no data received, language requires data\n"); std::exit(EXIT_SUCCESS);
	}
	std::printf("%u", buffer[0]);

	while (true) {
		size_t bytesRead = std::fread(buffer, sizeof(char), sizeof(buffer), stdin);
		if (bytesRead == 0) {
			if (std::ferror(stdin)) { writeError("ERROR: failed to read from stdin\n"); std::exit(EXIT_FAILURE); }
			break;
		}
		if (bytesRead >= sizeof(buffer)) {
			if (std::printf(", %u, %u, %u, %u, %u, %u, %u, %u", 
					buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]) < 0) {
				writeError("ERROR: failed to write to stdout\n");
				std::exit(EXIT_FAILURE);
			}
			continue;
		}
		for (char i = 0; i < bytesRead; i++) {
			if (std::printf(", %u", buffer[i]) < 0) {
				writeError("ERROR: failed to write to stdout\n");
				std::exit(EXIT_FAILURE);
			}
		}
		return;
	}
}

void outputSource(const char* varname, const char* language) noexcept {
	if (std::strcmp(language, "c++") == 0) {
		std::printf("const char %s[] { ", varname);
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		std::printf("const char %s[] = { ", varname);
		output_C_CPP_array_data();
		writeOutput(" };\n");
		return;
	}

	writeOutput("ERROR: invalid language\n"); std::exit(EXIT_SUCCESS);
}

int main(int argc, const char* const * argv) noexcept {
	/*
	NOTE: Linux has this cool readahead functionality that reads the disk ahead and minimizes cache-misses when you read sequentially.
	I can't really use the function here though because it only works on normal (and maybe a couple other) file-types.
	I could still put it in just in case stdin happens to be a supported file-type, but you have to specify a maximum amount
	of bytes that you want to read, which doesn't work here since we have no idea how many bytes we're going to be reading.
	You also can't just put in the maximum number for size_t because then it fails for some reason.
	NOTE: I guess we could test if stdin is a file, then find out how big the file is and use that for the amount of bytes that we want
	to read, but that's a lot of work for something that won't happen that often (remember bash pipes the file into stdin, it doesn't
	set stdin to the file, for whatever reason).
	*/

	// C++ standard I/O can suck it, it's super slow. We're using C standard I/O. Maybe I'll make a wrapper library for C++ eventually.

	// NOTE: This should do exactly what we want, but it totally doesn't for some reason.
	//setvbuf(stdout, nullptr, _IOFBF, 65536);
	//setvbuf(stdin, nullptr, _IOFBF, 65536);

	// NOTE: So instead, we're doing it like this.
	char stdout_buffer[BUFFER_SIZE];
	setbuffer(stdout, stdout_buffer, BUFFER_SIZE);
	char stdin_buffer[BUFFER_SIZE];
	setbuffer(stdin, stdin_buffer, BUFFER_SIZE);

	if (argc == 1) { writeError("ERROR: not enough args\n"); return EXIT_SUCCESS; }

	if (std::strcmp(argv[1], "--help") == 0) {
		if (argc != 2) { writeError("ERROR: too many args\n"); return EXIT_SUCCESS; }
		writeOutput(helpText);
		return EXIT_SUCCESS;
	}

	if (std::strcmp(argv[1], "--varname") == 0) {
		if (argc < 4) { writeError("ERROR: not enough args\n"); return EXIT_SUCCESS; }
		if (argc > 4) { writeError("ERROR: too many args\n"); return EXIT_SUCCESS; }
		outputSource(argv[2], argv[3]);
		return EXIT_SUCCESS;
	}

	if (argc != 2) { writeError("ERROR: too many args\n"); return EXIT_SUCCESS; }
	outputSource("data", argv[1]);
}
