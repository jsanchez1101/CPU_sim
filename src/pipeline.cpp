#include "pipeline.h"

#include <cstdio>
#include <string>

namespace cpusim {
namespace {

std::string stageText(bool valid, const Instr& in) {
  if (!valid) return "--";
  return disassemble(in);
}

}  // namespace

Core::Core(MemorySystem& mem, const CoreConfig& cfg) : mem_(mem), cfg_(cfg) {
  bht_.assign(static_cast<size_t>(1) << cfg_.bhtBits, 1);
}

void Core::attach(Process* p) {
  proc_ = p;
  pc_ = p->pc;
  cur_ = Latches{};
  nxt_ = Latches{};
  fetchOn_ = true;
  stallIF_ = false;
  flush_ = false;
  redirect_ = 0;
  ifStall_ = 0;
  memStall_ = 0;
  exit_ = Exit::Running;
  p->state = ProcState::Running;
  ++p->timesScheduled;
  p->quantumUsed = 0;
}

Process* Core::detach() {
  Process* p = proc_;
  if (p != nullptr) {
    p->pc = pc_;
    if (p->state == ProcState::Running) p->state = ProcState::Ready;
  }
  proc_ = nullptr;
  return p;
}

void Core::requestDrain(Exit reason) {
  fetchOn_ = false;
  if (exit_ == Exit::Running || reason == Exit::Fault || reason == Exit::Halt) {
    exit_ = reason;
  }
}

bool Core::drained() const {
  return !cur_.ifid.valid && !cur_.idex.valid && !cur_.exmem.valid &&
         !cur_.memwb.valid && ifStall_ == 0 && memStall_ == 0;
}

uint32_t Core::predictorIndex(uint32_t pc) const {
  return (pc >> 2) & ((1u << cfg_.bhtBits) - 1u);
}

void Core::raiseFault(Fault f, uint32_t addr, const char* where) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s at 0x%08x during %s", faultName(f), addr,
                where);
  proc_->faultMessage = buf;
  fetchOn_ = false;
  exit_ = Exit::Fault;
  cur_ = Latches{};
  nxt_ = Latches{};
  ifStall_ = 0;
  memStall_ = 0;
}

void Core::cycle(uint64_t globalCycle) {
  if (proc_ == nullptr) return;
  Stats& st = proc_->stats;
  ++st.cycles;
  ++proc_->cyclesOnCpu;
  ++proc_->quantumUsed;

  if (memStall_ > 0) {
    --memStall_;
    ++st.stallMemory;
    lastNote_ = "data memory stall";
    if (cfg_.trace) traceCycle(globalCycle);
    return;
  }

  stallIF_ = false;
  flushedThisCycle_ = false;
  writebackStage();
  if (exit_ == Exit::Fault) return;
  memoryStage();
  if (exit_ == Exit::Fault) return;
  executeStage();
  decodeStage();
  if (exit_ == Exit::Fault) return;
  fetchStage();
  if (exit_ == Exit::Fault) return;

  if (!nxt_.idex.valid) ++st.bubbles;
  lastNote_ = computeNote();
  if (cfg_.trace) traceCycle(globalCycle);

  cur_ = nxt_;
  nxt_ = Latches{};
}

const char* Core::computeNote() const {
  if (memStall_ > 0) return "data memory stall";
  if (stallIF_) return "load-use stall, IF/ID held";
  if (ifStall_ > 0) return "instruction fetch stall";
  if (flushedThisCycle_) return "branch mispredict, redirecting fetch";
  if (!fetchOn_) return "draining before switch";
  return "";
}

CoreSnapshot Core::snapshot() const {
  CoreSnapshot s;
  s.ifid = {cur_.ifid.valid, cur_.ifid.pc};
  s.idex = {cur_.idex.valid, cur_.idex.in.pc};
  s.exmem = {cur_.exmem.valid, cur_.exmem.in.pc};
  s.memwb = {cur_.memwb.valid, cur_.memwb.in.pc};
  s.nextPc = pc_;
  if (cur_.exmem.valid &&
      (isLoad(cur_.exmem.in.op) || isStore(cur_.exmem.in.op))) {
    s.hasMemAccess = true;
    s.memAddr = cur_.exmem.alu;
    s.memIsStore = isStore(cur_.exmem.in.op);
  }
  return s;
}

// --- WB --------------------------------------------------------------------

void Core::writebackStage() {
  if (!cur_.memwb.valid) return;
  const Instr& in = cur_.memwb.in;
  if (writesRegister(in)) proc_->regs[in.rd] = cur_.memwb.val;
  proc_->regs[0] = 0;
  if (in.op == Op::PRINT) {
    proc_->output.push_back(static_cast<int32_t>(cur_.memwb.val));
  }
  ++proc_->stats.instructions;
}

// --- MEM -------------------------------------------------------------------

