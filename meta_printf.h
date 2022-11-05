#pragma once

#include <type_traits>

#include <cstdint>
#include <limits>

#include <algorithm>

namespace meta {

	// Facilities for handling strings at compile-time:
	// NOTE: You have the following elsewhere as well probably, but just to reiterate:
	// 	- you can accept const char*'s as template inputs, but you can't send string literals through those inputs.
	//		- The mechanics of exactly how this is disallowed are weird, but the reasoning is somewhat understandable:
	//			- string literals don't have to have same address even if they are same, in fact sometimes they don't have
	//			any addresses because they can be represented as immediates in some instructions apparently.
	//			- that means two types might look the same but be totally different to the naked eye, which isn't nice.
	//	- accepting const char*'s is still useful though, if those const char*'s don't point to literals but point to arrays and such.
	//	- you can also accept const auto& to accept types (among others) of const char (&)[N].
	//		- you can't put string literals into those either, but you can put references to static constexpr char arrays in there.
	//		- you can probably shrink the possible inputs to just char arrays if you make use of concepts, but that's a different
	//		story.
	// 	- you can accept the const char (&)[N] (and also const char*) as args of constexpr/consteval functions.
	//		- this works with string literals, because the type can't be messed up like above, so the language is ok with it.
	//		- this works and has the added bonus that you can use template arg deduction to figure out the length of the char array.
	// 		- PROBLEM: You can't pass the args into nested-functions because the args aren't considered constant expressions.
	//			(super interesting, for reasoning see below somewhere)
	//	- The only practical alternative that is left open to you is to put an array in a struct and pass that around by value
	//	within the template args. If you don't plan on passing text to nested functions you can also accept it in function args of constexpr
	//	or consteval function as mentioned above.

	template <typename data_type, size_t length>
	struct meta_array {
		data_type data[length];

		// NOTE: These should be consteval's, but due to a clang bug, this only works properly with constexpr.
		// Hopefully this will get fixed in the future.
		constexpr data_type& operator[](size_t index) { return data[index]; }
		constexpr const data_type& operator[](size_t index) const { return data[index]; }
		// NOTE: cv and rvalue/lvalue ref markers towards the end of the function declaration affect the invisible this
		// argument and thereby participate in overload resolution. That means that the above two functions can both exist
		// at the same time as two different overloads. The const version will be selected when that one more closely matches
		// the input parameters.
	};

	template <typename data_type, size_t length>
	consteval auto construct_meta_array(const data_type (&source_array)[length]) {
		meta_array<data_type, length> result;
		// NOTE: std::copy should be constexpr but it isn't because of outdated stdlib, see below somewhere.
		//std::copy(source_array, source_array + length, result.data);
		for (size_t i = 0; i < length; i++) { result[i] = source_array[i]; }
		return result;
	}

	template <size_t string_size>
	using meta_string = meta_array<char, string_size>;

	template <size_t string_size>
	consteval auto construct_meta_string(const char (&string)[string_size]) {
		meta_string<string_size - 1> result;
		// NOTE: Same deal as above.
		//std::copy(string, string + string_size - 1, result.data);
		for (size_t i = 0; i < string_size - 1; i++) { result[i] = string[i]; }
		return result;
	}

	template <size_t size>
	using meta_byte_array = meta_array<uint8_t, size>;

	template <typename integral_type, typename std::enable_if<std::is_integral<integral_type>{}, bool>::type = false>
	// NOTE: This function is the subject of a linker error when something for example loops forever inside of it.
	// The runtime code wants to link to this function, but it isn't there in the object files since it never exists at runtime.
	// GCC doesn't do this, it handles the issue properly, this seems like a Clang bug.
	// Seems like partial confusion between constexpr and consteval on the compiler's part.
	// TODO: Ask some people (stackoverflow) and possibly report this bug.
	consteval size_t get_max_digits_of_integral_type() {
		integral_type value;
		size_t result;
		if constexpr (std::is_signed<integral_type>{}) {
			value = std::numeric_limits<integral_type>::min();
			result = 1;
		}
		else {
			value = std::numeric_limits<integral_type>::max();
			result = 0;
		}

		while (true) {
			// NOTE: Can't do the following because the order of execution of the left and right sides of the inequality is
			// undefined. Hence the whole thing is undefined behaviour.
			//if (value != (value /= 10)) { result++; }

			if (value == 0) { break; }
			result++;
			value /= 10;
		}

		return result;
	}

