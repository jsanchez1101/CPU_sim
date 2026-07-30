// isa.h -- instruction set definition, encoding and decoding.
//
// A compact 32-bit fixed-width toy ISA, deliberately shaped like a classic
// RISC load/store machine so that the textbook 5-stage pipeline maps onto it
// cleanly.
//
//   16 general purpose registers, r0 is hardwired to zero.
//   r14 (sp) is initialised to the top of the stack region.
//   r15 (ra) receives the return address on JAL.
//
// Encoding layout (bit 31 is the most significant bit):
//
//   R-type   [31:26] op  [25:22] rd   [21:18] rs1  [17:14] rs2  [13:0] unused
//   I-type   [31:26] op  [25:22] rd   [21:18] rs1  [17:0]  imm18 (signed)
//   MEM      LW  : rd  = mem[rs1 + imm18]
//            SW  : mem[rs1 + imm18] = rs2, where rs2 occupies the rd field
//   BRANCH   [31:26] op  [25:22] rs2  [21:18] rs1  [17:0]  imm18 (signed,
//            a byte displacement relative to pc + 4)
//   J-type   [31:26] op  [25:0]  absolute target address
//
#pragma once

#include <cstdint>
#include <string>

namespace cpusim {

constexpr int NUM_REGS = 16;
constexpr int REG_SP = 14;
constexpr int REG_RA = 15;

enum class Op : uint8_t {
  NOP = 0,
  // register-register ALU
  ADD, SUB, MUL, AND, OR, XOR, SLL, SRL, SLT,
  // register-immediate ALU
  ADDI, ANDI, ORI, XORI, SLLI, SRLI, SLTI, LI, LUI,
  // memory
  LW, SW,
  // control
  BEQ, BNE, BLT, BGE, JMP, JAL, JR,
  // system
  YIELD, PRINT, HALT,
  INVALID = 63,
};

enum class Fmt { R, I, Mem, Branch, Jump, None };

struct Instr {
  Op op = Op::NOP;
  uint8_t rd = 0;
  uint8_t rs1 = 0;
  uint8_t rs2 = 0;
  int32_t imm = 0;
  uint32_t pc = 0;
  uint32_t raw = 0;
};

Fmt formatOf(Op op);
const char* mnemonic(Op op);
bool parseMnemonic(const std::string& text, Op& out);

bool isLoad(Op op);
bool isStore(Op op);
bool isBranch(Op op);         // conditional branch
bool isJump(Op op);           // unconditional transfer
bool isControl(Op op);        // branch or jump
bool writesRegister(const Instr& in);
bool readsRs1(const Instr& in);
bool readsRs2(const Instr& in);

uint32_t encode(const Instr& in);
Instr decode(uint32_t raw, uint32_t pc);
std::string disassemble(const Instr& in);
std::string regName(unsigned r);

}  // namespace cpusim
