// trace.h -- records the full machine state every cycle and writes it as JSON
// for the interactive viewer in viewer/index.html.
//
// The recorder is a pure observer: it takes read-only snapshots of the core,
// the caches, the TLB and the page tables once per cycle and stores only what
// changed since the previous cycle. The viewer reconstructs the state at any
// cycle by replaying those deltas from the start, which keeps the file small
// without needing keyframes.
//
// Nothing here is on the fast path of an untraced run: if SimConfig::tracePath
// is empty the recorder is never constructed.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "assembler.h"
#include "memory.h"
#include "pipeline.h"
#include "process.h"

namespace cpusim {

// Per-process disassembly, so the viewer can label a stage from a PC alone.
using DisasmMap = std::map<uint32_t, std::string>;

struct TraceMeta {
  std::vector<std::pair<std::string, std::string>> config;
  uint32_t icacheSets = 0, icacheWays = 0;
  uint32_t dcacheSets = 0, dcacheWays = 0;
  uint32_t lineSize = 0;
  uint32_t tlbEntries = 0;
  uint32_t frames = 0;
};

class TraceRecorder {
 public:
  TraceRecorder(TraceMeta meta, uint64_t maxCycles);

  void addProcess(const Process& p, const Program& prog);

  // Called immediately before a cycle executes: captures the latch contents,
  // register file and memory structures the cycle is about to work on.
  void snapshot(uint64_t cycle, const Core& core, const MemorySystem& mem,
                const std::vector<std::unique_ptr<Process>>& procs);
  // Called immediately after, to attach the reason the cycle stalled.
  void annotate(const std::string& note);
  // Records a scheduling transition between cycles.
  void note(uint64_t cycle, const std::string& text);

  // Emits one trailing record so the deltas produced by the final cycle are
  // captured -- each snapshot only reports changes relative to the next one,
  // so without this the last cycle's retirement is lost.
  void finish(uint64_t totalCycles, const MemorySystem& mem,
              const std::vector<std::unique_ptr<Process>>& procs);
  bool write(const std::string& path) const;

  bool truncated() const { return truncated_; }
  uint64_t recordCount() const { return records_.size(); }

 private:
  struct StageRef {
    bool valid = false;
    uint32_t pc = 0;
  };

  struct Record {
    uint64_t cycle = 0;
    uint32_t pid = 0;
    StageRef stages[4];
    uint32_t nextPc = 0;
    bool hasMemAccess = false;
    uint32_t memAddr = 0;
    bool memIsStore = false;
    std::string note;
    std::string event;
    // pid is carried explicitly: every process is diffed each cycle, not just
    // the running one, so a delta does not necessarily belong to Record::pid.
    std::vector<std::array<uint32_t, 3>> regDeltas;          // pid, reg, value
    struct StatDelta {
      uint32_t pid;
      std::string name;
      uint64_t value;
    };
    std::vector<StatDelta> statDeltas;
    // kind ('i'/'d'), index, way, tag, valid, dirty
    std::vector<std::array<uint32_t, 6>> cacheDeltas;
    // slot, pid, vpn, frame, valid
    std::vector<std::array<uint32_t, 5>> tlbDeltas;
    // pid, vpn, frame, valid, dirty
    std::vector<std::array<uint32_t, 5>> pageDeltas;
  };

  struct ProcState {
    std::array<uint32_t, NUM_REGS> regs{};
    std::map<uint32_t, PTE> pageTable;
    Stats stats;
    bool seeded = false;
  };

  void diffProcess(const Process& p, Record* rec);
  void diffCaches(const MemorySystem& mem, Record* rec);
  void diffTlb(const MemorySystem& mem, Record* rec);

  TraceMeta meta_;
  uint64_t maxCycles_;
  bool truncated_ = false;

  std::vector<Record> records_;
  std::vector<std::pair<uint32_t, std::string>> processNames_;
  std::map<uint32_t, DisasmMap> disasm_;
  std::map<uint32_t, ProcState> prev_;
  std::vector<CacheLineView> prevI_, prevD_;
  std::vector<TlbEntryView> prevTlb_;
  bool seededCaches_ = false;

  Stats finalTotals_;
  std::vector<std::pair<std::string, std::string>> summary_;
  // Ground truth written straight from the Process objects, so the verifier can
  // confirm that replaying the deltas lands on the state the simulator reached.
  std::vector<std::pair<uint32_t, std::array<uint32_t, NUM_REGS>>> finalRegs_;
};

}  // namespace cpusim