	void report_consteval_error(const char* message);

	namespace printf {

		// NOTE: We stopped using iterators because they require use of fputc with every character.
		// NOTE: The new system can fwrite large chunks, which is faster.

		/*
		// NOTE: In case (in the future) you think this iterator is botched and not standards-conforming,
		// it is AFAIK completely conformant to the standard. Everything is A-OK!
		class streamed_stdout_it {
			std::ptrdiff_t amount_of_bytes_written = 0;
			// NOTE: The above setting to 0 causes value initialization to revert to default initialization.
			// Because a default constructor is generated to handle setting the variable to 0 on initialization,
			// value initialization isn't allowed to value initialize anymore.
			char buffer;

		public:
			using difference_type = std::ptrdiff_t;
			using value_type = decltype(buffer);
			using pointer = void;
			using reference = void;
			using iterator_category = std::output_iterator_tag;

			static consteval streamed_stdout_it make_compile_time_instance() {
				streamed_stdout_it result;
				result.buffer = 0;
				return result;
			}

			constexpr char& operator*() noexcept { return buffer; }
			constexpr const char& operator*() const noexcept { return buffer; }

			streamed_stdout_it& operator++() noexcept {
				if (amount_of_bytes_written == -1) { return *this; }
				if (fputc(buffer, stdout) == EOF) {
					amount_of_bytes_written = -1;
					return *this;
				}
				amount_of_bytes_written++;
				return *this;
			}

			// NOTE: The following function isn't part of LegacyOutputIterator, which is what this class is conforming to.
			// Luckily, extra functions don't hurt so this doesn't bother anyone.
			constexpr std::ptrdiff_t operator-(const streamed_stdout_it& other) const noexcept {
				return amount_of_bytes_written - other.amount_of_bytes_written;
			}
		};
		*/

		class memory_outputter {
			char* inner_ptr;

		public:
			constexpr memory_outputter(char* ptr) noexcept : inner_ptr(ptr) { }

			// NOTE: This should be able to be constexpr since std::copy should be constexpr,
			// but it isn't because I think I'm running an outdated stdlib.
			// There isn't any newer one for my system though (I think, at least not on apt), so it is what it is.
			void copy_input_from_ptr(const char* ptr, const char* end_ptr) noexcept {
				inner_ptr = std::copy(ptr, end_ptr, inner_ptr);
			}

			void copy_input_from_ptr(const char* ptr, size_t size) noexcept {
				copy_input_from_ptr(ptr, ptr + size);
			}

			constexpr void write_single_byte_no_increment(char byte) noexcept {
				*inner_ptr = byte;
			}

			constexpr void write_single_byte(char byte) noexcept {
				write_single_byte_no_increment(byte);
				inner_ptr++;
			}

			constexpr std::ptrdiff_t operator-(const memory_outputter& other) const noexcept {
				return inner_ptr - other.inner_ptr;
			}
		};

		class streamed_stdout_outputter {
			std::ptrdiff_t amount_of_bytes_written = 0;

		public:
			void copy_input_from_ptr(const char* ptr, size_t size) noexcept {
				if (amount_of_bytes_written == -1) { return; }
				if (!stdout_stream::write(ptr, size)) { amount_of_bytes_written = -1; return; }
				amount_of_bytes_written += size;
			}

