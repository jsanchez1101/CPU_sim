// pipeline.h -- the in-order 5-stage pipeline.
//
//   IF  fetch through the MMU and the L1 instruction cache
//   ID  decode, read the register file, detect load-use hazards
//   EX  ALU, branch resolution, operand forwarding
//   MEM data cache / MMU access
//   WB  register file write-back and retirement
//
// Latches are double buffered: every stage reads `cur_` and writes `nxt_`, and
// the two are swapped at the end of the cycle. Stage functions are still
// invoked back-to-front so that a register written in WB is visible to a read
// in ID in the same cycle, which is the classic "write first half, read second
// half" register file behaviour that makes distance-3 hazards free.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "isa.h"
#include "memory.h"
#include "process.h"

namespace cpusim {

struct CoreConfig {
  bool forwarding = true;
  bool predictor = true;    // false == static predict-not-taken
  uint32_t bhtBits = 6;
  bool trace = false;
};

// Reason the core stopped running the attached process.
enum class Exit { Running, Halt, Yield, Preempt, Fault };

struct IFID {
  bool valid = false;
  uint32_t pc = 0;
  uint32_t raw = 0;
  bool predTaken = false;
  uint32_t predTarget = 0;
};

struct IDEX {
  bool valid = false;
  Instr in;
  uint32_t rs1v = 0;
  uint32_t rs2v = 0;
  bool predTaken = false;
  uint32_t predTarget = 0;
};

struct EXMEM {
  bool valid = false;
  Instr in;
  uint32_t alu = 0;
  uint32_t storeVal = 0;
};

struct MEMWB {
  bool valid = false;
  Instr in;
  uint32_t val = 0;
};

class Core {
 public:
  Core(MemorySystem& mem, const CoreConfig& cfg);

  void attach(Process* p);
  Process* detach();
  Process* current() const { return proc_; }

  void cycle(uint64_t globalCycle);

  // Ask the core to stop fetching and let in-flight work finish.
  void requestDrain(Exit reason);

  bool drained() const;
  Exit exitReason() const { return exit_; }

 private:
  void writebackStage();
  void memoryStage();
  void executeStage();
  void decodeStage();
  void fetchStage();

  bool forwardOperand(uint8_t reg, uint32_t* value);
  uint32_t predictorIndex(uint32_t pc) const;
  void raiseFault(Fault f, uint32_t addr, const char* where);
  void traceCycle(uint64_t globalCycle);

  MemorySystem& mem_;
  CoreConfig cfg_;
  Process* proc_ = nullptr;

  struct Latches {
    IFID ifid;
    IDEX idex;
    EXMEM exmem;
    MEMWB memwb;
  };
  Latches cur_;
  Latches nxt_;

  uint32_t pc_ = 0;
  bool fetchOn_ = false;
  bool stallIF_ = false;
  bool flush_ = false;
  uint32_t redirect_ = 0;
  uint32_t ifStall_ = 0;
  uint32_t memStall_ = 0;
  Exit exit_ = Exit::Running;
  bool traceHeader_ = false;
  bool flushedThisCycle_ = false;

  std::vector<uint8_t> bht_;                      // 2-bit saturating counters
  std::unordered_map<uint32_t, uint32_t> btb_;    // pc -> last taken target
};

}  // namespace cpusim
