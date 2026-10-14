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
#include "trace.h"

namespace cpusim {

struct SimConfig {
  MemConfig mem;
  CoreConfig core;
  SchedPolicy policy = SchedPolicy::RoundRobin;
  uint64_t quantum = 200;
  uint64_t contextSwitchPenalty = 20;
  uint64_t maxCycles = 20'000'000;
  // Warm start: map text and data resident at load time, so measurements
  // reflect steady state rather than compulsory cold-start faults.
  bool prefault = false;
  // When non-empty, every cycle is recorded and written here as JSON for the
  // interactive viewer.
  std::string tracePath;
  uint64_t traceMaxCycles = 50'000;
};

// Upper bounds on the sizing knobs. They keep a mistyped or hostile value from
// requesting gigabytes of host memory, and keep physical addresses within 32
// bits.
constexpr uint32_t MAX_FRAMES = 65536;       // 256 MiB of simulated memory
constexpr uint32_t MAX_CACHE_LINES = 1u << 20;
constexpr uint32_t MAX_TLB_ENTRIES = 4096;

// Returns an empty string when cfg is usable, otherwise a message naming the
// first bad setting.
std::string validateConfig(const SimConfig& cfg);

class Simulator {
 public:
  // Throws std::invalid_argument when validateConfig rejects cfg.
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

  const TraceRecorder* recorder() const { return tracer_.get(); }
  bool writeTrace() const;

 private:
  bool allTerminated() const;

  SimConfig cfg_;
  MemorySystem mem_;
  Core core_;
  std::unique_ptr<Scheduler> sched_;
  std::vector<std::unique_ptr<Process>> procs_;
  std::unique_ptr<TraceRecorder> tracer_;
  uint32_t nextPid_ = 1;
  uint64_t cycles_ = 0;
  uint64_t switchCycles_ = 0;
  uint64_t switchCount_ = 0;
  uint32_t lastPid_ = 0;
  bool hitLimit_ = false;
  std::string pendingEvent_;
};

void printReport(const Simulator& sim);
bool writeCsv(const Simulator& sim, const std::string& path);

}  // namespace cpusim
