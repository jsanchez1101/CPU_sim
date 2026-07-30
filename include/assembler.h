// assembler.h -- two-pass assembler for the toy ISA.
//
// Supported syntax:
//
//   # comment            ; comment
//   .text                start of the instruction section (default)
//   .data                start of the data section
//   label:               may stand alone or precede an instruction
//   .word 1, 2, -3       emit 32-bit words
//   .space 64            emit N zero bytes
//
// Pseudo-instructions: nop, mv rd, rs / la rd, label / j label / ret
// Immediates accept decimal, hex (0x...), character-free labels, and the
// forms `label+8` / `label-4`.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpusim {

struct AssembleError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Program {
  std::string name;
  uint32_t textBase = 0;
  uint32_t dataBase = 0;
  uint32_t entry = 0;
  std::vector<uint8_t> text;
  std::vector<uint8_t> data;
  std::map<std::string, uint32_t> labels;
  uint32_t instructionCount = 0;
};

Program assemble(const std::string& source, const std::string& name);
Program assembleFile(const std::string& path);

}  // namespace cpusim