			void copy_input_from_ptr(const char* ptr, const char* end_ptr) noexcept {
				copy_input_from_ptr(ptr, end_ptr - ptr);
			}

			void write_single_byte_no_increment(char byte) noexcept {
				if (amount_of_bytes_written == -1) { return; }
				if (!stdout_stream::write(&byte, sizeof(byte))) { amount_of_bytes_written = -1; }
			}

			void write_single_byte(char byte) noexcept {
				write_single_byte_no_increment(byte);
				if (amount_of_bytes_written != -1) { amount_of_bytes_written++; }
			}

			// NOTE: Be careful with this, if both instances have error set, then this will return 0 and not -1 like expected.
			constexpr std::ptrdiff_t operator-(const streamed_stdout_outputter& other) const noexcept {
				return amount_of_bytes_written - other.amount_of_bytes_written;
			}
		};

		enum class op_type_t : uint8_t { INVALID, NOOP, TEXT, UINT8, EOFOP };

		struct parse_table_element {
			uint8_t next_state;

			//op_type op_type;		// NOTE: You're not allowed to hide types with other types or variables.
							// NOTE: Even though Clang allows it in this case for some reason. Probs compiler bug.

			op_type_t op_type;
		};

		consteval auto generate_blueprint_parse_table() {
			meta_array<parse_table_element, 129 * 3> table { };		// create table filled with invalid characters

			for (uint16_t i = 1 * 129; i < 1 * 129 + 128; i++) {		// make all characters valid for state 1 (except EOF)
				table[i].op_type = op_type_t::TEXT;
				table[i].next_state = 1;				// all state 1 characters loop back to state 1
			}

			table[1 * 129 + '%'].op_type = op_type_t::NOOP;			// '%' isn't a finished operation
			table[1 * 129 + '%'].next_state = 2;				// also brings state to special operation state

			table[2 * 129 + 'u'].op_type = op_type_t::UINT8;		// 'u' finishes operation, so mark it
			table[2 * 129 + 'u'].next_state = 1;				// also brings special operation back to text state

			table[1 * 129 + 128].op_type = op_type_t::EOFOP;		// text state EOF is the only valid one, mark it

			return table;
		}

		inline constexpr auto blueprint_parse_table = generate_blueprint_parse_table();

		struct blueprint_string {
			const char* ptr;
			size_t length;
		};

		struct op {
			op_type_t type;
			blueprint_string text;
		};

		template <size_t num_of_operations>
		using program = meta_array<op, num_of_operations>;

		template <size_t blueprint_length>
		consteval size_t calculate_num_of_operations(const meta_string<blueprint_length>& blueprint) {
			size_t result = 0;

			unsigned char state = 1;

			bool text_encountered = false;
			op_type_t last_op = op_type_t::INVALID;

			for (size_t i = 0; i < blueprint_length; i++) {
				parse_table_element table_entry = blueprint_parse_table[state * 129 + blueprint[i]];

				switch (last_op = table_entry.op_type) {

				case op_type_t::INVALID: report_consteval_error("meta_printf invalid: blueprint invalid");

				case op_type_t::NOOP:
					state = table_entry.next_state;
					text_encountered = false;
					break;

				case op_type_t::TEXT:
					state = table_entry.next_state;
					if (!text_encountered) {
						result++;
						text_encountered = true;
					}
					break;

				case op_type_t::UINT8:
					state = table_entry.next_state;
					result++;
					break;

				}
			}
			if (blueprint_parse_table[state * 129 + 128].op_type == op_type_t::INVALID) { report_consteval_error("meta_printf invalid: blueprint invalid"); }

			return result;
		}

