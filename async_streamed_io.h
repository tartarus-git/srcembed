#pragma once

#include <algorithm>

#include <thread>

#include "crossplatform_io.h"

namespace asyncio {

	enum class buffer_position_t : bool {
		left = true,
		right = false
	};

	buffer_position_t operator!(buffer_position_t buffer_position) noexcept { return (buffer_position_t)!(bool)buffer_position; }

	/*
	   Everyone is saying I shouldn't use volatile for multi-threaded logistics and such, but I think my implementation
	   should be fine.
		IMPORTANT:
			- all volatile does is make sure that the memory accesses aren't optimized out and are done in the exact
				order that you type them in (relative to all other volatile accesses, since non-volatile stuff can still
				move around)
			- that's perfect for me, the one other weird thing is caches.
			- since RAM writes go to cache first and don't propegate to actual RAM for a while, it's not immediately
				obvious that a second thread will see the data that you've written, even if the operation has passed,
				since it could still be stuck in a higher cache somewhere that is core specific.
			- the reason this doesn't happen is because of cache coherency (I think that's what it's called).
				- basically, it's designed so that an access to a memory location that theoretically has been changed
					causes the relevant data to be transported to the right caches.
				- there's multiple ways to accomplish this, but the important thing is that it makes it so that
					after a write, all reads from that location will return the correct data, which is exactly
					what is required for an application such as this.
				- almost all modern hardware is cache coherent apparently, so this program is cross-platform
					in that regard I suppose.
				- BTW: this behaviour makes total sense because the general concensus on caches is that the software
					is not supposed to know about them AFAIK. Obviously the software does know about them
					and optimizing for caches is incredibly effective, but the point is that it's expected that
					you can imagine interfacing directly with RAM, everything is supposed to emulate that, which is
					practical for us because that's the exact behaviour we need.
	*/

	template <size_t buffer_size>
	class stdin_stream {
		// NOTE: Multi-byte volatile variables could technically tear when reading from them.
		// Because the write to the variable happens in multiple stages, so the read could see multiple versions where
		// individual bytes were changed. This shouldn't happen as much on modern machines because they operate on units
		// that are bigger than a byte for load/store and such.
		// Maybe it could still happen on x64 when you write to a 64-bit variable, since it might still write just 32 bits to RAM
		// at a time, idk what the bus looks like on x64. It definitely happens (AFAIK) when you write to a 64-bit variable on x86,
		// since that requires two separate RAM writes.

		static inline volatile char buffer[buffer_size * 2];

		static inline const volatile char* buffer_stream_write_head = nullptr;
		static inline const volatile char* buffer_stream_write_head_copy = nullptr;
		static inline const volatile char* buffer_user_read_head = buffer;

		static inline std::thread reader_thread;

		static inline volatile buffer_position_t empty_buffer = buffer_position_t::right;
		static inline volatile bool buffer_read_pending = false;

		static inline volatile bool finalize_reader_thread = false;

		static sioret_t read_full_buffer(volatile char* buf, size_t count) noexcept {
			volatile char* original_buf_ptr = buf;
			while (true) {
				if (finalize_reader_thread) { return -2; }

				sioret_t bytes_read = crossplatform_read(STDIN_FILENO, (char*)buf, count);
				if (bytes_read == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {		// NOTE: Branch predictor should essentially never fail here, making this super duper fast!
						continue;
					}
					return -3;
				}
				if (bytes_read == 0) { return buf - original_buf_ptr; }

				count -= bytes_read;
				if (count == 0) { return -1; }
				buf += bytes_read;
			}
		}

		static void reader_thread_code() noexcept {
			while (true) {
				while (empty_buffer == buffer_position_t::left) { }

				sioret_t read_result = read_full_buffer(buffer + buffer_size, buffer_size);
				switch (read_result) {
				case -3:
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case -2: return;
				case -1: break;
				default:
					 buffer_stream_write_head = buffer + buffer_size + read_result;
					 buffer_read_pending = false;
					 return;
				}

				buffer_read_pending = false;

				while (empty_buffer == buffer_position_t::right) { }

				read_result = read_full_buffer(buffer, buffer_size);
				switch (read_result) {
				case -3:
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case -2: return;
				case -1: break;
				default:
					 buffer_stream_write_head = buffer + read_result;
					 buffer_read_pending = false;
					 return;
				}

				buffer_read_pending = false;
			}
		}

