#pragma once

#include "crossplatform_io.h"

#ifdef PLATFORM_WINDOWS
#error "meminfo_parser.h" header file cannot be included when compiling for Windows
#endif

#include <fcntl.h>

#include <cstdint>

// NOTE: I'm decently happy with this file's quality considering the short development time.
// If I had a lot more time I would make a really cool regex engine that leverages compile-time
// for the finite state automata generation and such. Then I could replace a lot of this stuff
// with simple state table lookup code.

inline bool is_char_invisible(char input) noexcept {
	switch (input) {
		case '\0':
			return true;
		default:
			return false;
	}
}

inline bool is_char_preamble(char input) noexcept {
	switch (input) {
		case '\n':
		case ' ':
		case ';':
			return true;
		default:
			return false;
	}
}

inline bool is_char_skippable(char input) noexcept {
	switch (input) {
	case ' ':
	case ':':
	case '\t':
		return true;
	default:
		return false;
	}
}

constexpr char huge_page_size_key[] = "Hugepagesize";

// Pretty efficient parser that finds the huge_page_size_key string and then parses out the value that follows it.
// It's pretty resilient to changes in the meminfo file format, in case those come in the future I guess (they won't I suppose).
inline ssize_t parse_huge_page_size_from_meminfo_file() noexcept {
	int fd = open("/proc/meminfo", 0, 0);
	if (fd == -1) { return -1; }

	char buffer[1024];

	ssize_t bytes_read;
	size_t i;
	uint8_t j = 0;
	while (true) {
		bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
		if (bytes_read <= 0) { return -1; }

		// NOTE: If you were looking for the string "bbba" and the junction looked like this "bb|bba",
		// parsing would fail since it can't move i back to before the junction after crossing the junction.
		// I'm struggling to formalize this phenomenon, but intuitively, I can practically assure you
		// that this effect doesn't happen for the string "Hugepagesize", so we're good.
		// TODO: Formalize this and get away from specific examples. Or is the problem undecidable or something?
		// TODO: I've changed the code quite a bit, so look in previous commits to fully make sense of this comment.

		// NOTE: Another interesting thing about the string "Hugepagesize" is that it doesn't contain the start of itself
		// in any other place other than the start of itself.
		// Based on this, it seems to be a valid optimization to continue looking for "Hugepagesize" only after
		// the end of the incomplete previous match of the string, since all of the matched characters cannot
		// represent a start. This is what we've done.

		for (i = 0; i < bytes_read; i++) {
			if (is_char_invisible(buffer[i])) { continue; }
			if (buffer[i] != huge_page_size_key[j]) {
			handle_preamble:
				while (!is_char_preamble(buffer[i])) {
					i++;
					if (i == bytes_read) {
						bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
						if (bytes_read <= 0) { return -1; }
						i = 0;
					}
				}

				j = 0;
				continue;
			}
			j++;
			if (j == sizeof(huge_page_size_key) - 1) {
				// After finding a full match we need to make sure that it isn't followed by something else, making it a different key.
				do {
					i++;
					if (i == bytes_read) {
						bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
						if (bytes_read <= 0) { return -1; }
						i = 0;
					}
				} while (is_char_invisible(buffer[i]));
				if (!is_char_skippable(buffer[i])) { goto handle_preamble; }
				goto noop_loop;
			}
		}
	}

noop_loop:
	while (true) {
		for (; i < bytes_read; i++) {
			if (buffer[i] < '0' || buffer[i] > '9') { continue; }
			goto value_parse_loop;
		}

		bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
		if (bytes_read <= 0) { return -1; }
		i = 0;
	}

value_parse_loop:
	size_t result = 0;
	// NOTE: AVOIDING SIGNED OVERFLOW, BECAUSE IT'S UNDEFINED!
	unsigned char digit = (unsigned char)buffer[i] - '0';
	while (true) {
		result = result * 10 + digit;
		i++;
		if (i == bytes_read) {
			bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
			if (bytes_read == 0) { return result; }
			if (bytes_read == -1) { return -1; }
			i = 0;
		}
		digit = (unsigned char)buffer[i] - '0';
		if (digit > '9') { return result * 1024; }
	}
}
