#ifndef PLATFORM_WINDOWS
#include <fcntl.h>		// for readahead()
#include <unistd.h>		// for STDIN_FILENO
#endif

#include <iostream>		// for I/O
#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE
#include <cstring>		// for std::strcmp()
#include <new>			// for non-throwing new
#include <cstdio>		// for EOF

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

void output_C_CPP_array_data() noexcept {
	int byte = std::cin.get();
	if (byte == EOF) {
		std::cerr << "ERROR: no data received, language requires data\n"; std::exit(EXIT_SUCCESS);
	}
	std::cout << byte;
	while (true) {
		byte = std::cin.get();
		if (byte == EOF) { break; }
		std::cout << ", " << byte;
	}
}

void outputSource(const char* varname, const char* language) noexcept {
	if (std::cout.bad()) { std::cerr << "ERROR: output to stdout failed\n"; std::exit(EXIT_FAILURE); }

	if (std::strcmp(language, "c++") == 0) {
		std::cout << "const char " << varname << "[] { ";
		output_C_CPP_array_data();
		std::cout << " };\n";
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		std::cout << "const char " << varname << "[] = { ";
		output_C_CPP_array_data();
		std::cout << " };\n";
		return;
	}

	std::cerr << "ERROR: invalid language\n"; std::exit(EXIT_SUCCESS);
}

int main(int argc, const char* const * argv) noexcept {
#ifndef PLATFORM_WINDOWS
	readahead(STDIN_FILENO, 0, (size_t)-1);
	/*
	NOTE: The above usually doesn't work since stdin is usually the tty or a pipe. If it were a normal file though,
	then the above call would actually be super important. It tells the OS to read ahead in the file and
	preemptively cache the disk sectors. This makes it so we get almost no cache-misses if we read the file
	sequentially, which we do. When you pipe a file into a command in bash though, it goes through a pipe first,
	so this is rarely applicable. We have it just in case though.
	*/
#endif

	std::cout.sync_with_stdio(false);
	std::cerr.sync_with_stdio(false);
	std::cin.sync_with_stdio(false);

	if (argc == 1) { std::cerr << "ERROR: not enough args\n"; return EXIT_SUCCESS; }

	if (std::strcmp(argv[1], "--help") == 0) {
		if (argc != 2) { std::cerr << "ERROR: too many args\n"; return EXIT_SUCCESS; }
		std::cout << helpText;
		return EXIT_SUCCESS;
	}

	if (std::strcmp(argv[1], "--varname") == 0) {
		if (argc < 4) { std::cerr << "ERROR: not enough args\n"; return EXIT_SUCCESS; }
		if (argc > 4) { std::cerr << "ERROR: too many args\n"; return EXIT_SUCCESS; }
		outputSource(argv[2], argv[3]);
		return EXIT_SUCCESS;
	}

	if (argc != 2) { std::cerr << "ERROR: too many args\n"; return EXIT_SUCCESS; }
	outputSource("data", argv[1]);
}