	public:
		// NOTE: Calling this function more than once is super duper UNDEFINED!
		static bool initialize() noexcept {
			int stdin_fd_flags = fcntl(STDIN_FILENO, F_GETFL);
			if (stdin_fd_flags == -1) { return false; }
			if (fcntl(STDIN_FILENO, F_SETFL, stdin_fd_flags | O_NONBLOCK) == -1) { return false; }

			const sioret_t read_result = read_full_buffer(buffer, buffer_size);
			switch (read_result) {
			case -3: return false;
			// case -2: while (true) { }	<-- shouldn't ever happen
			case -1: break;
			default:
				 buffer_stream_write_head_copy = buffer + read_result;
				 return true;
			}

			// INTERESTING NOTE: std::thread cannot be made volatile, but it doesn't have to be.
			// In C++, memory is "committed" before calling functions, because those functions could theoretically
			// read from the memory, and the correct value needs to be there.
			// Obviously, because of the as-if rule, if the compiler knows that a function doesn't use a specific variable,
			// it's free to keep it in some register (as long as that register stays free long enough) instead of writing it.
			// Thread functions are run via syscall, and the compiler doesn't know what code is in the syscall,
			// so I presume all "hot" variables are written to memory before calling the syscall.
			// This might seem a slight bit inefficient and dirty, but it's the only clean way of handling this.
			// Any other system would induce a lot of complexity and confusion I presume.
			reader_thread = std::thread((void(*)())reader_thread_code);

			return true;
		}

		template <typename T>
		static const T& minimum_value(const T& a, const T& b) noexcept {
			return a < b ? a : b;	// NOTE: This is ok, since: 1. it's so simple that compiler will optimize if there is something to optimize
						// 			    2. there is nothing to optimize since "a" will be smaller every time until the one time where it isn't, where EOF is encountered. --> epic branch prediction, very efficient!
		}

		// NOTE: You can call this function as many times as you like, even input EOF. It'll always just return 0 in that case, but you can totally do it.
		static ssize_t read(char* output_ptr, size_t output_size) noexcept {
			if (buffer_stream_write_head_copy != nullptr) {
				const volatile char* read_end_ptr = minimum_value(buffer_user_read_head + output_size, buffer_stream_write_head_copy);
				std::copy(buffer_user_read_head, read_end_ptr, output_ptr);
				const size_t amount_read = read_end_ptr - buffer_user_read_head;
				buffer_user_read_head = read_end_ptr;
				return amount_read;
			}

			const size_t orig_output_size = output_size;

			while (true) {
				// NOTE: The volatile after the * is the only volatile that is relevant for a theoretical pure
				// dereference, without an attached read or write to the variable. Idk if that exists in
				// C++ because every dereference I've ever seen is attached to a read or write, which then also
				// looks at the volatile before the *.
				// The only volatile necessary here is the one before the *, since the pointer itself is only accessed
				// from one thread.
				const volatile char* const current_buffer_end_ptr = buffer + buffer_size + (bool)empty_buffer * buffer_size;

				const volatile char* read_end_ptr = buffer_user_read_head + output_size;
				if (read_end_ptr < current_buffer_end_ptr) {
					std::copy(buffer_user_read_head, read_end_ptr, output_ptr);
					buffer_user_read_head = read_end_ptr;
					return orig_output_size;
				}

				std::copy(buffer_user_read_head, current_buffer_end_ptr, output_ptr);
				const size_t full_space = current_buffer_end_ptr - buffer_user_read_head;
				output_ptr += full_space;
				output_size -= full_space;
	
				while (buffer_read_pending) { }

				if (finalize_reader_thread) { return -1; }

				buffer_stream_write_head_copy = buffer_stream_write_head;

				buffer_read_pending = true;
				empty_buffer = !empty_buffer;

				// NOTE: We do this here because:
				// 1. We don't want error (finalize_reader_thread) to cause buffer bytes to be eaten, which would happen if this were above the if-stm.
				// 2. We use the inverted value of empty_buffer, which is only accessible here.
				buffer_user_read_head = buffer + (bool)empty_buffer * buffer_size;

				if (buffer_stream_write_head_copy != nullptr) {
					const volatile char* read_end_ptr = minimum_value(buffer_user_read_head + output_size, buffer_stream_write_head_copy);
					std::copy(buffer_user_read_head, read_end_ptr, output_ptr);
					const size_t amount_read = read_end_ptr - buffer_user_read_head;
					buffer_user_read_head = read_end_ptr;
					return orig_output_size - output_size + amount_read;
				}
			}
		}

