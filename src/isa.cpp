#include "isa.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace cpusim {
namespace {

struct OpInfo {
  Op op;
  const char* name;
  Fmt fmt;
};

// Single source of truth for mnemonics and formats.
const OpInfo kOps[] = {
    {Op::NOP,   "nop",   Fmt::None},
    {Op::ADD,   "add",   Fmt::R},
    {Op::SUB,   "sub",   Fmt::R},
    {Op::MUL,   "mul",   Fmt::R},
    {Op::AND,   "and",   Fmt::R},
    {Op::OR,    "or",    Fmt::R},
    {Op::XOR,   "xor",   Fmt::R},
    {Op::SLL,   "sll",   Fmt::R},
    {Op::SRL,   "srl",   Fmt::R},
    {Op::SLT,   "slt",   Fmt::R},
    {Op::ADDI,  "addi",  Fmt::I},
    {Op::ANDI,  "andi",  Fmt::I},
    {Op::ORI,   "ori",   Fmt::I},
    {Op::XORI,  "xori",  Fmt::I},
    {Op::SLLI,  "slli",  Fmt::I},
    {Op::SRLI,  "srli",  Fmt::I},
    {Op::SLTI,  "slti",  Fmt::I},
    {Op::LI,    "li",    Fmt::I},
    {Op::LUI,   "lui",   Fmt::I},
    {Op::LW,    "lw",    Fmt::Mem},
    {Op::SW,    "sw",    Fmt::Mem},
    {Op::BEQ,   "beq",   Fmt::Branch},
    {Op::BNE,   "bne",   Fmt::Branch},
    {Op::BLT,   "blt",   Fmt::Branch},
    {Op::BGE,   "bge",   Fmt::Branch},
    {Op::JMP,   "jmp",   Fmt::Jump},
    {Op::JAL,   "jal",   Fmt::Jump},
    {Op::JR,    "jr",    Fmt::I},
    {Op::YIELD, "yield", Fmt::None},
    {Op::PRINT, "print", Fmt::I},
    {Op::HALT,  "halt",  Fmt::None},
};

const OpInfo* find(Op op) {
  for (const OpInfo& info : kOps) {
    if (info.op == op) return &info;
  }
  return nullptr;
}

int32_t signExtend18(uint32_t v) {
  v &= 0x3FFFFu;
  return (v & 0x20000u) ? static_cast<int32_t>(v | 0xFFFC0000u)
                        : static_cast<int32_t>(v);
}

}  // namespace

Fmt formatOf(Op op) {
  const OpInfo* info = find(op);
  return info ? info->fmt : Fmt::None;
}

const char* mnemonic(Op op) {
  const OpInfo* info = find(op);
  return info ? info->name : "???";
}

bool parseMnemonic(const std::string& text, Op& out) {
  for (const OpInfo& info : kOps) {
    if (text == info.name) {
      out = info.op;
      return true;
    }
  }
  return false;
}

bool isLoad(Op op) { return op == Op::LW; }
bool isStore(Op op) { return op == Op::SW; }
bool isBranch(Op op) {
  return op == Op::BEQ || op == Op::BNE || op == Op::BLT || op == Op::BGE;
}
bool isJump(Op op) { return op == Op::JMP || op == Op::JAL || op == Op::JR; }
bool isControl(Op op) { return isBranch(op) || isJump(op); }

bool writesRegister(const Instr& in) {
  if (in.rd == 0) return false;
  switch (formatOf(in.op)) {
    case Fmt::R:
      return true;
    case Fmt::I:
      return in.op != Op::JR && in.op != Op::PRINT;
    case Fmt::Mem:
      return in.op == Op::LW;
    case Fmt::Jump:
      return in.op == Op::JAL;
    default:
      return false;
  }
}

bool readsRs1(const Instr& in) {
  switch (in.op) {
    case Op::LI:
    case Op::LUI:
    case Op::NOP:
    case Op::HALT:
    case Op::YIELD:
    case Op::JMP:
    case Op::JAL:
      return false;
    default:
      return formatOf(in.op) != Fmt::None;
  }
}

bool readsRs2(const Instr& in) {
  if (formatOf(in.op) == Fmt::R) return true;
  if (isBranch(in.op)) return true;
  if (in.op == Op::SW) return true;
  return false;
}

