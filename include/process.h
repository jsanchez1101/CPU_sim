// process.h -- the process control block.
//
// A Process owns everything that has to be saved and restored across a context
// switch: the architectural register file, the program counter, its address
// space (page table plus the regions that define which addresses are legal),
// scheduling bookkeeping and its own copy of the performance counters.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "isa.h"
#include "memory.h"
#include "stats.h"

namespace cpusim {

enum class ProcState { New, Ready, Running, Terminated };

const char* stateName(ProcState s);

struct Region {
  uint32_t base = 0;
  uint32_t size = 0;
  bool writable = true;
  std::string name;

  bool contains(uint32_t va, uint32_t width) const {
    return va >= base && static_cast<uint64_t>(va) + width <= static_cast<uint64_t>(base) + size;
  }
};

struct Process {
  uint32_t pid = 0;
  std::string name;
  ProcState state = ProcState::New;

  std::array<uint32_t, NUM_REGS> regs{};
  uint32_t pc = 0;

  PageTable pageTable;
  std::vector<Region> regions;

  // Scheduling
  int priority = 0;         // lower value == higher priority
  int mlfqLevel = 0;
  uint64_t arrivalCycle = 0;
  uint64_t firstRunCycle = 0;
  uint64_t finishCycle = 0;
  uint64_t cyclesOnCpu = 0;
  uint64_t quantumUsed = 0;
  uint64_t timesScheduled = 0;
  bool started = false;

  Stats stats;
  std::vector<int32_t> output;   // values produced by the print instruction
  std::string faultMessage;

  const Region* regionFor(uint32_t va, uint32_t width) const {
    for (const Region& r : regions) {
      if (r.contains(va, width)) return &r;
    }
    return nullptr;
  }

  void reset() {
    regs.fill(0);
    regs[REG_SP] = STACK_TOP;
    regs[REG_RA] = 0;
  }
};

}  // namespace cpusim
