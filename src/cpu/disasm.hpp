#pragma once
// MIPS III / VR4300 disassembler for traces, tracediff context and debugging.

#include <string_view>

#include "common/types.hpp"

namespace ultra {

// Formats `instr` (at virtual address `pc`) into `buf` and returns a view of it.
std::string_view disassemble(u32 instr, u64 pc, char* buf, usize n);

}  // namespace ultra