void Core::memoryStage() {
  if (!cur_.exmem.valid) return;
  const EXMEM& s = cur_.exmem;
  MEMWB out;
  out.valid = true;
  out.in = s.in;
  out.val = s.alu;

  if (isLoad(s.in.op)) {
    uint32_t value = 0;
    const AccessResult r = mem_.read32(*proc_, s.alu, &value);
    if (!r.ok()) {
      raiseFault(r.fault, s.alu, "load");
      return;
    }
    out.val = value;
    memStall_ = r.extraCycles;
    ++proc_->stats.loads;
  } else if (isStore(s.in.op)) {
    const AccessResult r = mem_.write32(*proc_, s.alu, s.storeVal);
    if (!r.ok()) {
      raiseFault(r.fault, s.alu, "store");
      return;
    }
    memStall_ = r.extraCycles;
    ++proc_->stats.stores;
  }
  nxt_.memwb = out;
}

// --- EX --------------------------------------------------------------------

bool Core::forwardOperand(uint8_t reg, uint32_t* value) {
  if (reg == 0) return false;
  if (cur_.exmem.valid && writesRegister(cur_.exmem.in) &&
      cur_.exmem.in.rd == reg && !isLoad(cur_.exmem.in.op)) {
    *value = cur_.exmem.alu;
    return true;
  }
  if (cur_.memwb.valid && writesRegister(cur_.memwb.in) &&
      cur_.memwb.in.rd == reg) {
    *value = cur_.memwb.val;
    return true;
  }
  return false;
}

void Core::executeStage() {
  if (!cur_.idex.valid) return;
  const IDEX& s = cur_.idex;
  const Instr& in = s.in;
  Stats& st = proc_->stats;

  uint32_t a = s.rs1v;
  uint32_t b = s.rs2v;
  if (cfg_.forwarding) {
    uint32_t fwd = 0;
    if (readsRs1(in) && forwardOperand(in.rs1, &fwd)) {
      a = fwd;
      ++st.forwards;
    }
    if (readsRs2(in) && forwardOperand(in.rs2, &fwd)) {
      b = fwd;
      ++st.forwards;
    }
  }

  EXMEM out;
  out.valid = true;
  out.in = in;
  out.storeVal = b;

  const int32_t sa = static_cast<int32_t>(a);
  const int32_t sb = static_cast<int32_t>(b);
  const uint32_t ui = static_cast<uint32_t>(in.imm);

  switch (in.op) {
    case Op::ADD:  out.alu = a + b; break;
    case Op::SUB:  out.alu = a - b; break;
    case Op::MUL:  out.alu = static_cast<uint32_t>(sa * sb); break;
    case Op::AND:  out.alu = a & b; break;
    case Op::OR:   out.alu = a | b; break;
    case Op::XOR:  out.alu = a ^ b; break;
    case Op::SLL:  out.alu = a << (b & 31u); break;
    case Op::SRL:  out.alu = a >> (b & 31u); break;
    case Op::SLT:  out.alu = (sa < sb) ? 1u : 0u; break;
    case Op::ADDI: out.alu = a + ui; break;
    case Op::ANDI: out.alu = a & ui; break;
    case Op::ORI:  out.alu = a | ui; break;
    case Op::XORI: out.alu = a ^ ui; break;
    case Op::SLLI: out.alu = a << (ui & 31u); break;
    case Op::SRLI: out.alu = a >> (ui & 31u); break;
    case Op::SLTI: out.alu = (sa < in.imm) ? 1u : 0u; break;
    case Op::LI:   out.alu = ui; break;
    case Op::LUI:  out.alu = ui << 14; break;
    case Op::LW:
    case Op::SW:   out.alu = a + ui; break;
    case Op::PRINT: out.alu = a; break;
    case Op::JAL:  out.alu = in.pc + 4; break;
    default:       out.alu = 0; break;
  }

  if (isControl(in.op)) {
    bool taken = false;
    uint32_t target = in.pc + 4;
    switch (in.op) {
      case Op::BEQ: taken = (a == b); target = in.pc + 4 + in.imm; break;
      case Op::BNE: taken = (a != b); target = in.pc + 4 + in.imm; break;
      case Op::BLT: taken = (sa < sb); target = in.pc + 4 + in.imm; break;
      case Op::BGE: taken = (sa >= sb); target = in.pc + 4 + in.imm; break;
      case Op::JMP:
      case Op::JAL: taken = true; target = static_cast<uint32_t>(in.imm); break;
      case Op::JR:  taken = true; target = a; break;
      default: break;
    }

    ++st.branches;
    const uint32_t predNext = s.predTaken ? s.predTarget : in.pc + 4;
    const uint32_t actualNext = taken ? target : in.pc + 4;

    if (cfg_.predictor) {
      const uint32_t idx = predictorIndex(in.pc);
      if (isJump(in.op)) {
        bht_[idx] = 3;
      } else if (taken) {
        if (bht_[idx] < 3) ++bht_[idx];
      } else {
        if (bht_[idx] > 0) --bht_[idx];
      }
      if (taken) btb_[in.pc] = target;
    }

    if (predNext != actualNext) {
      ++st.mispredicts;
      st.flushCycles += 2;
      flush_ = true;
      flushedThisCycle_ = true;
      redirect_ = actualNext;
    }
  } else if (s.predTaken) {
    // The BTB is indexed by PC alone and is shared by every process, so a
    // tag-less hit can land on an instruction that is not a branch at all
    // (two processes both start their text at TEXT_BASE). Without this check
    // the bogus redirect would never be corrected and instructions would be
    // skipped outright, so treat it as a mispredict and fall through to pc+4.
    ++st.mispredicts;
    st.flushCycles += 2;
    flush_ = true;
    flushedThisCycle_ = true;
    redirect_ = in.pc + 4;
    btb_.erase(in.pc);
  }

  nxt_.exmem = out;
}

