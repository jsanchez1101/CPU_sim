// main.cpp -- command line driver.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "assembler.h"
#include "simulator.h"

using namespace cpusim;

namespace {

void usage(const char* argv0) {
  std::printf(
      "usage: %s [options] program.asm [program2.asm ...]\n"
      "\n"
      "scheduling\n"
      "  --sched rr|priority|mlfq   scheduling policy (default rr)\n"
      "  --quantum N                cycles per time slice (default 200)\n"
      "  --switch-cost N            cycles charged per context switch (default 20)\n"
      "  --priority N               priority for the next program listed (lower runs first)\n"
      "\n"
      "pipeline\n"
      "  --no-forwarding            disable EX/MEM and MEM/WB bypass paths\n"
      "  --no-predictor             use static predict-not-taken instead of a 2-bit BHT\n"
      "  --trace                    print the pipeline contents every cycle\n"
      "\n"
      "memory hierarchy\n"
      "  --frames N                 physical frames of %u bytes each (default 64)\n"
      "  --tlb N                    TLB entries (default 16)\n"
      "  --line N                   cache line size in bytes (default 32)\n"
      "  --icache SETSxWAYS         L1I geometry (default 32x2)\n"
      "  --dcache SETSxWAYS         L1D geometry (default 32x2)\n"
      "  --miss-penalty N           cycles to fill a line (default 12)\n"
      "  --fault-penalty N          cycles to service a page fault (default 200)\n"
      "\n"
      "output\n"
      "  --max-cycles N             safety limit (default 20000000)\n"
      "  --csv FILE                 dump every counter as CSV\n"
      "  --dump-mem ADDR:COUNT      print COUNT words starting at ADDR after the run\n"
      "  -h, --help                 this message\n",
      argv0, PAGE_SIZE);
}

bool parseGeometry(const char* text, uint32_t* sets, uint32_t* ways) {
  const char* x = std::strchr(text, 'x');
  if (x == nullptr) return false;
  *sets = static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
  *ways = static_cast<uint32_t>(std::strtoul(x + 1, nullptr, 10));
  return *sets > 0 && *ways > 0;
}

}  // namespace

int main(int argc, char** argv) {
  SimConfig cfg;
  std::vector<std::pair<std::string, int>> programs;
  int nextPriority = 0;
  std::string csvPath;
  uint32_t dumpAddr = 0;
  uint32_t dumpCount = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--sched") {
      if (!parsePolicy(needValue("--sched"), &cfg.policy)) {
        std::fprintf(stderr, "error: unknown scheduler\n");
        return 2;
      }
    } else if (arg == "--quantum") {
      cfg.quantum = std::strtoull(needValue("--quantum"), nullptr, 10);
    } else if (arg == "--switch-cost") {
      cfg.contextSwitchPenalty = std::strtoull(needValue("--switch-cost"), nullptr, 10);
    } else if (arg == "--priority") {
      nextPriority = static_cast<int>(std::strtol(needValue("--priority"), nullptr, 10));
    } else if (arg == "--no-forwarding") {
      cfg.core.forwarding = false;
    } else if (arg == "--no-predictor") {
      cfg.core.predictor = false;
    } else if (arg == "--trace") {
      cfg.core.trace = true;
    } else if (arg == "--frames") {
      cfg.mem.frames = static_cast<uint32_t>(std::strtoul(needValue("--frames"), nullptr, 10));
    } else if (arg == "--tlb") {
      cfg.mem.tlbEntries = static_cast<uint32_t>(std::strtoul(needValue("--tlb"), nullptr, 10));
    } else if (arg == "--line") {
      cfg.mem.lineSize = static_cast<uint32_t>(std::strtoul(needValue("--line"), nullptr, 10));
    } else if (arg == "--icache") {
      if (!parseGeometry(needValue("--icache"), &cfg.mem.icacheSets, &cfg.mem.icacheWays)) {
        std::fprintf(stderr, "error: --icache wants SETSxWAYS\n");
        return 2;
      }
    } else if (arg == "--dcache") {
      if (!parseGeometry(needValue("--dcache"), &cfg.mem.dcacheSets, &cfg.mem.dcacheWays)) {
        std::fprintf(stderr, "error: --dcache wants SETSxWAYS\n");
        return 2;
      }
    } else if (arg == "--miss-penalty") {
      cfg.mem.missPenalty = static_cast<uint32_t>(std::strtoul(needValue("--miss-penalty"), nullptr, 10));
    } else if (arg == "--fault-penalty") {
      cfg.mem.pageFaultPenalty = static_cast<uint32_t>(std::strtoul(needValue("--fault-penalty"), nullptr, 10));
    } else if (arg == "--max-cycles") {
      cfg.maxCycles = std::strtoull(needValue("--max-cycles"), nullptr, 10);
    } else if (arg == "--csv") {
      csvPath = needValue("--csv");
    } else if (arg == "--dump-mem") {
      const std::string spec = needValue("--dump-mem");
      const size_t colon = spec.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "error: --dump-mem wants ADDR:COUNT\n");
        return 2;
      }
      dumpAddr = static_cast<uint32_t>(std::strtoul(spec.c_str(), nullptr, 0));
      dumpCount = static_cast<uint32_t>(std::strtoul(spec.c_str() + colon + 1, nullptr, 0));
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return 2;
    } else {
      programs.emplace_back(arg, nextPriority);
    }
  }

  if (programs.empty()) {
    usage(argv[0]);
    return 2;
  }

  Simulator sim(cfg);
  for (const auto& entry : programs) {
    try {
      const Program prog = assembleFile(entry.first);
      std::printf("loaded %-12s %u instructions, text 0x%x, data 0x%x, entry 0x%x\n",
                  prog.name.c_str(), prog.instructionCount, prog.textBase,
                  prog.dataBase, prog.entry);
      sim.load(prog, entry.second);
    } catch (const AssembleError& e) {
      std::fprintf(stderr, "%s: %s\n", entry.first.c_str(), e.what());
      return 1;
    }
  }
  std::printf("\n");

  sim.run();
  printReport(sim);

  if (dumpCount > 0 && !sim.processes().empty()) {
    Process& p = *sim.processes().front();
    std::printf("\nmemory dump for %s\n", p.name.c_str());
    for (uint32_t i = 0; i < dumpCount; ++i) {
      const uint32_t addr = dumpAddr + i * 4;
      uint32_t value = 0;
      if (sim.memory().peek32(p, addr, &value)) {
        std::printf("  [0x%08x] %11d  0x%08x\n", addr, static_cast<int32_t>(value), value);
      } else {
        std::printf("  [0x%08x] <unmapped>\n", addr);
      }
    }
  }

  if (!csvPath.empty()) {
    if (writeCsv(sim, csvPath)) {
      std::printf("\nwrote %s\n", csvPath.c_str());
    } else {
      std::fprintf(stderr, "error: cannot write %s\n", csvPath.c_str());
      return 1;
    }
  }
  return 0;
}
