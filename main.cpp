#include <iostream>		// for I/O
#include <cstdlib>		// for std::exit(), EXIT_SUCCESS and EXIT_FAILURE
#include <cstring>		// for std::strcmp()
#include <new>			// for non-throwing new
#include <cstdio>		// for EOF

// TODO: Research why buffer sizes are the way they are and why bigger isn't always better?

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

void outputSource(const char* varname, const char* language) noexcept {
	if (std::cout.bad()) { std::cerr << "ERROR: output to stdout failed\n"; std::exit(EXIT_FAILURE); }

	if (std::strcmp(language, "c++") == 0) {
		std::cout << "#pragma once; const char " << varname << "[] { ";
		while (true) {
			int byte = std::cin.get();
			if (byte == EOF) { break; }
			std::cout << byte << ", ";
		}
		std::cout << "0 };\n";
		return;
	}
	if (std::strcmp(language, "c") == 0) {
		std::cout << "const char " << varname << "[] = { ";
		while (true) {
			int byte = std::cin.get();
			if (byte == EOF) { break; }
			std::cout << byte << ", ";
		}
		std::cout << "0 };\n";
		return;
	}

	std::cerr << "ERROR: invalid language\n"; std::exit(EXIT_SUCCESS);
}

int main(int argc, const char* const * argv) noexcept {
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