uint32_t encode(const Instr& in) {
  const uint32_t op = static_cast<uint32_t>(in.op) & 0x3Fu;
  uint32_t raw = op << 26;
  switch (formatOf(in.op)) {
    case Fmt::R:
      raw |= (in.rd & 0xFu) << 22;
      raw |= (in.rs1 & 0xFu) << 18;
      raw |= (in.rs2 & 0xFu) << 14;
      break;
    case Fmt::I:
      raw |= (in.rd & 0xFu) << 22;
      raw |= (in.rs1 & 0xFu) << 18;
      raw |= static_cast<uint32_t>(in.imm) & 0x3FFFFu;
      break;
    case Fmt::Mem:
      // LW uses rd; SW parks its source register in the rd field.
      raw |= ((in.op == Op::LW ? in.rd : in.rs2) & 0xFu) << 22;
      raw |= (in.rs1 & 0xFu) << 18;
      raw |= static_cast<uint32_t>(in.imm) & 0x3FFFFu;
      break;
    case Fmt::Branch:
      raw |= (in.rs2 & 0xFu) << 22;
      raw |= (in.rs1 & 0xFu) << 18;
      raw |= static_cast<uint32_t>(in.imm) & 0x3FFFFu;
      break;
    case Fmt::Jump:
      if (in.op == Op::JAL) {
        // Return register is fixed at ra so the whole 26-bit target survives.
        raw |= static_cast<uint32_t>(in.imm) & 0x03FFFFFFu;
      } else {
        raw |= static_cast<uint32_t>(in.imm) & 0x03FFFFFFu;
      }
      break;
    case Fmt::None:
      break;
  }
  return raw;
}

Instr decode(uint32_t raw, uint32_t pc) {
  Instr in;
  in.raw = raw;
  in.pc = pc;
  const uint8_t opcode = static_cast<uint8_t>((raw >> 26) & 0x3Fu);
  const OpInfo* info = nullptr;
  for (const OpInfo& candidate : kOps) {
    if (static_cast<uint8_t>(candidate.op) == opcode) {
      info = &candidate;
      break;
    }
  }
  if (info == nullptr) {
    in.op = Op::INVALID;
    return in;
  }
  in.op = info->op;
  const uint8_t a = static_cast<uint8_t>((raw >> 22) & 0xFu);
  const uint8_t b = static_cast<uint8_t>((raw >> 18) & 0xFu);
  switch (info->fmt) {
    case Fmt::R:
      in.rd = a;
      in.rs1 = b;
      in.rs2 = static_cast<uint8_t>((raw >> 14) & 0xFu);
      break;
    case Fmt::I:
      in.rd = a;
      in.rs1 = b;
      in.imm = signExtend18(raw);
      break;
    case Fmt::Mem:
      if (in.op == Op::LW) {
        in.rd = a;
      } else {
        in.rs2 = a;
      }
      in.rs1 = b;
      in.imm = signExtend18(raw);
      break;
    case Fmt::Branch:
      in.rs2 = a;
      in.rs1 = b;
      in.imm = signExtend18(raw);
      break;
    case Fmt::Jump:
      in.imm = static_cast<int32_t>(raw & 0x03FFFFFFu);
      if (in.op == Op::JAL) in.rd = REG_RA;
      break;
    case Fmt::None:
      break;
  }
  return in;
}

std::string regName(unsigned r) {
  if (r == 0) return "zero";
  if (r == REG_SP) return "sp";
  if (r == REG_RA) return "ra";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "r%u", r & 0xFFu);
  return buf;
}

std::string disassemble(const Instr& in) {
  char buf[96];
  const char* m = mnemonic(in.op);
  switch (formatOf(in.op)) {
    case Fmt::R:
      std::snprintf(buf, sizeof(buf), "%-5s %s, %s, %s", m,
                    regName(in.rd).c_str(), regName(in.rs1).c_str(),
                    regName(in.rs2).c_str());
      break;
    case Fmt::I:
      if (in.op == Op::LI || in.op == Op::LUI) {
        std::snprintf(buf, sizeof(buf), "%-5s %s, %d", m,
                      regName(in.rd).c_str(), in.imm);
      } else if (in.op == Op::JR || in.op == Op::PRINT) {
        std::snprintf(buf, sizeof(buf), "%-5s %s", m, regName(in.rs1).c_str());
      } else {
        std::snprintf(buf, sizeof(buf), "%-5s %s, %s, %d", m,
                      regName(in.rd).c_str(), regName(in.rs1).c_str(), in.imm);
      }
      break;
    case Fmt::Mem:
      std::snprintf(buf, sizeof(buf), "%-5s %s, %d(%s)", m,
                    regName(in.op == Op::LW ? in.rd : in.rs2).c_str(), in.imm,
                    regName(in.rs1).c_str());
      break;
    case Fmt::Branch:
      std::snprintf(buf, sizeof(buf), "%-5s %s, %s, 0x%x", m,
                    regName(in.rs1).c_str(), regName(in.rs2).c_str(),
                    in.pc + 4 + in.imm);
      break;
    case Fmt::Jump:
      std::snprintf(buf, sizeof(buf), "%-5s 0x%x", m,
                    static_cast<unsigned>(in.imm));
      break;
    case Fmt::None:
      std::snprintf(buf, sizeof(buf), "%s", m);
      break;
  }
  return buf;
}

}  // namespace cpusim
