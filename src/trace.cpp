#include "trace.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace cpusim {
namespace {

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string quoted(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

template <size_t N>
void appendArray(std::ostringstream& os, const std::vector<std::array<uint32_t, N>>& v) {
  os << '[';
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) os << ',';
    os << '[';
    for (size_t k = 0; k < N; ++k) {
      if (k) os << ',';
      os << v[i][k];
    }
    os << ']';
  }
  os << ']';
}

}  // namespace

TraceRecorder::TraceRecorder(TraceMeta meta, uint64_t maxCycles)
    : meta_(std::move(meta)), maxCycles_(maxCycles) {}

void TraceRecorder::addProcess(const Process& p, const Program& prog) {
  processNames_.emplace_back(p.pid, p.name);
  DisasmMap& map = disasm_[p.pid];
  for (size_t off = 0; off + 3 < prog.text.size(); off += 4) {
    const uint32_t pc = prog.textBase + static_cast<uint32_t>(off);
    const uint32_t raw = static_cast<uint32_t>(prog.text[off]) |
                         (static_cast<uint32_t>(prog.text[off + 1]) << 8) |
                         (static_cast<uint32_t>(prog.text[off + 2]) << 16) |
                         (static_cast<uint32_t>(prog.text[off + 3]) << 24);
    map[pc] = disassemble(decode(raw, pc));
  }
}

