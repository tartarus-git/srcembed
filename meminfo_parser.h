#pragma once

#include "crossplatform_io.h"

#ifdef PLATFORM_WINDOWS
#error "meminfo_parser.h" header file cannot be included when compiling for Windows
#endif

#include <fcntl.h>

#include <cstdint>

inline bool is_char_skippable(char input) noexcept {
	switch (input) {
	case ' ':
	case ':':
	case '\0':
	case '\t':
	// TODO: Maybe there are a couple more cases to consider, look at the ASCII table and find out.
		return true;
	default:
		return false;
	}
}

// TODO: You could have used this method for meta_printf no?
constexpr char huge_page_size_key[] = "Hugepagesize";

inline ssize_t parse_huge_page_size_from_meminfo_file() noexcept {
	int fd = open("/proc/meminfo", 0, 0);
	if (fd == -1) { return -1; }

	char buffer[1024];

	ssize_t bytes_read;
	size_t i;
	uint16_t j = 0;
	uint16_t j_sub = 0;
	while (true) {
	continue_main_loop:
		bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
		if (bytes_read <= 0) { return -1; }
		
		// NOTE: If you were looking for the string "bbba" and the junction looked like this "bb|bba",
		// parsing would fail since it can't move i back to before the junction after crossing the junction.
		// I'm struggling to formalize this phenomenon, but intuitively, I can practically assure you
		// that this effect doesn't happen for the string "Hugepagesize", so we're good.
		// TODO: Formalize this and get away from specific examples. Or is the problem undecidable or something?
		for (i = 0; i < bytes_read; i++) {
			size_t index;
			for (; j < sizeof(huge_page_size_key) - 1; j++) {
				index = i + (j - j_sub);
				if (index >= bytes_read) {
					j_sub = j;
					goto continue_main_loop;
				}
				if (buffer[index] != huge_page_size_key[j]) {
					j = 0;
					j_sub = 0;
					goto continue_outside_for_loop;
				}
			}
			if (is_char_skippable(buffer[index + 1])) {
				i = index + 2;
				goto noop_loop;
			}
			j = 0;			// NOTE: This is duplicated on purpose, since it should be more efficient.
			j_sub = 0;
		continue_outside_for_loop: continue;	// NOTE: I'm really relying on the optimizer for this one.
		}
	}

noop_loop:
	while (true) {
		for (; ; i++) {
			if (i >= bytes_read) { goto refill_buffer; }
			if (buffer[i] < '0' || buffer[i] > '9') { continue; }
			goto value_parse_loop;
		}

	refill_buffer:
		bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
		if (bytes_read <= 0) { return -1; }
		i = 0;
	}

value_parse_loop:
	size_t result = 0;
	while (true) {
		for (; ; i++) {
			if (i >= bytes_read) { goto refill_buffer2; }
			if (buffer[i] < '0' || buffer[i] > '9') { return result * 1024; }
			result = result * 10 + (buffer[i] - '0');
		}

	refill_buffer2:
		bytes_read = read_entire_buffer(fd, buffer, sizeof(buffer)); 
		if (bytes_read <= 0) { return -1; }
		i = 0;
	}
}