// --- ID --------------------------------------------------------------------

void Core::decodeStage() {
  if (flush_) return;  // squash whatever is sitting in IF/ID
  if (!cur_.ifid.valid) return;

  const Instr in = decode(cur_.ifid.raw, cur_.ifid.pc);
  if (in.op == Op::INVALID) {
    raiseFault(Fault::Segmentation, cur_.ifid.pc, "decode of illegal opcode");
    return;
  }

  Stats& st = proc_->stats;
  bool stall = false;

  auto conflicts = [&in](const Instr& producer) {
    if (producer.rd == 0) return false;
    if (readsRs1(in) && in.rs1 == producer.rd) return true;
    if (readsRs2(in) && in.rs2 == producer.rd) return true;
    return false;
  };

  if (cfg_.forwarding) {
    // Only a load cannot be forwarded in time; everything else is covered by
    // the EX/MEM and MEM/WB bypass paths.
    if (cur_.idex.valid && isLoad(cur_.idex.in.op) && conflicts(cur_.idex.in)) {
      stall = true;
      ++st.stallLoadUse;
    }
  } else {
    if ((cur_.idex.valid && writesRegister(cur_.idex.in) && conflicts(cur_.idex.in)) ||
        (cur_.exmem.valid && writesRegister(cur_.exmem.in) && conflicts(cur_.exmem.in))) {
      stall = true;
      ++st.stallDataHazard;
    }
  }

  if (stall) {
    nxt_.ifid = cur_.ifid;  // hold the instruction in IF/ID
    stallIF_ = true;
    return;
  }

  if (in.op == Op::HALT) requestDrain(Exit::Halt);
  if (in.op == Op::YIELD) requestDrain(Exit::Yield);

  IDEX out;
  out.valid = true;
  out.in = in;
  out.rs1v = proc_->regs[in.rs1];
  out.rs2v = proc_->regs[in.rs2];
  out.predTaken = cur_.ifid.predTaken;
  out.predTarget = cur_.ifid.predTarget;
  nxt_.idex = out;
}

// --- IF --------------------------------------------------------------------

void Core::fetchStage() {
  if (stallIF_) return;  // ID is holding IF/ID, do not overwrite it
  if (flush_) {
    pc_ = redirect_;
    flush_ = false;
    ifStall_ = 0;
    return;
  }
  if (ifStall_ > 0) {
    --ifStall_;
    ++proc_->stats.stallFetch;
    return;
  }
  if (!fetchOn_) return;

  uint32_t raw = 0;
  const AccessResult r = mem_.fetch32(*proc_, pc_, &raw);
  if (!r.ok()) {
    raiseFault(r.fault, pc_, "instruction fetch");
    return;
  }
  ifStall_ = r.extraCycles;

  IFID out;
  out.valid = true;
  out.pc = pc_;
  out.raw = raw;

  uint32_t next = pc_ + 4;
  if (cfg_.predictor) {
    auto it = btb_.find(pc_);
    if (it != btb_.end() && bht_[predictorIndex(pc_)] >= 2) {
      out.predTaken = true;
      out.predTarget = it->second;
      next = it->second;
    }
  }
  pc_ = next;
  nxt_.ifid = out;
}

// --- trace -----------------------------------------------------------------

void Core::traceCycle(uint64_t globalCycle) {
  if (!traceHeader_) {
    traceHeader_ = true;
    std::printf("%8s  %-10s  %-22s %-22s %-22s %-22s  %s\n", "cycle", "process",
                "IF/ID", "ID/EX", "EX/MEM", "MEM/WB", "note");
  }
  Instr ifInstr{};
  if (cur_.ifid.valid) ifInstr = decode(cur_.ifid.raw, cur_.ifid.pc);
  const char* note = lastNote_.c_str();

  std::printf("%8llu  %-10s  %-22s %-22s %-22s %-22s  %s\n",
              static_cast<unsigned long long>(globalCycle),
              proc_ ? proc_->name.c_str() : "-",
              stageText(cur_.ifid.valid, ifInstr).c_str(),
              stageText(cur_.idex.valid, cur_.idex.in).c_str(),
              stageText(cur_.exmem.valid, cur_.exmem.in).c_str(),
              stageText(cur_.memwb.valid, cur_.memwb.in).c_str(), note);
}

}  // namespace cpusim