void TraceRecorder::diffProcess(const Process& p, Record* rec) {
  ProcState& prev = prev_[p.pid];

  if (!prev.seeded) {
    prev.seeded = true;
    prev.regs.fill(0);
    // Seed with the architectural reset value so the first snapshot reports sp.
    for (int i = 0; i < NUM_REGS; ++i) {
      if (p.regs[i] != 0) {
        rec->regDeltas.push_back({p.pid, static_cast<uint32_t>(i), p.regs[i]});
      }
    }
  } else {
    for (int i = 0; i < NUM_REGS; ++i) {
      if (p.regs[i] != prev.regs[i]) {
        rec->regDeltas.push_back({p.pid, static_cast<uint32_t>(i), p.regs[i]});
      }
    }
  }
  prev.regs = p.regs;

  for (const auto& kv : p.pageTable) {
    auto it = prev.pageTable.find(kv.first);
    const PTE& now = kv.second;
    if (it == prev.pageTable.end() || it->second.valid != now.valid ||
        it->second.frame != now.frame || it->second.dirty != now.dirty) {
      rec->pageDeltas.push_back({p.pid, kv.first, now.frame,
                                 now.valid ? 1u : 0u, now.dirty ? 1u : 0u});
      prev.pageTable[kv.first] = now;
    }
  }

#define CPUSIM_STAT_DELTA(name, desc)                                          \
  if (p.stats.name != prev.stats.name) {                                       \
    rec->statDeltas.push_back({p.pid, #name, p.stats.name - prev.stats.name});  \
  }
  CPUSIM_STAT_FIELDS(CPUSIM_STAT_DELTA)
#undef CPUSIM_STAT_DELTA
  prev.stats = p.stats;
}

void TraceRecorder::diffCaches(const MemorySystem& mem, Record* rec) {
  const std::vector<CacheLineView> nowI = mem.icacheView();
  const std::vector<CacheLineView> nowD = mem.dcacheView();
  if (!seededCaches_) {
    seededCaches_ = true;
    prevI_ = nowI;
    prevD_ = nowD;
    prevTlb_ = mem.tlbView();
    return;
  }
  auto diff = [&](const std::vector<CacheLineView>& now,
                  std::vector<CacheLineView>& prev, uint32_t kind,
                  uint32_t ways) {
    for (size_t i = 0; i < now.size() && i < prev.size(); ++i) {
      if (now[i] == prev[i]) continue;
      const uint32_t index = static_cast<uint32_t>(i) / ways;
      const uint32_t way = static_cast<uint32_t>(i) % ways;
      rec->cacheDeltas.push_back({kind, index, way, now[i].tag,
                                  now[i].valid ? 1u : 0u,
                                  now[i].dirty ? 1u : 0u});
      prev[i] = now[i];
    }
  };
  diff(nowI, prevI_, 0, meta_.icacheWays ? meta_.icacheWays : 1);
  diff(nowD, prevD_, 1, meta_.dcacheWays ? meta_.dcacheWays : 1);
}

void TraceRecorder::diffTlb(const MemorySystem& mem, Record* rec) {
  const std::vector<TlbEntryView> now = mem.tlbView();
  for (size_t i = 0; i < now.size() && i < prevTlb_.size(); ++i) {
    if (now[i] == prevTlb_[i]) continue;
    rec->tlbDeltas.push_back({static_cast<uint32_t>(i), now[i].pid, now[i].vpn,
                              now[i].frame, now[i].valid ? 1u : 0u});
    prevTlb_[i] = now[i];
  }
}

void TraceRecorder::snapshot(uint64_t cycle, const Core& core,
                             const MemorySystem& mem,
                             const std::vector<std::unique_ptr<Process>>& procs) {
  if (records_.size() >= maxCycles_) {
    if (!truncated_) {
      truncated_ = true;
      // finalRegs always means "state at the last recorded cycle". Snapshots
      // are taken before a cycle runs, so `procs` is already one cycle ahead of
      // the last record; prev_ is what that record actually left behind.
      finalRegs_.clear();
      for (const auto& p : procs) {
        auto it = prev_.find(p->pid);
        finalRegs_.emplace_back(p->pid,
                                it != prev_.end() ? it->second.regs : p->regs);
      }
    }
    return;
  }
  const Process* running = core.current();
  Record rec;
  rec.cycle = cycle;
  rec.pid = running != nullptr ? running->pid : 0;

  const CoreSnapshot cs = core.snapshot();
  rec.stages[0] = {cs.ifid.valid, cs.ifid.pc};
  rec.stages[1] = {cs.idex.valid, cs.idex.pc};
  rec.stages[2] = {cs.exmem.valid, cs.exmem.pc};
  rec.stages[3] = {cs.memwb.valid, cs.memwb.pc};
  rec.nextPc = cs.nextPc;
  rec.hasMemAccess = cs.hasMemAccess;
  rec.memAddr = cs.memAddr;
  rec.memIsStore = cs.memIsStore;

  diffCaches(mem, &rec);
  diffTlb(mem, &rec);
  // Every process is diffed, not just the running one: evicting a frame edits
  // the page table of whoever owned it.
  for (const auto& p : procs) diffProcess(*p, &rec);

  records_.push_back(std::move(rec));
}

void TraceRecorder::annotate(const std::string& note) {
  if (records_.empty()) return;
  records_.back().note = note;
}

void TraceRecorder::note(uint64_t cycle, const std::string& text) {
  if (records_.empty() || records_.back().cycle != cycle) return;
  records_.back().event = text;
}

void TraceRecorder::finish(uint64_t totalCycles, const MemorySystem& mem,
                           const std::vector<std::unique_ptr<Process>>& procs) {
  if (!records_.empty() && !truncated_) {
    Record rec;
    rec.cycle = records_.back().cycle + 1;
    rec.pid = records_.back().pid;
    rec.note = "pipeline empty";
    diffCaches(mem, &rec);
    diffTlb(mem, &rec);
    for (const auto& p : procs) diffProcess(*p, &rec);
    const bool hasDelta = !rec.regDeltas.empty() || !rec.statDeltas.empty() ||
                          !rec.cacheDeltas.empty() || !rec.tlbDeltas.empty() ||
                          !rec.pageDeltas.empty();
    if (hasDelta) records_.push_back(std::move(rec));
  }

  finalTotals_ = Stats{};
  for (const auto& p : procs) finalTotals_.merge(p->stats);
  finalTotals_.cycles = totalCycles;
  if (!truncated_) {
    finalRegs_.clear();
    for (const auto& p : procs) finalRegs_.emplace_back(p->pid, p->regs);
  }
  for (const auto& p : procs) {
    std::ostringstream os;
    os << stateName(p->state) << ", " << p->stats.instructions
       << " instructions retired";
    if (!p->faultMessage.empty()) os << ", " << p->faultMessage;
    if (!p->output.empty()) {
      os << ", output";
      for (size_t i = 0; i < p->output.size(); ++i) {
        os << (i ? ", " : " ") << p->output[i];
      }
    }
    summary_.emplace_back(p->name, os.str());
  }
}

bool TraceRecorder::write(const std::string& path) const {
  std::ofstream out(path);
  if (!out) return false;
  std::ostringstream os;

  os << "{\n\"version\":1,\n";
  os << "\"truncated\":" << (truncated_ ? "true" : "false") << ",\n";
  os << "\"geometry\":{"
     << "\"icacheSets\":" << meta_.icacheSets
     << ",\"icacheWays\":" << meta_.icacheWays
     << ",\"dcacheSets\":" << meta_.dcacheSets
     << ",\"dcacheWays\":" << meta_.dcacheWays
     << ",\"lineSize\":" << meta_.lineSize
     << ",\"tlbEntries\":" << meta_.tlbEntries
     << ",\"frames\":" << meta_.frames
     << ",\"pageSize\":" << PAGE_SIZE << "},\n";

  os << "\"config\":[";
  for (size_t i = 0; i < meta_.config.size(); ++i) {
    if (i) os << ',';
    os << '[' << quoted(meta_.config[i].first) << ','
       << quoted(meta_.config[i].second) << ']';
  }
  os << "],\n";

  os << "\"processes\":[";
  for (size_t i = 0; i < processNames_.size(); ++i) {
    if (i) os << ',';
    os << "{\"pid\":" << processNames_[i].first << ",\"name\":"
       << quoted(processNames_[i].second) << ",\"code\":{";
    const DisasmMap& map = disasm_.at(processNames_[i].first);
    bool first = true;
    for (const auto& kv : map) {
      if (!first) os << ',';
      first = false;
      os << '"' << kv.first << "\":" << quoted(kv.second);
    }
    os << "}}";
  }
  os << "],\n";

  os << "\"summary\":[";
  for (size_t i = 0; i < summary_.size(); ++i) {
    if (i) os << ',';
    os << '[' << quoted(summary_[i].first) << ',' << quoted(summary_[i].second) << ']';
  }
  os << "],\n";

  // Keyed by field name, matching the per-cycle delta keys exactly so the
  // viewer and verifier can compare the two without a translation table.
  os << "\"totals\":{";
  {
    bool first = true;
#define CPUSIM_TOTAL(name, desc)                                               \
    if (!first) os << ',';                                                     \
    first = false;                                                             \
    os << "\"" #name "\":" << finalTotals_.name;
    CPUSIM_STAT_FIELDS(CPUSIM_TOTAL)
#undef CPUSIM_TOTAL
  }
  os << "},\n";

  os << "\"finalRegs\":[";
  for (size_t i = 0; i < finalRegs_.size(); ++i) {
    if (i) os << ',';
    os << "{\"pid\":" << finalRegs_[i].first << ",\"regs\":[";
    for (int r = 0; r < NUM_REGS; ++r) {
      if (r) os << ',';
      os << finalRegs_[i].second[r];
    }
    os << "]}";
  }
  os << "],\n";

  os << "\"cycles\":[\n";
  for (size_t i = 0; i < records_.size(); ++i) {
    const Record& r = records_[i];
    if (i) os << ",\n";
    os << "{\"c\":" << r.cycle << ",\"p\":" << r.pid << ",\"s\":[";
    for (int k = 0; k < 4; ++k) {
      if (k) os << ',';
      if (r.stages[k].valid) {
        os << r.stages[k].pc;
      } else {
        os << "null";
      }
    }
    os << "],\"pc\":" << r.nextPc;
    if (r.hasMemAccess) {
      os << ",\"a\":[" << r.memAddr << ',' << (r.memIsStore ? 1 : 0) << ']';
    }
    if (!r.note.empty()) os << ",\"n\":" << quoted(r.note);
    if (!r.event.empty()) os << ",\"ev\":" << quoted(r.event);
    if (!r.regDeltas.empty()) {
      os << ",\"r\":";
      appendArray(os, r.regDeltas);
    }
    if (!r.statDeltas.empty()) {
      os << ",\"d\":[";
      for (size_t k = 0; k < r.statDeltas.size(); ++k) {
        if (k) os << ',';
        os << '[' << r.statDeltas[k].pid << ','
           << quoted(r.statDeltas[k].name) << ','
           << r.statDeltas[k].value << ']';
      }
      os << ']';
    }
    if (!r.cacheDeltas.empty()) {
      os << ",\"L\":";
      appendArray(os, r.cacheDeltas);
    }
    if (!r.tlbDeltas.empty()) {
      os << ",\"T\":";
      appendArray(os, r.tlbDeltas);
    }
    if (!r.pageDeltas.empty()) {
      os << ",\"P\":";
      appendArray(os, r.pageDeltas);
    }
    os << '}';
  }
  os << "\n]\n}\n";

  const std::string text = os.str();
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out.good();
}

}  // namespace cpusim