		// NOTE: This would still accept const auto& even if we left the const out because the auto automatically accepts const and non-const due
		// to pattern-matching-like behaviour. This is dangerous, and it also means that we can't accept only non-const things when using auto.
		// I'm fairly sure you can fix this situation with concepts, but I haven't explored those yet.
		// It doesn't matter here though because we only want the const things, which we can easily force with const auto&.
		template <const auto& blueprint>
		consteval auto create_program() {
			/*
			   SUPER IMPORTANT NOTE ABOUT CONSTEVAL TYPE STUFF:
			   	- the arguments of constexpr/consteval functions aren't considered constant expressions the avoid the following:
					- if they were constant expressions, you could use them as template args, thereby making
						the type of something dependant on the function args.
					- imagine a function that returns different types based on the given set of function args
					- other than being incredibly weird and crazy, this doesn't work with the C++ type system
						precisely because it's incredibly weird and crazy.
					- determining the return type of a function would be incredibly difficult and/or impossible.
				- all other non-constexpr variables in constexpr/consteval functions are also not constant expressions.
					- it follows from the above since non-constexpr variables are only useful if they depend on
						either function args or global non-constexpr variables.
					- global non-constexpr variable usage in consteval/constexpr functions is disallowed for obvious reasons.
					- this justifies automatic variables in this context not being constant expressions
					- (non-constexpr vars are also useful for loops, but those shouldn't be able to be used
						for template types either for obvious type checking reasons)
				- if arguments for consteval/constexpr functions needed to be constant expressions (at call-site (not the above)),
					then you couldn't nest consteval functions (follows from the above for obvious reasons).
				- for that reason, consteval/constexpr functions can be called without constant expression args, but only
					within other consteval/constexpr functions.
				- it's pretty nicely implemented though:
					- the result of a consteval/constexpr function isn't always constant expression,
						only when the args are constant expression
					- so basically, inside a consteval/constexpr function, you can't use the result of a nested
						consteval/constexpr function for a template arg
					- you CAN however use the result of a top-level consteval/constexpr function as template arg
					---> super interesting!

				- so all in all, in this situation, where we want to pass the blueprint to nested functions and use the result
					for templates, we have to accept the blueprint as a template argument
				- we've covered above that the best way to do this is to make a struct with an array and pass it around
				- we have to pass it around via reference in template args because template args don't accept structs as values
				- that's fine though, references are better anyway
			*/

			program<calculate_num_of_operations(blueprint)> program { };
			size_t operation_index = 0;

			size_t text_begin_index;

			unsigned char state = 1;

			bool text_encountered = false;

			for (size_t i = 0; i < sizeof(blueprint); i++) {
				parse_table_element table_entry = blueprint_parse_table[state * 129 + blueprint[i]];

				switch (table_entry.op_type) {

				// NOTE: throw is illegal in a consteval function, but only if the interpreter actually hits it.
				// This is perfect for us, since we want to trigger an error only if control flow gets here.
				//case op_type_t::INVALID: throw "meta_printf invalid: blueprint invalid";
				// NOTE: In a constexpr function, throw will force evaluation at runtime, since it's illegal at compile-time.
				// NOTE: But constexpr functions must have at least one set of args that allows compile-time execution,
				// so it fails if throw is always hit.
				// PROBLEM: The problem of whether a set of args exists that is compile-time is not easy to solve.
				// There are situations where it is practically unsolvable. In that case I assume the compiler doesn't let
				// you finish compilation even if there theoretically is a route where compile-time evaluation is possible.
				// TODO: Research this and see what actually happens.

				// NOTE: The above method was used when exceptions were enabled. With -fno-exceptions, the compiler doesn't let us
				// use the throw keyword. Calling a function with a declaration but not an implementation is another good way to do this though.
				// FUN FACT: report_consteval_error is a normal function, it isn't consteval or constexpr, although it could be if I wanted it to be.
				// It still works great with normal functions, which seems weird. I guess it simply has to do with the order in which constraints are
				// enforced in the compiler (this order is standardized (at least when it comes to this) AFAIK, so don't worry).
				// I imagine syntax is checked first, then non-dependent type checking is done, then instantiation is done, then dependant
				// type checking is done, then the AST that resulted from instantiation is interpreted. While it is interpreted, any function calls
				// that the interpreter hits are checked to make sure they are all compile-time and that the implementations exist.
				// One could certainly do this function checking before interpretation, thereby ruining our hack, but C++ doesn't do this.
				// PROBABLE EXPLANATION:
				// 	why they check the implementation only when the function actually gets used makes sense:
				// 		- you often want to use function types in template constructs without actually calling them, having to implement
				// 			a dummy implementation for each function every time would be a little tiny bit cumbersome.
				//		- maybe that's not the original reason, but that's still a good reason
				//		- maybe the original reason was just compiler efficieny:
				// 			- if you're gonna do the check, you also need to recursively check the called function body and it's subfunctions' bodies.
				//			- avoiding that seems worthwhile.
				// 	THE ABOVE BASICALLY FORCES YOU TO HANDLE COMPILE-TIME CHECKING IN THE SAME WAY:
				// 		- yeah, you could simply check the called functions before interpretation, but then
				// 			what about the functions that those functions call, those must be left out of the process
				//			and only checked once interpretation starts and the implementations are called upon.
				//		- now, you're checking is split up into multiple chunks and any constraints the first chunk offers can be hacked
				// 			by writing the code differently (nesting more).
				//		- you could do it like this, but it's disgusting and stupid, better to wait until interpretation, much cleaner!
				case op_type_t::INVALID: report_consteval_error("meta_printf invalid: blueprint invalid");

				case op_type_t::NOOP:
					state = table_entry.next_state;
					if (text_encountered) {
						program[operation_index].type = op_type_t::TEXT;
						program[operation_index].text.ptr = blueprint.data + text_begin_index;
						program[operation_index].text.length = i - text_begin_index;
						operation_index++;
						text_encountered = false;
					}
					break;

				case op_type_t::TEXT:
					state = table_entry.next_state;
					if (!text_encountered) {
						text_begin_index = i;
						text_encountered = true;
					}
					break;

				case op_type_t::UINT8:
					state = table_entry.next_state;
					program[operation_index++].type = op_type_t::UINT8;
					break;

				}
			}
			if (blueprint_parse_table[state * 129 + 128].op_type == op_type_t::INVALID) { report_consteval_error("meta_printf invalid: blueprint invalid"); }

			if (text_encountered) {
				program[operation_index].type = op_type_t::TEXT;
				program[operation_index].text.ptr = blueprint.data + text_begin_index;
				program[operation_index].text.length = sizeof(blueprint) - text_begin_index;
			}

			return program;
		}

