#pragma once

#include <unistd.h>

#include <algorithm>	// TODO: for std::copy right?

#include <thread>

namespace asyncio {

	enum class buffer_position_t : bool {
		left = true,
		right = false
	};

	template <size_t buffer_size>
	class stdin_stream {
		static volatile char buffer[buffer_size * 2];
		static constexpr volatile char* buffer_half_end_ptr = buffer + buffer_size;
		static volatile char* buffer_user_read_head = 0;

		static volatile std::thread reader_thread;

		static volatile buffer_position_t empty_buffer = buffer_position_t::right;
		static volatile bool buffer_read_pending = false;

		static volatile finalize_reader_thread = false;

		int8_t read_full_buffer(void* buf, size_t count) noexcept {
			while (true) {
				if (finalize_reader_thread) { return 0; }

				ssize_t bytes_read = read(STDIN_FILENO, buf, count);
				bool nothing_to_read = (errno == EAGAIN || errno == EWOULDBLOCK);
				if (bytes_read == -1 && !nothing_to_read) { return -1; }
				bytes_read += nothing_to_read;
				// TODO: Test if the above line is faster than an if statement, look at assembly, stuff like that.
				// It turns -1 to 0 when EAGAIN or EWOULDBLOCK is there. Consider that stdin isn't always a regular file.

				count -= bytes_read;
				if (count == 0) { return 1; }
				buf += bytes_read;
			}
		}

		static void reader_thread_code() noexcept {
			while (true) {
				while (empty_buffer == buffer_position_t::right) { }

				int8_t read_result = read_full_buffer(buffer, buffer_size);
				switch (read_result) {
				case -1:
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case 0: return;
				}

				buffer_read_pending = false;

				while (empty_buffer == buffer_position_t::left) { }

				read_result = read_full_buffer(buffer + buffer_size, buffer_size);
				switch (read_result) {
				case -1:
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case 0: return;
				}

				buffer_read_pending = false;
			}
		}

	public:
		static bool initialize() noexcept {
			int stdin_fd_flags = fcntl(STDIN_FILENO, F_GETFL);
			if (stdin_fd_flags == -1) { return false; }
			if (fcntl(STDIN_FILENO, F_SETFL, stdin_fd_flags | O_NONBLOCK) == -1) { return false; }

			reader_thread = std::thread(reader_thread_code);

			return true;
		}

		static bool read(char* output_ptr, size_t output_size) noexcept {
			const volatile char* read_end_ptr = buffer_user_read_head + output_size;
			while (true) {
				// TODO: What does double volatile mean with pointers?
				const volatile char* const current_buffer_end_ptr = empty_buffer * buffer_half_end_ptr + buffer_half_end_ptr;
				if (read_end_ptr < current_buffer_end_ptr) {
					std::copy(buffer_user_read_head, read_end_ptr, output_ptr);
					buffer_user_read_head = read_end_ptr;
					return true;
				}
	
				std::copy(buffer_user_read_head, current_buffer_end_ptr, output_ptr);
				const size_t full_space = current_buffer_end_ptr - buffer_user_read_head;
				output_ptr += full_space;
				read_end_ptr -= full_space;
	
				while (buffer_read_pending) { }
				if (finalize_reader_thread) { return false; }
				buffer_read_pending = true;
				empty_buffer = !empty_buffer;

				// NOTE: We do this here for two reasons:
				// 1. So that an error doesn't cause buffer bytes to be eaten (it would do that if this were above the if-stm)
				// 2. So we can use the toggled version of empty_buffer.
				buffer_user_read_head = empty_buffer * current_buffer_end_ptr;
			}
		}

		static void dispose() noexcept {
			finalize_reader_thread = true;
			empty_buffer = !empty_buffer;
			reader_thread.join();
		}
	};

	template <size_t buffer_size>
	class StdoutStream {
		// TODO: put these outside of the class, basically figure out the details of the syntax for this.
		static volatile char buffer[buffer_size * 2];
		static volatile size_t buffer_user_write_head = 0;

		static volatile std::thread flusher_thread;

		static volatile buffer_position_t full_buffer = buffer_position_t::right;
		static volatile bool buffer_flush_pending = false;

		static volatile size_t flush_size = buffer_size;

		static volatile bool finalize_flusher_thread = false;

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

				buffer_flush_pending = false;
			}
		}

		static void initialize() noexcept {
			flusher_thread = std::thread(flusher_thread_code);
		}

		// TODO: Go through this function and make it optimal.
		static bool write(const char* input_ptr, size_t input_size) noexcept {
			while (true) {
				// NOTE: Converting the full_buffer bool to another integer type is totally fine, since converted bools always equal 0 or 1, all other non-zero values get transformed to 1 on conversion.
				// NOTE: Unless of course bools cannot contain other non-zero values because converting to bool might snap to 0 or 1 already. That's an implementation detail though. Whether that detail is implemented by the standard or left up to the compiler I cannot say without further research.
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
			// TODO: Make sure this function makes sense and then see if you should put some comments in.
			flush_size = buffer_user_write_head - full_buffer * buffer_size;

			while (buffer_flush_pending) { }

			if (finalize_flusher_thread) { return false; }

			buffer_flush_pending = true;
			full_buffer = !full_buffer;

			while (buffer_flush_pending) { }

			flush_size = buffer_size;

			return true;
		}

		static void dispose() noexcept {
			flush();
			finalize_flusher_thread = true;
			full_buffer = !full_buffer;
			flusher_thread.join();
		}
	};

}