		// NOTE: As of this moment, I'm standardizing the fact that calling this function more than once and/or calling the initialize() function after calling this function is UNDEFINED.
		// REASON: for the former: implementation may change ; for the latter: that just straight up doesn't work, probably causes some undefined behavior somewhere or something.
		static void dispose() noexcept {
			if (reader_thread.joinable()) {
				finalize_reader_thread = true;
				empty_buffer = !empty_buffer;
				reader_thread.join();
			}
		}
	};

	template <size_t buffer_size>
	class stdout_stream {
		static inline volatile char buffer[buffer_size * 2];

		static inline volatile char* buffer_user_write_head = buffer;

		static inline std::thread flusher_thread;

		static inline volatile buffer_position_t full_buffer = buffer_position_t::right;
		static inline volatile bool buffer_flush_pending = false;

		static inline volatile size_t flush_size = buffer_size;

		static inline volatile bool finalize_flusher_thread = false;

		static void flusher_thread_code() noexcept {
			while (true) {
				while (full_buffer == buffer_position_t::right) { }

				if (finalize_flusher_thread) { return; }

				if (crossplatform_write(STDOUT_FILENO, (char*)buffer, flush_size) == -1) {
					finalize_flusher_thread = true;
					buffer_flush_pending = false;
					return;
				}

				buffer_flush_pending = false;

				while (full_buffer == buffer_position_t::left) { }

				if (finalize_flusher_thread) { return; }

				if (crossplatform_write(STDOUT_FILENO, (char*)(buffer + buffer_size), flush_size) == -1) {
					finalize_flusher_thread = true;
					buffer_flush_pending = false;
					return;
				}

				buffer_flush_pending = false;
			}
		}

	public:
		// NOTE: As above, UNDEFINED to call this more than once.
		static void initialize() noexcept {
			flusher_thread = std::thread((void(*)())flusher_thread_code);
		}

		static bool write(const char* input_ptr, size_t input_size) noexcept {
			// NOTE: We could have done this branchless, but we're optimizing for small writes, which makes this more optimal than branchless in this case.
			if (full_buffer == buffer_position_t::left) {
				while (true) {
					// NOTE: Converting the full_buffer bool to another integer type is totally fine, since converted bools
					// always equal 0 or 1, all other non-zero values get transformed to 1 on conversion.
					// NOTE: Unless of course bools cannot contain other non-zero values because converting to bool might snap to
					// 0 or 1 already. That's an implementation detail though. Whether that detail is implemented by the
					// standard or left up to the compiler I cannot say without further research. I think compiler though.
					const size_t free_space = buffer + buffer_size * 2 - buffer_user_write_head;
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
					full_buffer = buffer_position_t::right;

					buffer_user_write_head = buffer;
				}
			} else {
				while (true) {
					const size_t free_space = buffer + buffer_size - buffer_user_write_head;
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
					full_buffer = buffer_position_t::left;

					buffer_user_write_head = buffer + buffer_size;
				}
			}
		}

		static bool flush() noexcept {
			// Wait for other buffer to finish flushing.
			while (buffer_flush_pending) { }

			// If error occurred, report it.
			if (finalize_flusher_thread) { return false; }

			// Set flush_size to the exact amount that still needs to be flushed.
			flush_size = buffer_user_write_head - (buffer + (bool)full_buffer * buffer_size);

			// Start flush.
			buffer_flush_pending = true;
			full_buffer = !full_buffer;

			// Wait for it to finish.
			while (buffer_flush_pending) { }

			// Reset flush_size to default.
			flush_size = buffer_size;

			// Though both buffers are empty (theoretically we could set this to "global" start), it needs to be at start of correct buffer
			// for the rest of the system to work.
			buffer_user_write_head = buffer + (bool)full_buffer * buffer_size;
			// NOTE: We could replace the above branchless version with a branch over the whole function body, but we're optimizing for sparse flushing,
			// which makes this our best option.

			return true;
		}

		// NOTE: Calling this function more than once is UNDEFINED as per my standard for this header.
		static bool dispose() noexcept {
			if (!flush()) { return false; }
			finalize_flusher_thread = true;
			full_buffer = !full_buffer;
			flusher_thread.join();
			return true;
		}
	};

}
