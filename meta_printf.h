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

	// TODO: You know the drill, use concepts here instead of enable_if.
	template <typename integral_type, typename std::enable_if<std::is_integral<integral_type>{}, bool>::type = false>
		// TODO: Why does this cause a linker error when something inside this function loops forever for example?
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

		// TODO: Use concepts to constrain this to integral types.
		template <typename integral_type, typename std::enable_if<std::is_integral<integral_type>{}, bool>::type = false>
		// TODO: This function could 100% be made a fair bit faster, I just don't know exactly how that works yet.
		constexpr size_t write_integral(char* buffer, integral_type input) {
			char temp_buffer[get_max_digits_of_integral_type<integral_type>()];
			char* temp_buffer_end_ptr = temp_buffer + sizeof(temp_buffer);
			char* temp_buffer_ptr = temp_buffer_end_ptr - 1;

			if (std::is_signed<integral_type>{}) {
				if (input < 0) {
					while (true) {
						*(temp_buffer_ptr--) = -(input % 10) + '0';
						input /= 10;
						if (input == 0) { break; }
					}
					*temp_buffer_ptr = '-';
				}
			}
			while (true) {
				if (input < 10) { break; }
				*(temp_buffer_ptr--) = (input % 10) + '0';
				input /= 10;
			}
			*temp_buffer_ptr = input + '0';

			std::copy(temp_buffer_ptr, temp_buffer_end_ptr, buffer);

			return temp_buffer_end_ptr - temp_buffer_ptr;
		}

		template <const auto& program, size_t operation_index>
		// TODO: Add note about variadic functions and what we're doing here.
		void execute_program(char* buffer) noexcept {
			// TODO: Move comment from second overload into this overload since it's above.
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

		template <const auto& program, size_t operation_index, typename first_arg_type, typename... rest_arg_types>
		void execute_program(char* buffer, first_arg_type first_arg, rest_arg_types... rest_args) noexcept {
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
				*buffer = '\0';
				return;
			}
			else if constexpr (program[operation_index].type == op_type::TEXT) {
				std::copy(program[operation_index].text.ptr, program[operation_index].text.ptr + program[operation_index].text.length, buffer);
				execute_program<program, operation_index + 1>(buffer + program[operation_index].text.length, first_arg, rest_args...);
				return;
			}
			else if constexpr (program[operation_index].type == op_type::UINT8) {
				// TODO: Change this to use some sort of is_implicitly_castable thing.
				static_assert(std::is_same<first_arg_type, uint8_t>{}, "meta_printf failed: one or more input args have incorrect types");
				size_t bytes_written = write_integral<uint8_t>(buffer, first_arg);
				execute_program<program, operation_index + 1>(buffer + bytes_written, rest_args...);
				return;
			}
		}
	}
}

// We have to use static constexpr variable here because binding non-static constexpr to template non-type doesn't work since the address of the variable could potentially be run-time dependant.
// static makes the variable be located in some global memory, which (as far as the binary file is concerned) has constant addresses.
#define meta_sprintf_test(buffer, blueprint, ...) { static constexpr auto meta_printf_blueprint = meta::construct_meta_string(blueprint); static constexpr auto program = meta::printf::create_program<meta_printf_blueprint>(); meta::printf::execute_program<program, 0>(buffer __VA_OPT__(,) __VA_ARGS__); }
// NOTE: We enclose the macro body in a scope (not the same thing as an unnamed namespace) so that it may be used multiple times by the caller.
// NOTE: This causes some weirdness in the static constexpr variables:
//	- what makes sense is that subsequent usages of the macro have different static constexpr variables so everything is fine
//	- what about using the macro over and over again in a loop though?
//		- well there, the static constexpr vars are in fact be the same variable, which then only gets initialized once,
//			but it doesn't matter because there is no situation where the value would need to change in such a construction.
// NOTE: So basically, everythings good!
