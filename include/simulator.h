// simulator.h -- ties the core, the memory hierarchy and the scheduler together.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assembler.h"
#include "memory.h"
#include "pipeline.h"
#include "process.h"
#include "scheduler.h"
#include "stats.h"

namespace cpusim {

struct SimConfig {
  MemConfig mem;
  CoreConfig core;
  SchedPolicy policy = SchedPolicy::RoundRobin;
  uint64_t quantum = 200;
  uint64_t contextSwitchPenalty = 20;
  uint64_t maxCycles = 20'000'000;
};

class Simulator {
 public:
  explicit Simulator(const SimConfig& cfg);

  Process* load(const Program& prog, int priority = 0);
  void run();

  uint64_t cycles() const { return cycles_; }
  bool hitCycleLimit() const { return hitLimit_; }
  const SimConfig& config() const { return cfg_; }
  MemorySystem& memory() { return mem_; }
  const std::vector<std::unique_ptr<Process>>& processes() const { return procs_; }

  // Sum of every process's counters plus the simulator-level ones.
  Stats totals() const;

 private:
  bool allTerminated() const;

  SimConfig cfg_;
  MemorySystem mem_;
  Core core_;
  std::unique_ptr<Scheduler> sched_;
  std::vector<std::unique_ptr<Process>> procs_;
  uint32_t nextPid_ = 1;
  uint64_t cycles_ = 0;
  uint64_t switchCycles_ = 0;
  uint64_t switchCount_ = 0;
  uint32_t lastPid_ = 0;
  bool hitLimit_ = false;
};

void printReport(const Simulator& sim);
bool writeCsv(const Simulator& sim, const std::string& path);

}  // namespace cpusim