		consteval auto generate_uint8_string_lookup_list() {
			meta_byte_array<256 * 4> result { };		// TODO: Change this to meta_string.
			for (uint16_t i = 0, true_index = 0; i < 256; i++, true_index += 4) {
				uint8_t blank_space = 4;
				uint16_t value = i;
				while (true) {
					result[true_index + (--blank_space)] = (value % 10) + '0';
					if ((value /= 10) == 0) { break; }
				}
				result[true_index] = blank_space;
			}
			return result;
		}

		inline constexpr auto uint8_string_lookup_list = generate_uint8_string_lookup_list();

		template <typename outputter_t>
		constexpr void output_uint8(outputter_t& outputter, uint8_t input) {
			uint16_t lookup_index = input * 4;
			uint8_t blank_space = uint8_string_lookup_list[lookup_index];
			outputter.copy_input_from_ptr((const char*)&uint8_string_lookup_list[lookup_index + blank_space], 4 - blank_space);
		}

		template <const auto& program, size_t operation_index, bool write_nul_terminator, typename outputter_t>
		auto execute_program(outputter_t outputter) noexcept -> outputter_t {
			// NOTE: We have to put constexpr here because or else the lower if statement will get processed even when it doesn't
			// need to be. This doesn't seem like an issue, but it is:
			// Since the lower if has constexpr, the expression inside is evaluated while instantiating the template, to determine if the if-statements
			// below it will need to be instantiated. If expression below it contains an out-of-bounds array access, it suddenly isn't a
			// constant expression anymore, causing a compilation error.
			// To avoid the case where it has an out-of-bounds array access, we stop AST generation before it gets there by making this if
			// statement constexpr as well.
			// NOTE: If you remove constexpr from the lower if, this problem goes away, but not really.
			// Basically, everything works fine because the if condition doesn't need to be a constant expression anymore (it is now translated into AST instead of being interpreted by the AST generator (all of this still strictly on instantiation of the template)),
			// but now the AST generation doesn't have a stop condition so a recursion limit is hit in the compiler and compilation doesn't
			// succeed. --> because the endlessly-recursive function calls all need to be put into the AST.
			// The constexpr is necessary to stop the AST generation at some point, or else it would explode everytime.
			// PROBLEM:
			// If this function would be evaluated at compile-time, the compiler could interpret the AST while generating it, and
			// every branch would be predictable. Basically, the AST generation wouldn't need to explode because after a certain point,
			// the exit condition would be reached and the AST would have no reason to keep generating.
			// SOLUTION: This is a fair point and totally doable as far as compiler design goes, but it's suboptimal because now,
			// your template function will instantiate fine when called from compile-time and fail compilation when called from runtime.
			// (because, assuming we make first if non-constexpr, runtime execution could create out-of-bounds if that if fails).
			// Imagine your writing a function without knowing from where it will be used, you obviously want these types of AST
			// overflows to cause compilation errors so that they don't slip under the radar, which is presumably why the compiler
			// adheres to runtime behaviour even in compile-time, in this case.
			// NOTE: Just remember that it behaves as if it completes generating the post-template-instantiation AST before interpreting it
			// and running compile-time functions.
			if constexpr (operation_index >= sizeof(program) / sizeof(op)) {
				if (write_nul_terminator) { outputter.write_single_byte_no_increment('\0'); }
				return outputter;
			}
			else if constexpr (program[operation_index].type == op_type_t::TEXT) {
				outputter.copy_input_from_ptr(program[operation_index].text.ptr, program[operation_index].text.length);
				return execute_program<program, operation_index + 1, write_nul_terminator>(outputter);
			}
			else if constexpr (program[operation_index].type == op_type_t::UINT8) {
				// NOTE: The condition below cannot be straight false because then the static_assert fires on every build,
				// no matter what.
				// This is because code in the false segments of constexpr if's is still parsed and analysed and such,
				// presumably to avoid letting coding mistakes slip through in the false branches of these if's.
				// Along with a couple other specific things, the main guarantee of constexpr if is that
				// the false branches are not instantiated when the template (if one is present) is instantiated.
				// Types are still checked for things that are not template dependant, and static_asserts that are not
				// template dependant still fire. That's why we need the condition to be template dependant.
				// TODO: Idk if I like the static_assert behaviour with the false in it, ask about it and discuss and such.
				static_assert(operation_index == operation_index + 1, "meta_printf invalid: blueprint requires input arguments");
			}
		}

