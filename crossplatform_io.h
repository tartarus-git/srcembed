#pragma once

#ifndef PLATFORM_WINDOWS

#include <unistd.h>

using sioret_t = ssize_t;

#define crossplatform_read(fd, buf, count) ::read(fd, buf, count)
#define crossplatform_write(fd, buf, count) ::write(fd, buf, count)

#else

#include <type_traits>

#include <io.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

using sioret_t = int;

using ssize_t = typename std::make_signed<size_t>::type;

#define crossplatform_read(fd, buf, count) ::read(fd, buf, count)
#define crossplatform_write(fd, buf, count) ::write(fd, buf, count)

#endif
