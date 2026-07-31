#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "simulator.h"

namespace cpusim {
namespace {

void rule(char c = '-', int width = 78) {
  std::string line(width, c);
  std::printf("%s\n", line.c_str());
}

double pct(uint64_t part, uint64_t whole) {
  return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

struct Component {
  const char* label;
  uint64_t cycles;
};

}  // namespace

void printReport(const Simulator& sim) {
  const Stats t = sim.totals();
  const SimConfig& cfg = sim.config();

  rule('=');
  std::printf("CONFIGURATION\n");
  rule();
  std::printf("  scheduler          %s (quantum %llu cycles, switch cost %llu)\n",
              policyName(cfg.policy),
              static_cast<unsigned long long>(cfg.quantum),
              static_cast<unsigned long long>(cfg.contextSwitchPenalty));
  std::printf("  forwarding         %s\n", cfg.core.forwarding ? "enabled" : "disabled");
  std::printf("  branch prediction  %s\n",
              cfg.core.predictor ? "2-bit saturating counters + BTB"
                                 : "static predict-not-taken");
  std::printf("  L1I / L1D          %u sets x %u ways / %u sets x %u ways, %u B lines\n",
              cfg.mem.icacheSets, cfg.mem.icacheWays, cfg.mem.dcacheSets,
              cfg.mem.dcacheWays, cfg.mem.lineSize);
  std::printf("  TLB / frames       %u entries / %u frames (%u KiB physical)\n",
              cfg.mem.tlbEntries, cfg.mem.frames,
              cfg.mem.frames * PAGE_SIZE / 1024);
  std::printf("  penalties          cache miss %u, writeback %u, TLB miss %u, page fault %u\n",
              cfg.mem.missPenalty, cfg.mem.writebackPenalty,
              cfg.mem.tlbMissPenalty, cfg.mem.pageFaultPenalty);
  std::printf("  start              %s\n",
              cfg.prefault ? "warm -- text and data preloaded, no compulsory faults"
                           : "cold -- demand paged on first touch");

  std::printf("\n");
  rule('=');
  std::printf("PER-PROCESS\n");
  rule();
  std::printf("  %-12s %4s %-11s %9s %9s %6s %7s %7s %6s\n", "process", "pid",
              "state", "retired", "cycles", "CPI", "D$miss", "mispred", "flts");
  for (const auto& p : sim.processes()) {
    const Stats& s = p->stats;
    std::printf("  %-12s %4u %-11s %9llu %9llu %6.2f %6.1f%% %6.1f%% %6llu\n",
                p->name.c_str(), p->pid, stateName(p->state),
                static_cast<unsigned long long>(s.instructions),
                static_cast<unsigned long long>(s.cycles), s.cpi(),
                s.dcacheMissRate(), s.mispredictRate(),
                static_cast<unsigned long long>(s.pageFaults));
  }
  for (const auto& p : sim.processes()) {
    if (!p->faultMessage.empty()) {
      std::printf("  !! %s terminated: %s\n", p->name.c_str(), p->faultMessage.c_str());
    }
  }
  for (const auto& p : sim.processes()) {
    if (p->output.empty()) continue;
    std::printf("  %s output:", p->name.c_str());
    for (size_t i = 0; i < p->output.size(); ++i) {
      std::printf("%s%d", i ? ", " : " ", p->output[i]);
    }
    std::printf("\n");
  }
  std::printf("  turnaround: ");
  for (const auto& p : sim.processes()) {
    std::printf("%s=%llu ", p->name.c_str(),
                static_cast<unsigned long long>(
                    p->finishCycle > p->arrivalCycle ? p->finishCycle - p->arrivalCycle : 0));
  }
  std::printf("\n");

  std::printf("\n");
  rule('=');
  std::printf("AGGREGATE\n");
  rule();
  std::printf("  cycles                 %llu%s\n",
              static_cast<unsigned long long>(t.cycles),
              sim.hitCycleLimit() ? "  (cycle limit reached)" : "");
  std::printf("  instructions retired   %llu\n",
              static_cast<unsigned long long>(t.instructions));
  std::printf("  CPI / IPC              %.3f / %.3f\n", t.cpi(), t.ipc());
  std::printf("  operands forwarded     %llu\n",
              static_cast<unsigned long long>(t.forwards));
  std::printf("  control instructions   %llu (%llu mispredicted, %.1f%%)\n",
              static_cast<unsigned long long>(t.branches),
              static_cast<unsigned long long>(t.mispredicts), t.mispredictRate());
  std::printf("  loads / stores         %llu / %llu\n",
              static_cast<unsigned long long>(t.loads),
              static_cast<unsigned long long>(t.stores));
  std::printf("  L1I                    %llu accesses, %llu misses (%.2f%%)\n",
              static_cast<unsigned long long>(t.icacheAccesses),
              static_cast<unsigned long long>(t.icacheMisses), t.icacheMissRate());
  std::printf("  L1D                    %llu accesses, %llu misses (%.2f%%), %llu writebacks\n",
              static_cast<unsigned long long>(t.dcacheAccesses),
              static_cast<unsigned long long>(t.dcacheMisses), t.dcacheMissRate(),
              static_cast<unsigned long long>(t.writebacks));
  std::printf("  TLB                    %llu lookups, %llu misses (%.2f%%)\n",
              static_cast<unsigned long long>(t.tlbAccesses),
              static_cast<unsigned long long>(t.tlbMisses), t.tlbMissRate());
  std::printf("  paging                 %llu faults, %llu evictions\n",
              static_cast<unsigned long long>(t.pageFaults),
              static_cast<unsigned long long>(t.pageEvictions));
  std::printf("  context switches       %llu (%llu cycles)\n",
              static_cast<unsigned long long>(t.contextSwitches),
              static_cast<unsigned long long>(t.contextSwitchCycles));

  std::printf("\n");
  rule('=');
  std::printf("WHERE THE CYCLES WENT\n");
  rule();
  const uint64_t issued = t.instructions;
  const Component parts[] = {
      {"useful issue (1 instr/cycle)", issued},
      {"data memory stalls", t.stallMemory},
      {"instruction fetch stalls", t.stallFetch},
      {"load-use hazard stalls", t.stallLoadUse},
      {"data hazard stalls", t.stallDataHazard},
      {"branch mispredict flushes", t.flushCycles},
      {"context switch overhead", t.contextSwitchCycles},
  };
  uint64_t accounted = 0;
  for (const Component& c : parts) {
    accounted += c.cycles;
    if (c.cycles == 0) continue;
    std::printf("  %-32s %9llu  %5.1f%%\n", c.label,
                static_cast<unsigned long long>(c.cycles), pct(c.cycles, t.cycles));
  }
  const uint64_t other = t.cycles > accounted ? t.cycles - accounted : 0;
  std::printf("  %-32s %9llu  %5.1f%%\n", "pipeline fill / other bubbles",
              static_cast<unsigned long long>(other), pct(other, t.cycles));
  std::printf("  (of the memory stalls, %llu cycles were page faults)\n",
              static_cast<unsigned long long>(t.pageFaultCycles));

  std::printf("\n");
  rule('=');
  std::printf("BOTTLENECK ANALYSIS\n");
  rule();
  const Component* worst = nullptr;
  for (const Component& c : parts) {
    if (c.label == parts[0].label) continue;  // skip useful work
    if (worst == nullptr || c.cycles > worst->cycles) worst = &c;
  }
  std::vector<std::string> notes;
  if (t.cpi() > 2.0) {
    notes.push_back("CPI above 2.0: the pipeline is spending most of its time stalled.");
  }
  if (worst != nullptr && worst->cycles > 0) {
    notes.push_back(std::string("Largest single overhead: ") + worst->label + " (" +
                    std::to_string(static_cast<int>(pct(worst->cycles, t.cycles))) +
                    "% of all cycles).");
  }
  if (t.dcacheMissRate() > 10.0) {
    notes.push_back("D-cache miss rate over 10%: memory bound. Try --dcache-sets/-ways, "
                    "a larger line size, or a more cache-friendly access order.");
  }
  if (t.mispredictRate() > 15.0) {
    notes.push_back("Mispredict rate over 15%: control bound. Loop-heavy code with short "
                    "trip counts hurts a 2-bit predictor; try unrolling.");
  }
  if (t.pageEvictions > 0 && t.pageFaultCycles > t.cycles / 10) {
    notes.push_back("Page faults are being caused by evictions and cost over 10% of all "
                    "cycles: the working set does not fit. Increase --frames.");
  } else if (t.pageFaultCycles > t.cycles / 4 && t.pageEvictions == 0) {
    notes.push_back("Most of the paging cost is compulsory cold-start faults, which a "
                    "longer-running workload would amortise away.");
  }
  if (t.stallLoadUse > t.cycles / 20) {
    notes.push_back("Load-use stalls over 5% of cycles: schedule independent work between "
                    "a load and its first use.");
  }
  if (t.contextSwitchCycles > t.cycles / 10) {
    notes.push_back("Context switching costs over 10% of cycles: raise --quantum.");
  }
  if (notes.empty()) {
    notes.push_back("No dominant bottleneck; the pipeline is running close to its ideal rate.");
  }
  for (const std::string& n : notes) std::printf("  * %s\n", n.c_str());
  rule('=');
}

bool writeCsv(const Simulator& sim, const std::string& path) {
  std::ofstream out(path);
  if (!out) return false;
  out << "scope,counter,value\n";
  for (const auto& kv : statPairs(sim.totals())) {
    out << "total," << kv.first << "," << kv.second << "\n";
  }
  for (const auto& p : sim.processes()) {
    for (const auto& kv : statPairs(p->stats)) {
      out << p->name << "," << kv.first << "," << kv.second << "\n";
    }
  }
  return true;
}

}  // namespace cpusim