		template <const auto& program, size_t operation_index, bool write_nul_terminator, typename first_arg_type, typename... rest_arg_types, typename outputter_t>
		// NOTE: We could have used C-style variadic functions here, but that's a mess and I despise that.
		// Instead, we use C++ parameter packs, which are nicer.
		// Just remember that variadic functions have the ... at the end (optionally preceded by a comma) and
		// parameter packs have the ... after the type (or after the typename/class keyword).
		// I'm very sure that the ... of a variadic function cannot accept a C++ parameter pack, so these two concepts are not
		// compatible in any way AFAIK.
		auto execute_program(outputter_t outputter, first_arg_type first_arg, rest_arg_types... rest_args) noexcept -> outputter_t {
			if constexpr (operation_index >= sizeof(program) / sizeof(op)) {
				if (write_nul_terminator) { outputter.write_single_byte_no_increment('\0'); }
				return outputter;
			}
			else if constexpr (program[operation_index].type == op_type_t::TEXT) {
				outputter.copy_input_from_ptr(program[operation_index].text.ptr, program[operation_index].text.length);
				return execute_program<program, operation_index + 1, write_nul_terminator>(outputter, first_arg, rest_args...);
			}
			else if constexpr (program[operation_index].type == op_type_t::UINT8) {
				// NOTE: One could make this more flexible by allowing non-narrowing conversions for example,
				// but I'm gonna pass on that for now, so that the code is more explicit.
				static_assert(std::is_same<first_arg_type, uint8_t>{}, "meta_printf failed: one or more input args have incorrect types");
				output_uint8(outputter, first_arg);
				return execute_program<program, operation_index + 1, write_nul_terminator>(outputter, rest_args...);
			}
		}

