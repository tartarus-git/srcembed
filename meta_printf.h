#pragma once

#include <type_traits>

#include <cstdint>
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
	// 	- you can accept the const char (&)[N] (and probably also const char*) as args of constexpr/consteval functions.
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

	template <size_t string_size>
	using meta_string = meta_array<char, string_size>;

	template <size_t string_size>
	consteval auto construct_meta_string(const char (&string)[string_size]) {
		meta_string<string_size - 1> result;
		for (size_t i = 0; i < string_size - 1; i++) { result[i] = string[i]; }
		return result;
	}

	namespace printf {

		enum class op_type : uint8_t { INVALID, NOOP, TEXT, UINT8, EOFOP };

		struct parse_table_element {
			uint8_t next_state;
			op_type op_type;
		};

		consteval auto generate_blueprint_parse_table() {
			meta_array<parse_table_element, 129 * 3> table { };		// create table filled with invalid characters

			for (uint16_t i = 1 * 129; i < 1 * 129 + 128; i++) {		// make all characters valid for state 1 (except EOF)
				table[i].op_type = op_type::TEXT;
				table[i].next_state = 1;				// all state 1 characters loop back to state 1
			}

			table[1 * 129 + '%'].op_type = op_type::NOOP;			// '%' isn't a finished operation
			table[1 * 129 + '%'].next_state = 2;				// also brings state to special operation state

			table[2 * 129 + 'u'].op_type = op_type::UINT8;			// 'u' finishes operation, so mark it
			table[2 * 129 + 'u'].next_state = 1;				// also brings special operation back to text state

			table[1 * 129 + 128].op_type = op_type::EOFOP;			// text state EOF is the only valid one, mark it

			return table;
		}

		constexpr auto blueprint_parse_table = generate_blueprint_parse_table();

		struct blueprint_string {
			const char* ptr;
			size_t length;
		};

		struct op {
			op_type type;
			blueprint_string text;
		};

		template <size_t num_of_operations>
		using program = meta_array<op, num_of_operations>;

		template <size_t blueprint_length>
		consteval size_t calculate_num_of_operations(const meta_string<blueprint_length>& blueprint) {
			size_t result = 0;

			unsigned char state = 1;

			bool text_encountered = false;

			for (size_t i = 0; i < blueprint_length; i++) {
				parse_table_element table_entry = blueprint_parse_table[state * 129 + blueprint[i]];

				switch (table_entry.op_type) {

				case op_type::INVALID: throw "meta_printf blueprint invalid";

				case op_type::NOOP:
					state = table_entry.next_state;
					text_encountered = false;
					break;

				case op_type::TEXT:
					state = table_entry.next_state;
					if (!text_encountered) {
						result++;
						text_encountered = true;
					}
					break;

				case op_type::UINT8:
					state = table_entry.next_state;
					result++;
					break;

				}
			}
			if (blueprint_parse_table[state * 129 + 128].op_type == op_type::INVALID) { throw "meta_printf blueprint invalid"; }

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

				case op_type::INVALID: throw "meta_printf blueprint invalid";

				case op_type::NOOP:
					state = table_entry.next_state;
					if (text_encountered) {
						program[operation_index].type = op_type::TEXT;
						program[operation_index].text.ptr = blueprint.data + text_begin_index;
						program[operation_index].text.length = i - text_begin_index;
						operation_index++;
						text_encountered = false;
					}
					break;

				case op_type::TEXT:
					state = table_entry.next_state;
					if (!text_encountered) {
						text_begin_index = i;
						text_encountered = true;
					}
					break;

				case op_type::UINT8:
					state = table_entry.next_state;
					program[operation_index++].type = op_type::UINT8;
					break;

				}
			}

			if (blueprint_parse_table[state * 129 + 128].op_type == op_type::INVALID) { throw "meta_printf blueprint invalid"; }

			if (text_encountered) {
				program[operation_index].type = op_type::TEXT;
				program[operation_index].text.ptr = blueprint.data + text_begin_index;
				program[operation_index].text.length = sizeof(blueprint) - text_begin_index;
			}

			return program;
		}

		template <const auto& program, size_t operation_index>
			// TODO: Don't use C-style variadic functions because those are disgusting. Use parameter packs and such, there is a way,
			// don't worry.
		void execute_program(char* buffer, ...) noexcept {
			// NOTE: We have to put constexpr here because or else the lower if statement will get processed even when it doesn't
			// need to be. This doesn't seem like an issue, but it is:
			// Since the lower if has constexpr, the expression inside is evaluated while generating the AST (as opposed to later in compile-time or even in runtime), to determine if the if-statements
			// below it will need to be analysed. If expression below it contains an out-of-bounds array access, it suddenly isn't a
			// constant expression anymore, causing a compilation error.
			// To avoid the case where it has an out-of-bounds array access, we stop AST generation before it gets there by making this if
			// statement constexpr as well.
			// NOTE: If you remove constexpr from the lower if, this problem goes away, but not really.
			// Basically, everything works fine because the if condition doesn't need to be a constant expression anymore (it is now translated into AST instead of being interpreted by the AST generator),
			// but now the AST generation doesn't have a stop condition so a recursion limit is hit in the compiler and compilation doesn't
			// succeed.
			// The constexpr is necessary to stop the AST generation at some point, or else it would explode everytime.
			// PROBLEM:
			// If this function would be evaluated at compile-time, the compiler could interpret the AST while generating it, and
			// every branch would be predictable. Basically, the AST generation wouldn't need to explode because after a certain point,
			// the exit condition would be reached and the AST would have no reason to keep generating.
			// SOLUTION: This is a fair point and totally doable as far as compiler design goes, but it's suboptimal because now,
			// your function will compile fine when called from compile-time and fail compilation when called from runtime.
			// Imagine your writing a function without knowing from where it will be used, you obviously want these types of AST
			// overflows to cause compilation errors so that they don't slip under the radar, which is presumably why the compiler
			// adheres to runtime behaviour even in compile-time, in this case.
			if constexpr (operation_index >= sizeof(program) / sizeof(op)) {
				*buffer = '\0';
				return;
			}
			else if constexpr (program[operation_index].type == op_type::TEXT) {
				std::copy(program[operation_index].text.ptr, program[operation_index].text.ptr + program[operation_index].text.length, buffer);
				execute_program<program, operation_index + 1>(buffer + program[operation_index].text.length);
				return;
			}
			else if constexpr (program[operation_index].type == op_type::UINT8) {
				// TODO: Read uint8 from source somehow.
				return;
			}
		}
	}
}

// We have to use static constexpr variable here because binding non-static constexpr to template non-type doesn't work since the address of the variable could potentially be run-time dependant.
// static makes the variable be located in some global memory, which (as far as the binary file is concerned) has constant addresses.
#define meta_sprintf_test(buffer, blueprint, ...) namespace { static constexpr auto meta_printf_blueprint = meta::construct_meta_string(blueprint); static constexpr auto program = meta::printf::create_program<meta_printf_blueprint>(); meta::printf::execute_program<program, 0>(buffer __VA_OPT__(,) __VA_ARGS__); }
// TODO: Write about the static constexpr problem and how it technically could cause issues sometimes, but we don't care anyway because in those situations, it would be the same between invocations anyway, just remember stackoverflow.
