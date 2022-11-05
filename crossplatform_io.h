#pragma once

#ifndef PLATFORM_WINDOWS

#include <unistd.h>

using sioret_t = ssize_t;

#define crossplatform_read(fd, buf, count) ::read(fd, buf, count)		// TODO: You could turn these into functions, although macros work as well here.
#define crossplatform_write(fd, buf, count) ::write(fd, buf, count)

#else

#include <type_traits>

#include <io.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

using sioret_t = int;

using ssize_t = typename std::make_signed<size_t>::type;

#define crossplatform_read(fd, buf, count) ::_read(fd, buf, count)
#define crossplatform_write(fd, buf, count) ::_write(fd, buf, count)

#endif

inline ssize_t read_entire_buffer(int fd, void* buffer, size_t size) noexcept {
	const size_t original_size = size;
	while (true) {
		sioret_t bytes_read = crossplatform_read(fd, buffer, size);
		if (bytes_read == 0) { return original_size - size; }
		if (bytes_read == -1) { return -1; }
		size -= bytes_read;
		if (size == 0) { return original_size; }
		*(char**)&buffer += bytes_read;
	}
}
