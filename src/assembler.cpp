#include "assembler.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "isa.h"
#include "memory.h"

namespace cpusim {
namespace {

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return s;
}

std::vector<std::string> splitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  const std::string last = trim(cur);
  if (!last.empty() || !out.empty()) out.push_back(last);
  return out;
}

std::string toHex(uint32_t v) {
  std::ostringstream os;
  os << std::hex << v;
  return os.str();
}

[[noreturn]] void fail(int line, const std::string& msg) {
  throw AssembleError("line " + std::to_string(line) + ": " + msg);
}

bool parseRegister(const std::string& tokenRaw, uint8_t* out) {
  const std::string token = lower(trim(tokenRaw));
  if (token == "zero") { *out = 0; return true; }
  if (token == "sp") { *out = REG_SP; return true; }
  if (token == "ra") { *out = REG_RA; return true; }
  if (token.size() < 2 || token[0] != 'r') return false;
  for (size_t i = 1; i < token.size(); ++i) {
    if (!isdigit(static_cast<unsigned char>(token[i]))) return false;
  }
  const long n = std::strtol(token.c_str() + 1, nullptr, 10);
  if (n < 0 || n >= NUM_REGS) return false;
  *out = static_cast<uint8_t>(n);
  return true;
}

bool parseNumber(const std::string& token, int64_t* out) {
  if (token.empty()) return false;
  size_t idx = 0;
  bool neg = false;
  if (token[0] == '-' || token[0] == '+') {
    neg = token[0] == '-';
    idx = 1;
  }
  if (idx >= token.size()) return false;
  int base = 10;
  std::string digits = token.substr(idx);
  if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    base = 16;
    digits = digits.substr(2);
  }
  if (digits.empty()) return false;
  char* end = nullptr;
  const long long v = std::strtoll(digits.c_str(), &end, base);
  if (end == nullptr || *end != '\0') return false;
  *out = neg ? -v : v;
  return true;
}

// Records collected during the first pass.
struct TextItem {
  int line = 0;
  Op op = Op::NOP;
  std::string operands;
  uint32_t index = 0;   // instruction slot
};

struct DataItem {
  int line = 0;
  bool isSpace = false;
  uint32_t offset = 0;
  uint32_t bytes = 0;
  std::vector<std::string> values;
};

class Assembler {
 public:
  Assembler(std::string source, std::string name)
      : source_(std::move(source)), name_(std::move(name)) {}

  Program run() {
    collect();
    layout();
    Program prog;
    prog.name = name_;
    prog.textBase = TEXT_BASE;
    prog.dataBase = dataBase_;
    prog.labels = labels_;
    prog.instructionCount = static_cast<uint32_t>(text_.size());
    emitText(&prog);
    emitData(&prog);
    auto mainIt = labels_.find("main");
    prog.entry = mainIt != labels_.end() ? mainIt->second : TEXT_BASE;
    return prog;
  }

 private:
  // ---- pass 1 -------------------------------------------------------------
  void collect() {
    std::istringstream in(source_);
    std::string raw;
    int lineNo = 0;
    bool inData = false;
    // 64-bit so a huge .space cannot wrap the offset back into range.
    uint64_t dataOffset = 0;

    while (std::getline(in, raw)) {
      ++lineNo;
      // Strip comments.
      const size_t hash = raw.find_first_of("#;");
      std::string line = trim(hash == std::string::npos ? raw : raw.substr(0, hash));
      if (line.empty()) continue;

      // Leading labels (possibly several separated by whitespace).
      for (;;) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) break;
        const std::string label = lower(trim(line.substr(0, colon)));
        if (label.empty() || label.find_first_of(" \t") != std::string::npos) break;
        if (pendingLabels_.count(label) || labelSlot_.count(label)) {
          fail(lineNo, "duplicate label '" + label + "'");
        }
        if (inData) {
          dataLabels_[label] = static_cast<uint32_t>(dataOffset);
        } else {
          labelSlot_[label] = static_cast<uint32_t>(text_.size());
        }
        pendingLabels_.insert(label);
        line = trim(line.substr(colon + 1));
        if (line.empty()) break;
      }
      if (line.empty()) continue;

      std::istringstream ls(line);
      std::string head;
      ls >> head;
      std::string rest;
      std::getline(ls, rest);
      rest = trim(rest);
      const std::string mnem = lower(head);

      if (mnem == ".text") { inData = false; continue; }
      if (mnem == ".data") { inData = true; continue; }

      if (inData) {
        DataItem item;
        item.line = lineNo;
        item.offset = static_cast<uint32_t>(dataOffset);
        if (mnem == ".word") {
          item.values = splitCommas(rest);
          if (item.values.empty()) fail(lineNo, ".word needs at least one value");
          item.bytes = static_cast<uint32_t>(item.values.size()) * 4;
        } else if (mnem == ".space") {
          int64_t n = 0;
          if (!parseNumber(trim(rest), &n) || n <= 0) fail(lineNo, "bad .space size");
          if (n > IMAGE_LIMIT) fail(lineNo, ".space size exceeds the address space");
          item.isSpace = true;
          item.bytes = alignUp(static_cast<uint32_t>(n), 4);
        } else {
          fail(lineNo, "unknown data directive '" + mnem + "'");
        }
        dataOffset += item.bytes;
        if (dataOffset > IMAGE_LIMIT) fail(lineNo, "data section exceeds the address space");
        data_.push_back(item);
        continue;
      }