		inline constexpr streamed_stdout_outputter stdout_output;
		// NOTE: constexpr variables are const by default, but not inline by default.
		// Since const global variables have internal linkage instead of externel linkage like normal variables,
		// including this header in multiple files would still work without the above inline.
		// Still, the inline is better because with external linkage, the variable only exists in one place in the end binary.
		// NOTE: It might exist in only one place anyway because of optimizations, but you get my point.
	}
}

// We have to use static constexpr variable here because binding non-static constexpr to template non-type doesn't work since the address of the variable could potentially be run-time dependant.
// static makes the variable be located in some global memory, which (as far as the binary file is concerned) has constant addresses.
#define meta_print_to_outputter(outputter, blueprint, write_nul_terminator, ...) [&]() { static constexpr auto meta_printf_blueprint = meta::construct_meta_string(blueprint); static constexpr auto program = meta::printf::create_program<meta_printf_blueprint>(); return meta::printf::execute_program<program, 0, write_nul_terminator>(outputter __VA_OPT__(,) __VA_ARGS__) - outputter; }()
// NOTE: We enclose the macro body in a lambda so that it may return a value in a nice stable fashion.
// NOTE: Also, the scope of the lambda allows us to use the macro multiple times without initialization issues with the static variables.
// NOTE: We could achieve this as well using simple scope brackets ("{ ... }"), but like I said, we want to return things.
// NOTE: Fun fact: namespace { ... } wouldn't work here because unnamed namespaces are only valid outside of functions.
// NOTE: This causes some weirdness in the static constexpr variables:
//	- what makes sense is that subsequent usages of the macro have different static constexpr variables so everything is fine
//	- what about using the macro over and over again in a loop though?
//		- well there, the static constexpr vars are in fact the same variable, which then only gets initialized once,
//			but it doesn't matter because there is no situation where the value would need to change in such a construction.
// NOTE: So basically, everythings good!

#define meta_sprintf(buffer, blueprint, ...) [&]() { meta::printf::memory_outputter mem_output(buffer); return meta_print_to_outputter(mem_output, blueprint, true __VA_OPT__(,) __VA_ARGS__); }()
#define meta_printf(blueprint, ...) meta_print_to_outputter(meta::printf::stdout_output, blueprint, true __VA_OPT__(,) __VA_ARGS__)

#define meta_sprintf_no_terminator(buffer, blueprint, ...) [&]() { meta::printf::memory_outputter mem_output(buffer); return meta_print_to_outputter(mem_output, blueprint, false __VA_OPT__(,) __VA_ARGS__); }()
#define meta_printf_no_terminator(blueprint, ...) meta_print_to_outputter(meta::printf::stdout_output, blueprint, false __VA_OPT__(,) __VA_ARGS__)

// NOTE: Technically, printf functions return ints, and I should definitely make my implementation more conformant to the standard if/when I make a general purpose meta_printf.
// Right now, returning std::ptrdiff_t is fine.
