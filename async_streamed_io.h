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
		static inline volatile char buffer[buffer_size * 2];

		static constexpr volatile char* buffer_half_end_ptr = buffer + buffer_size;

		static inline volatile char* buffer_user_read_head = 0;

		static inline volatile std::thread reader_thread;

		static inline volatile buffer_position_t empty_buffer = buffer_position_t::right;
		static inline volatile bool buffer_read_pending = false;

		static inline volatile finalize_reader_thread = false;

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

				// NOTE: We do this here
				// so that an error doesn't cause buffer bytes to be eaten (it would do that if this were above the if-stm)
				buffer_user_read_head = current_buffer_end_ptr - empty_buffer * (buffer_size * 2);
				// TODO: See if this would be faster with an if-statement, considering that we call read with small chunks
				// instead of big ones.
				// TODO: Put it down like you did the one below, looks better and is more understandable.

				buffer_read_pending = true;
				empty_buffer = !empty_buffer;
			}
		}

		static void dispose() noexcept {
			finalize_reader_thread = true;
			empty_buffer = !empty_buffer;
			reader_thread.join();
		}
	};

	template <size_t buffer_size>
	class stdout_stream {
		static inline volatile char buffer[buffer_size * 2];

		static constexpr volatile char* buffer_half_end_ptr = buffer + buffer_size;

		static inline volatile size_t buffer_user_write_head = 0;

		static inline volatile std::thread flusher_thread;

		static inline volatile buffer_position_t full_buffer = buffer_position_t::right;
		static inline volatile bool buffer_flush_pending = false;

		static inline volatile size_t flush_size = buffer_size;

		static inline volatile bool finalize_flusher_thread = false;

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

		static bool write(const char* input_ptr, size_t input_size) noexcept {
			while (true) {
				// NOTE: Converting the full_buffer bool to another integer type is totally fine, since converted bools
				// always equal 0 or 1, all other non-zero values get transformed to 1 on conversion.
				// NOTE: Unless of course bools cannot contain other non-zero values because converting to bool might snap to
				// 0 or 1 already. That's an implementation detail though. Whether that detail is implemented by the
				// standard or left up to the compiler I cannot say without further research.
				const volatile char* const free_space = full_buffer * buffer_half_end_ptr + buffer_half_end_ptr - buffer_user_write_head;
				// TODO: This is great for pretty large writes and reads, but for small chunks, would it be better
				// to have 2 while loops and alternate between them with if-stm every time one is full?
				// Within those loops, you wouldn't have to algebraically check which buffer is full since it's a given.
				// You should test this theory with some benchmarking.
				if (input_size < free_space) {
					buffer_user_write_head = std::copy(input_ptr, input_ptr + input_size, buffer_user_write_head);
					return true;
				}

				const char* new_input_ptr = input_ptr + free_space;
				buffer_user_write_head = std::copy(input_ptr, new_input_ptr, buffer_user_write_head);
				input_ptr = new_input_ptr;
				input_size -= free_space;

				while (buffer_flush_pending) { }
				if (finalize_flusher_thread) { return false; }
				buffer_flush_pending = true;
				full_buffer = !full_buffer;

				// TODO: Same TODO as above.
				buffer_user_write_head = buffer + full_buffer * buffer_size;
			}
		}

		static bool flush() noexcept {
			// Set flush_size to the exact amount that still needs to be flushed.
			flush_size = buffer_user_write_head - (buffer + full_buffer * buffer_size);

			// Wait for other buffer to finish flushing.
			while (buffer_flush_pending) { }

			// If error occurred, report it.
			if (finalize_flusher_thread) { return false; }

			// Start flush.
			buffer_flush_pending = true;
			full_buffer = !full_buffer;

			// Wait for it to finish.
			while (buffer_flush_pending) { }

			// Reset flush_size to default.
			flush_size = buffer_size;

			// Though both buffers are empty (theoretically we could set this to start), it needs to be at start of correct buffer
			// for the rest of the system to work.
			buffer_user_write_head = buffer + full_buffer * buffer_size;

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