      TextItem item;
      item.line = lineNo;
      item.index = static_cast<uint32_t>(text_.size());
      item.operands = rest;
      if (!expandPseudo(mnem, &item, lineNo) && !parseMnemonic(mnem, item.op)) {
        fail(lineNo, "unknown instruction '" + mnem + "'");
      }
      text_.push_back(item);
    }
    dataSize_ = static_cast<uint32_t>(dataOffset);
  }

  // Rewrites convenience mnemonics into real ones. Every pseudo maps to a
  // single instruction so the text layout stays a simple index * 4.
  bool expandPseudo(const std::string& mnem, TextItem* item, int lineNo) {
    if (mnem == "mv") {
      const auto ops = splitCommas(item->operands);
      if (ops.size() != 2) fail(lineNo, "mv needs 2 operands");
      item->op = Op::ADD;
      item->operands = ops[0] + ", " + ops[1] + ", zero";
      return true;
    }
    if (mnem == "la") {
      const auto ops = splitCommas(item->operands);
      if (ops.size() != 2) fail(lineNo, "la needs 2 operands");
      item->op = Op::LI;
      item->operands = ops[0] + ", " + ops[1];
      return true;
    }
    if (mnem == "j") {
      item->op = Op::JMP;
      return true;
    }
    if (mnem == "ret") {
      item->op = Op::JR;
      item->operands = "ra";
      return true;
    }
    return false;
  }

  void layout() {
    const uint64_t textBytes = static_cast<uint64_t>(text_.size()) * 4;
    const uint64_t dataBase = pageSpan(TEXT_BASE + std::max<uint64_t>(textBytes, 1));
    if (!imageFits(TEXT_BASE, textBytes, dataBase, dataSize_)) {
      throw AssembleError("program too large: text, data and heap must end below 0x" +
                          toHex(IMAGE_LIMIT) + ", where the stack begins");
    }
    textSize_ = static_cast<uint32_t>(textBytes);
    dataBase_ = alignUp(TEXT_BASE + textSize_, PAGE_SIZE);
    if (dataBase_ == TEXT_BASE) dataBase_ = TEXT_BASE + PAGE_SIZE;
    for (const auto& kv : labelSlot_) labels_[kv.first] = TEXT_BASE + kv.second * 4;
    for (const auto& kv : dataLabels_) labels_[kv.first] = dataBase_ + kv.second;
  }

  // ---- operand resolution -------------------------------------------------
  int64_t resolve(const std::string& tokenRaw, int line) {
    const std::string token = trim(tokenRaw);
    int64_t value = 0;
    if (parseNumber(token, &value)) return value;

    // label, label+N, label-N
    size_t split = std::string::npos;
    for (size_t i = 1; i < token.size(); ++i) {
      if (token[i] == '+' || token[i] == '-') { split = i; break; }
    }
    const std::string name = lower(trim(split == std::string::npos ? token : token.substr(0, split)));
    auto it = labels_.find(name);
    if (it == labels_.end()) fail(line, "unknown label or immediate '" + token + "'");
    int64_t base = it->second;
    if (split != std::string::npos) {
      int64_t delta = 0;
      if (!parseNumber(token.substr(split), &delta)) {
        fail(line, "bad label offset in '" + token + "'");
      }
      base += delta;
    }
    return base;
  }

  uint8_t reg(const std::string& token, int line) {
    uint8_t r = 0;
    if (!parseRegister(token, &r)) fail(line, "expected register, got '" + trim(token) + "'");
    return r;
  }

  // Parses `imm(rs1)`, `(rs1)`, `label`, or `label(rs1)`.
  void memOperand(const std::string& token, int line, uint8_t* rs1, int64_t* imm) {
    const size_t open = token.find('(');
    if (open == std::string::npos) {
      *rs1 = 0;
      *imm = resolve(token, line);
      return;
    }
    const size_t close = token.find(')', open);
    if (close == std::string::npos) fail(line, "unbalanced parentheses in '" + token + "'");
    const std::string offsetPart = trim(token.substr(0, open));
    *rs1 = reg(token.substr(open + 1, close - open - 1), line);
    *imm = offsetPart.empty() ? 0 : resolve(offsetPart, line);
  }

  static void checkImm18(int64_t v, int line) {
    if (v < -131072 || v > 131071) {
      fail(line, "immediate " + std::to_string(v) + " does not fit in 18 bits");
    }
  }

  // ---- pass 2 -------------------------------------------------------------
  void emitText(Program* prog) {
    prog->text.reserve(text_.size() * 4);
    for (const TextItem& item : text_) {
      Instr in;
      in.op = item.op;
      in.pc = TEXT_BASE + item.index * 4;
      const auto ops = splitCommas(item.operands);
      const int line = item.line;
      const size_t n = item.operands.empty() ? 0 : ops.size();

      switch (formatOf(in.op)) {
        case Fmt::R: {
          if (n != 3) fail(line, std::string(mnemonic(in.op)) + " needs 3 operands");
          in.rd = reg(ops[0], line);
          in.rs1 = reg(ops[1], line);
          in.rs2 = reg(ops[2], line);
          break;
        }
        case Fmt::I: {
          if (in.op == Op::LI || in.op == Op::LUI) {
            if (n != 2) fail(line, std::string(mnemonic(in.op)) + " needs 2 operands");
            in.rd = reg(ops[0], line);
            const int64_t v = resolve(ops[1], line);
            checkImm18(v, line);
            in.imm = static_cast<int32_t>(v);
          } else if (in.op == Op::JR || in.op == Op::PRINT) {
            if (n != 1) fail(line, std::string(mnemonic(in.op)) + " needs 1 operand");
            in.rs1 = reg(ops[0], line);
          } else {
            if (n != 3) fail(line, std::string(mnemonic(in.op)) + " needs 3 operands");
            in.rd = reg(ops[0], line);
            in.rs1 = reg(ops[1], line);
            const int64_t v = resolve(ops[2], line);
            checkImm18(v, line);
            in.imm = static_cast<int32_t>(v);
          }
          break;
        }
        case Fmt::Mem: {
          if (n != 2) fail(line, std::string(mnemonic(in.op)) + " needs 2 operands");
          uint8_t base = 0;
          int64_t off = 0;
          memOperand(ops[1], line, &base, &off);
          checkImm18(off, line);
          in.rs1 = base;
          in.imm = static_cast<int32_t>(off);
          if (in.op == Op::LW) {
            in.rd = reg(ops[0], line);
          } else {
            in.rs2 = reg(ops[0], line);
          }
          break;
        }
        case Fmt::Branch: {
          if (n != 3) fail(line, std::string(mnemonic(in.op)) + " needs 3 operands");
          in.rs1 = reg(ops[0], line);
          in.rs2 = reg(ops[1], line);
          const int64_t target = resolve(ops[2], line);
          const int64_t delta = target - static_cast<int64_t>(in.pc + 4);
          checkImm18(delta, line);
          in.imm = static_cast<int32_t>(delta);
          break;
        }
        case Fmt::Jump: {
          if (n != 1) fail(line, std::string(mnemonic(in.op)) + " needs 1 operand");
          const int64_t target = resolve(ops[0], line);
          if (target < 0 || target > 0x03FFFFFF) fail(line, "jump target out of range");
          in.imm = static_cast<int32_t>(target);
          if (in.op == Op::JAL) in.rd = REG_RA;
          break;
        }
        case Fmt::None:
          if (n != 0) fail(line, std::string(mnemonic(in.op)) + " takes no operands");
          break;
      }

      const uint32_t raw = encode(in);
      prog->text.push_back(static_cast<uint8_t>(raw & 0xFF));
      prog->text.push_back(static_cast<uint8_t>((raw >> 8) & 0xFF));
      prog->text.push_back(static_cast<uint8_t>((raw >> 16) & 0xFF));
      prog->text.push_back(static_cast<uint8_t>((raw >> 24) & 0xFF));
    }
  }

  void emitData(Program* prog) {
    prog->data.assign(dataSize_, 0);
    for (const DataItem& item : data_) {
      if (item.isSpace) continue;
      uint32_t off = item.offset;
      for (const std::string& token : item.values) {
        const int64_t v = resolve(token, item.line);
        const uint32_t word = static_cast<uint32_t>(static_cast<int32_t>(v));
        prog->data[off + 0] = static_cast<uint8_t>(word & 0xFF);
        prog->data[off + 1] = static_cast<uint8_t>((word >> 8) & 0xFF);
        prog->data[off + 2] = static_cast<uint8_t>((word >> 16) & 0xFF);
        prog->data[off + 3] = static_cast<uint8_t>((word >> 24) & 0xFF);
        off += 4;
      }
    }
  }

  std::string source_;
  std::string name_;
  std::vector<TextItem> text_;
  std::vector<DataItem> data_;
  std::map<std::string, uint32_t> labelSlot_;
  std::map<std::string, uint32_t> dataLabels_;
  std::map<std::string, uint32_t> labels_;
  std::set<std::string> pendingLabels_;
  uint32_t textSize_ = 0;
  uint32_t dataSize_ = 0;
  uint32_t dataBase_ = 0;
};

}  // namespace

Program assemble(const std::string& source, const std::string& name) {
  Assembler as(source, name);
  return as.run();
}

Program assembleFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw AssembleError("cannot open '" + path + "'");
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string base = path;
  const size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  return assemble(ss.str(), base);
}

}  // namespace cpusim
