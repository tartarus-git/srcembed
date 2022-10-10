#pragma once

#include <unistd.h>

#include <algorithm>	// TODO: for std::copy right?

#include <thread>

template <size_t buffer_size>
class StdinStream {
	static volatile char buffer[buffer_size * 2];
	static volatile size_t buffer_user_read_head = 0;
	static volatile size_t buffer_stream_write_head = 0;

	static volatile std::thread reader_thread;

	static volatile reader_thread_should_be_active = true;

	static void reader_thread_code() noexcept {
		while (reader_thread_should_be_active) {
			// TODO: Do some sort of polling thing so that it doesn't block unnecessarily.
			if (buffer_user_read_head > buffer_stream_write_head + 1) {	// NOTE: This branch ain't so bad because it's outcome shouldn't really ever change once the program is running, either the user reads fast enough or too slow, but he reads at constant speed.
				ssize_t bytes_read = read(STDIN_FILENO, buffer + buffer_stream_write_head, buffer_user_read_head - buffer_stream_write_head - 1);
				// TODO: Check for read error and report through flag to other thread.
				buffer_stream_write_head += bytes_read;
				continue;
			}
			/*
			if (buffer_user_read_head == 0) {
				ssize_t bytes_read = read(STDIN_FILENO, buffer + buffer_stream_write_head, buffer_size - buffer_stream_write_head - 1);
				buffer_stream_write_head += bytes_read;
				continue;
			}
			ssize_t bytes_read = read(STDIN_FILENO, buffer + buffer_stream_write_head, buffer_size - buffer_stream_write_head);
			buffer_stream_write_head = (buffer_stream_write_head + bytes_read) % buffer_size;	// NOTE: div/modulo with fixed divisor is a lot faster than with variable divisor.
			*/

// NOTE: Version 1 (above) could be faster, but I really don't think so. Version 2 (below) is probably faster because it has one less branch in it and that branch periodically faults the branch predictor, which is bad.

														// TODO: Unless of course this bool cast trick implies an if statement, inspect assembly to find out.
				ssize_t bytes_read = read(STDIN_FILENO, buffer + buffer_stream_write_head, buffer_size - buffer_stream_write_head - !(bool)buffer_user_read_head);
				buffer_stream_write_head = (buffer_stream_write_head + bytes_read) % buffer_size;
				continue;

			// TODO: Should the above be replaced with an if statement, I feel like the branch predictor might miss it a little too much or something. We might be better off with the fast modulo.
			// TODO: If new data is available but the user hasn't popped off the old data yet, this loop will spam the read syscall super fast. Test if that's alright.
			// I'm okay with massive CPU core draw while it's in this busy loop, because that's not what this is optimized for, but is it good to be calling syscalls that fast. I don't see why not.
		}
	}

	static void initialize() noexcept {
		reader_thread = std::thread(reader_thread_code);
	}

	static void read(char* output_ptr, size_t output_size) noexcept {
		std::copy(buffer + buffer_user_read_head, buffer + buffer_stream_write_head, output_ptr);
		buffer_user_read_thread = buffer_stream_write_head;
	}

	static void dispose() noexcept {
		reader_thread_should_be_active = false;
		reader_thread.join();
	}
};

template <size_t buffer_size>
class StdoutStream {
	// TODO: Maybe not all of these have to be volatile, idk yet, check it out.
	static volatile char buffer[buffer_size * 2];
	static volatile size_t buffer_user_write_head = 0;

	static volatile std::thread flusher_thread;

	static volatile enum class buffer_position_t : bool { left = true, right = false } full_buffer = buffer_position_t::right;
	static volatile bool buffer_flush_pending = false;

	static volatile size_t flush_size = buffer_size;

	static volatile bool finalize_flusher_thread = false;

	// TODO: Understand forwarding references. What if we want to only accept rvalue refs in template function, what the hell would we do then?

	static void flusher_thread_code() noexcept {
		while (true) {
			while (full_buffer == buffer_position_t::right) { }
			if (finalize_flusher_thread) { return; }
			if (write(STDOUT_FILENO, buffer, flush_size) == -1) {
				finalize_flusher_thread = true;
				buffer_flush_pending = false;
				return;
			}
			buffer_flush_pending = false;
			while (full_buffer == buffer_position_t::left) { }
			if (finalize_flusher_thread) { return; }
			if (write(STDOUT_FILENO, buffer + buffer_size, flush_size) == -1) {
				finalize_flusher_thread = true;
				buffer_flush_pending = false;
				return;
			}
			buffer_flush_pending = true;
		}
	}

	static void initialize() noexcept {
		flusher_thread = std::thread(flusher_thread_code);
	}

	static bool write(const char* input_ptr, size_t input_size) noexcept {
		while (true) {
			// NOTE: Converting the full_buffer bool to another integer type is totally fine, since converted bools always equal 0 or 1, all other non-zero values get transformed to 1 on conversion.
			// NOTE: Unless of course bools cannot contain other non-zero values because converting to bool might snap to 0 or 1 already. That's an implementation detail though.
			size_t free_space = buffer_size - (buffer_user_write_head - full_buffer * buffer_size);
			if (input_size < free_space) {
				buffer_user_write_head = std::copy(input_ptr, input_ptr + input_size, buffer + buffer_user_write_head);
				return true;
			}

			buffer_user_write_head = std::copy(input_ptr, input_ptr + free_space, buffer + buffer_user_write_head);
			input_ptr += free_space;
			input_size -= free_space;			// TODO: It's annoying to have to do this, any fixes?

			while (buffer_flush_pending) { }
			if (finalize_flusher_thread) { return false; }
			buffer_flush_pending = true;
			full_buffer = !full_buffer;
			if (buffer_user_write_head == buffer_size * 2) { buffer_user_write_head = 0; }
		}
	}

	static bool flush() noexcept {
		flush_size = buffer_user_write_head - full_buffer * buffer_size;
		while (buffer_flush_pending) { }
		if (finalize_flusher_thread) { return false; }
		buffer_flush_pending = true;
		full_buffer = !full_buffer;
		return true;
	}

	static void dispose() noexcept {
		flush();
		finalize_flusher_thread = true;
		full_buffer = !full_buffer;
		flusher_thread.join();
	}
};
