// stats.h -- performance counters.
//
// Every counter the simulator maintains is declared once in the X-macro list
// below, which gives merge/CSV/report code for free and keeps the three from
// drifting apart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cpusim {

#define CPUSIM_STAT_FIELDS(X)                                                  \
  X(cycles, "cycles")                                                          \
  X(instructions, "instructions retired")                                      \
  X(bubbles, "pipeline bubbles")                                               \
  X(stallLoadUse, "load-use stall cycles")                                     \
  X(stallDataHazard, "data hazard stall cycles (no forwarding)")               \
  X(stallFetch, "instruction fetch stall cycles")                              \
  X(stallMemory, "data memory stall cycles")                                   \
  X(forwards, "operands forwarded")                                            \
  X(branches, "control instructions")                                          \
  X(mispredicts, "branch mispredictions")                                      \
  X(flushCycles, "cycles lost to flushes")                                     \
  X(icacheAccesses, "I-cache accesses")                                        \
  X(icacheMisses, "I-cache misses")                                            \
  X(dcacheAccesses, "D-cache accesses")                                        \
  X(dcacheMisses, "D-cache misses")                                            \
  X(writebacks, "dirty line writebacks")                                       \
  X(tlbAccesses, "TLB lookups")                                                \
  X(tlbMisses, "TLB misses")                                                   \
  X(pageFaults, "page faults")                                                 \
  X(pageEvictions, "page evictions")                                           \
  X(pageFaultCycles, "cycles servicing page faults")                           \
  X(contextSwitches, "context switches")                                       \
  X(contextSwitchCycles, "cycles spent switching")                             \
  X(loads, "loads")                                                            \
  X(stores, "stores")

struct Stats {
#define CPUSIM_DECLARE(name, desc) uint64_t name = 0;
  CPUSIM_STAT_FIELDS(CPUSIM_DECLARE)
#undef CPUSIM_DECLARE

  void merge(const Stats& other) {
#define CPUSIM_MERGE(name, desc) name += other.name;
    CPUSIM_STAT_FIELDS(CPUSIM_MERGE)
#undef CPUSIM_MERGE
  }

  double cpi() const {
    return instructions ? static_cast<double>(cycles) / instructions : 0.0;
  }
  double ipc() const {
    return cycles ? static_cast<double>(instructions) / cycles : 0.0;
  }
  double icacheMissRate() const {
    return icacheAccesses ? 100.0 * icacheMisses / icacheAccesses : 0.0;
  }
  double dcacheMissRate() const {
    return dcacheAccesses ? 100.0 * dcacheMisses / dcacheAccesses : 0.0;
  }
  double tlbMissRate() const {
    return tlbAccesses ? 100.0 * tlbMisses / tlbAccesses : 0.0;
  }
  double mispredictRate() const {
    return branches ? 100.0 * mispredicts / branches : 0.0;
  }
};

std::vector<std::pair<std::string, uint64_t>> statPairs(const Stats& s);

}  // namespace cpusim
