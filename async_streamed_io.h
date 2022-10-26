#pragma once

#include <unistd.h>

#include <algorithm> // TODO: For std::copy right?

#include <thread>

#include <iostream>

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

		static constexpr volatile char* buffer_half_end_ptr = buffer + buffer_size;

		// TODO: Do these both have to be nullptr, probs not.
		static inline volatile char* buffer_stream_write_head = nullptr;
		static inline volatile char* buffer_stream_write_head_copy = nullptr;
		static inline volatile char* buffer_user_read_head = buffer;

		static inline std::thread reader_thread;

		static inline volatile buffer_position_t empty_buffer = buffer_position_t::right;
		static inline volatile bool buffer_read_pending = false;

		static inline volatile bool finalize_reader_thread = false;

		static ssize_t read_full_buffer(volatile char* buf, size_t count) noexcept {
			volatile char* original_buf_ptr = buf;
			std::cerr << "got here\n";
			while (true) {
				if (finalize_reader_thread) { return -2; }

				ssize_t bytes_read = ::read(STDIN_FILENO, (char*)buf, count);
				bool nothing_to_read = false;
				if (bytes_read == -1 && !(nothing_to_read = (errno == EAGAIN || errno == EWOULDBLOCK))) {
					std::cerr << "got hard error on reading full buffer\n";
					std::cerr << errno << '\n';
					std::cerr << buf - original_buf_ptr << '\n';
					std::cerr << count << '\n';
					return -3;
				}
				std::cerr << bytes_read << '\n';
				if (bytes_read == 0) { return buf - original_buf_ptr; }
				bytes_read += nothing_to_read;
				// TODO: Test if the above line is faster than an if statement, look at assembly, stuff like that.
				// It turns -1 to 0 when EAGAIN or EWOULDBLOCK is there. Consider that stdin isn't always a regular file.

				count -= bytes_read;
				if (count == 0) {
					std::cerr << "hit full exit scenario\n";
					return -1; }
				buf += bytes_read;
			}
		}

		static void reader_thread_code() noexcept {
			while (true) {
				while (empty_buffer == buffer_position_t::left) { }

				ssize_t read_result = read_full_buffer(buffer + buffer_size, buffer_size);
				std::cerr << "got past read_full_buffer\n";
				switch (read_result) {
				case -3:
					std::cerr << "read_full_buffer got error returned\n";
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case -2:
					std::cerr << "wanted to exit\n";
					return;
				case -1:
					 std::cerr << "full return\n";
					 break;
				default:
					 std::cerr << "right end state hit\n";
					 buffer_stream_write_head = buffer + buffer_size + read_result;
					 buffer_read_pending = false;
					 return;
				}

				buffer_read_pending = false;

				while (empty_buffer == buffer_position_t::right) { }

				read_result = read_full_buffer(buffer, buffer_size);
				std::cerr << "got past read_full_buffer\n";
				switch (read_result) {
				case -3:
					std::cerr << "read_full_buffer got error returned\n";
					finalize_reader_thread = true;
					buffer_read_pending = false;
				case -2:
					std::cerr << "wanted to exit\n";
					return;
				case -1:
					 std::cerr << "full return\n";
					 break;
				default:
					 std::cerr << "left end state hit\n";
					 buffer_stream_write_head = buffer + read_result;
					 buffer_read_pending = false;
					 return;
				}

				buffer_read_pending = false;
			}
		}

	public:
		static bool initialize() noexcept {
			int stdin_fd_flags = fcntl(STDIN_FILENO, F_GETFL);
			if (stdin_fd_flags == -1) { return false; }
			std::cerr << "starting fcntl\n";
			if (fcntl(STDIN_FILENO, F_SETFL, stdin_fd_flags | O_NONBLOCK) == -1) { return false; }
			std::cerr << "finished fcntl\n";

			ssize_t read_result = read_full_buffer(buffer, buffer_size);
			switch (read_result) {
			case -3: return false;
			// case -2: while (true) { }	<-- shouldn't ever happen
			case -1: break;
			default:
				 std::cerr << "hit other thing\n";
				 std::cerr << (const char*)buffer << '\n';
				 buffer_stream_write_head_copy = buffer + read_result;
				 std::cerr << "set write head\n";
				 return true;
			}

			std::cerr << "starting stdin thread\n";

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
			return a < b ? a : b;
			// TODO: Or like this?:
			//return a + (b - a) * (a < b);
		}

		// NOTE: Behaviour is super duper undefined if you call this after encountering EOF.
		// BTW: EOF is denoted by returning less than output_size. After that, no more calling this function!
		// TODO: Actually, above note isn't true anymore, check to make sure though.
		static ssize_t read(char* output_ptr, size_t output_size) noexcept {
			volatile char* read_end_ptr = buffer_user_read_head + output_size;
			size_t todo_change_later_orig_output_size = output_size;
			char* todo_change_later_orig_output_ptr = output_ptr;
			while (true) {
				std::cerr << "buwh: " << buffer_user_read_head - buffer << '\n';
				// NOTE: The volatile after the * is the only volatile that is relevant for a theoretical pure
				// dereference, without an attached read or write to the variable. Idk if that exists in
				// C++ because every dereference I've ever seen is attached to a read or write, which then also
				// looks at the volatile before the *.
				// The only volatile necessary here is the one before the *, since the pointer itself is only accessed
				// from one thread.
				volatile char* const current_buffer_end_ptr = buffer_half_end_ptr + (bool)empty_buffer * buffer_size;
				//std::cerr << "looped the thing\n";

				if (buffer_stream_write_head_copy != nullptr) {
					std::cerr << "hit thing\n";
					read_end_ptr = minimum_value(read_end_ptr, buffer_stream_write_head_copy);
					size_t amount_read = std::copy(buffer_user_read_head, read_end_ptr, output_ptr) - todo_change_later_orig_output_ptr;
					buffer_user_read_head = read_end_ptr;
					return amount_read;
				}
				// TODO: Read up on exception overhead again, and then try to disable exceptions on this build because there is overhead on x86 even when no exception gets triggered.

				if (read_end_ptr < current_buffer_end_ptr) {
					std::copy(buffer_user_read_head, read_end_ptr, output_ptr);
					buffer_user_read_head = read_end_ptr;
					return todo_change_later_orig_output_size;
				}
	
				std::copy(buffer_user_read_head, current_buffer_end_ptr, output_ptr);
				const size_t full_space = current_buffer_end_ptr - buffer_user_read_head;
				output_ptr += full_space;
				output_size -= full_space;
	
				while (buffer_read_pending) { }
				if (finalize_reader_thread) { return -1; }

				buffer_stream_write_head_copy = buffer_stream_write_head;

				// NOTE: We do this here
				// so that an error doesn't cause buffer bytes to be eaten (it would do that if this were above the if-stm)
				buffer_user_read_head = current_buffer_end_ptr - (bool)empty_buffer * (buffer_size * 2);
				read_end_ptr = buffer_user_read_head + output_size;
				// TODO: See if this would be faster with an if-statement, considering that we call read with small chunks
				// instead of big ones.
				// TODO: Put it down like you did the one below, looks better and is more understandable.

				buffer_read_pending = true;
				empty_buffer = !empty_buffer;
			}
		}

		static void dispose() noexcept {
			std::cerr << "hi there got to thing\n";
			if (reader_thread.joinable()) {
				finalize_reader_thread = true;
				empty_buffer = !empty_buffer;
				reader_thread.join();
			}
			std::cerr << "joined thread 1\n";
		}
	};

	template <size_t buffer_size>
	class stdout_stream {
		static inline volatile char buffer[buffer_size * 2];

		static constexpr volatile char* buffer_half_end_ptr = buffer + buffer_size;

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

				if (::write(STDOUT_FILENO, (char*)buffer, flush_size) == -1) {
					finalize_flusher_thread = true;
					buffer_flush_pending = false;
					return;
				}

				buffer_flush_pending = false;

				while (full_buffer == buffer_position_t::left) { }

				if (finalize_flusher_thread) { return; }

				if (::write(STDOUT_FILENO, (char*)(buffer + buffer_size), flush_size) == -1) {
					finalize_flusher_thread = true;
					buffer_flush_pending = false;
					return;
				}

				buffer_flush_pending = false;
			}
		}

	public:
		static void initialize() noexcept {
			flusher_thread = std::thread((void(*)())flusher_thread_code);
		}

		static bool write(const char* input_ptr, size_t input_size) noexcept {
			while (true) {
				// NOTE: Converting the full_buffer bool to another integer type is totally fine, since converted bools
				// always equal 0 or 1, all other non-zero values get transformed to 1 on conversion.
				// NOTE: Unless of course bools cannot contain other non-zero values because converting to bool might snap to
				// 0 or 1 already. That's an implementation detail though. Whether that detail is implemented by the
				// standard or left up to the compiler I cannot say without further research.
				const size_t free_space = buffer_half_end_ptr + (bool)full_buffer * buffer_size - buffer_user_write_head;
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
				buffer_user_write_head = buffer + (bool)full_buffer * buffer_size;
			}
		}

		static bool flush() noexcept {
			// Set flush_size to the exact amount that still needs to be flushed.
			flush_size = buffer_user_write_head - (buffer + (bool)full_buffer * buffer_size);

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
			buffer_user_write_head = buffer + (bool)full_buffer * buffer_size;

			return true;
		}

		static void dispose() noexcept {
			std::cerr << "got to flusher disposal\n";
			flush();		// TODO: Report error here.
			finalize_flusher_thread = true;
			full_buffer = !full_buffer;
			flusher_thread.join();
		}
	};

}
