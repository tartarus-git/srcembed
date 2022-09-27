#pragma once

#include <type_traits>

#include <cstdint>
#include <algorithm>

namespace meta {

	struct PrintfText {
		const char* ptr;
		size_t length;
	};

	enum class PrintfOperationType : uint8_t { NOOP, TEXT, UINT8 };

	struct PrintfOperation {
		PrintfOperationType type;
		union {
			PrintfText text;
		};

		consteval PrintfOperation() : type(PrintfOperationType::NOOP) { }
	};

	template <size_t num_of_operations>
	struct PrintfProgram {
		PrintfOperation operations[num_of_operations];
		static constexpr size_t operations_length = num_of_operations;
	};

	template <size_t printf_blueprint_size>
	consteval size_t calculate_num_of_operations(const char (&printfBlueprint)[printf_blueprint_size]) {
		size_t printf_blueprint_length = printf_blueprint_size - 1;

		size_t result = 1;
		/*for (size_t i = 0; i < printf_blueprint_length; i++) {
			break;
			if (printfBlueprint[i] == '%') { result++; }
			// TODO.
		}*/
		return result;
	}

	template <size_t printf_blueprint_size>
		// TODO: See why you seemingly can't pass const char* strings in non-type template params. Pointers are allowed, why can't we pass strings, since those are global and should have constant addresses.
	consteval auto create_printf_program(const char (&printfBlueprint)[printf_blueprint_size]) {
		size_t printf_blueprint_length = printf_blueprint_size - 1;

		// TODO: Can't pass printfBlueprint into calculate_num_of_operations inside PrintfProgram here because it doesn't think it's compile-time. Stupid C++ unfinished-ness.
		// TODO: Ask about this nested const char& situation on stackoverflow, maybe there is a legitimate reason why the problem exists.
		// TODO: You can fix all this with a really annoying work-around. Create a function that changes the printfBlueprint into a nested compile-time struct linked list thing, that'll do the trick.
		// TODO: OR, you can just count the stuff inside this function, which is annoying but less annoying.
		// TODO: OR, you could copy the data from the ref array into a struct containing an array, then run the calculate function with the struct. This would all be compile-time, so the overhead doesn't matter much. You could make a macro to do this for you and you could make somewhat reasonable code this way. Goddamnit the fact that C++ seemingly never follows through with it's ideas is driving me crazy. Like if your going to do something, do it fucking right goddamnit.
		// TODO: If you apply the structification before calling this function, you could put it inside a function and then no extra macros are required, which is nice.
		PrintfProgram<calculate_num_of_operations(printfBlueprint)> program { };
		size_t operation_index = 0;
		for (size_t i = 0; i < printf_blueprint_length; i++) {
			switch (printfBlueprint[i]) {
			case '%':
				i++;
				if (i < printf_blueprint_size) {
					if (printfBlueprint[i] == 'u') {
						//operation_index++;		// TODO: Make this actually work right.
						program.operations[operation_index].type = PrintfOperationType::UINT8;
						operation_index++;
						continue;
					}
					// TODO: error.
				}
				// TODO: error.
			default:
				if (program.operations[operation_index].type == PrintfOperationType::TEXT) {
					program.operations[operation_index].text.length++;
					continue;
				}
				program.operations[operation_index].type = PrintfOperationType::TEXT;
				program.operations[operation_index].text.ptr = &printfBlueprint[i];
				program.operations[operation_index].text.length = 1;
				continue;
			}
		}

		return program;
	}

	// NOTE: Can't pass objects by value so we have to do it by reference. I would have done it by reference anyway.
	template <const auto& program, size_t operation_index>
	constexpr void execute_printf_program(char* buffer, ...) noexcept {
		if (operation_index == std::remove_reference<typename std::remove_cv<decltype(program)>::type>::type::operations_length) { return; }
		if constexpr (program.operations[operation_index].type == PrintfOperationType::TEXT) {
			std::copy(program.operations[operation_index].text.ptr, program.operations[operation_index].text.ptr + program.operations[operation_index].text.length, buffer);
			execute_printf_program<program, operation_index + 1>(buffer + program.operations[operation_index].text.length);
			return;
		}
	}

	/*template <size_t blueprint_size, typename... input_types>
	constexpr void sprintf(char* buffer, const char (&blueprint)[blueprint_size], input_types... inputs) noexcept {
		auto program = create_printf_program(blueprint);
		execute_printf_program<program, 0>(buffer, inputs...);
	}*/

	// We have to use static constexpr variable here because binding non-static constexpr to template non-type doesn't work since the address of the variable could potentially be run-time dependant.
	// static makes the variable be located in some global memory, which (as far as the binary file is concerned) has constant addresses.
	#define meta_sprintf_test(buffer, blueprint, ...) static constexpr auto program = meta::create_printf_program(blueprint); meta::execute_printf_program<program, 0>(buffer __VA_OPT__(,) __VA_ARGS__);

}
