#include "simulator.h"

#include <algorithm>

namespace cpusim {

const char* stateName(ProcState s) {
  switch (s) {
    case ProcState::New: return "new";
    case ProcState::Ready: return "ready";
    case ProcState::Running: return "running";
    case ProcState::Terminated: return "terminated";
  }
  return "?";
}

Simulator::Simulator(const SimConfig& cfg)
    : cfg_(cfg),
      mem_(cfg.mem),
      core_(mem_, cfg.core),
      sched_(makeScheduler(cfg.policy, cfg.quantum)) {
  if (cfg_.tracePath.empty()) return;
  TraceMeta meta;
  meta.icacheSets = cfg_.mem.icacheSets;
  meta.icacheWays = cfg_.mem.icacheWays;
  meta.dcacheSets = cfg_.mem.dcacheSets;
  meta.dcacheWays = cfg_.mem.dcacheWays;
  meta.lineSize = cfg_.mem.lineSize;
  meta.tlbEntries = cfg_.mem.tlbEntries;
  meta.frames = cfg_.mem.frames;
  meta.config = {
      {"scheduler", policyName(cfg_.policy)},
      {"quantum", std::to_string(cfg_.quantum) + " cycles"},
      {"context switch cost", std::to_string(cfg_.contextSwitchPenalty) + " cycles"},
      {"forwarding", cfg_.core.forwarding ? "enabled" : "disabled"},
      {"branch prediction", cfg_.core.predictor ? "2-bit counters + BTB"
                                                : "static predict-not-taken"},
      {"L1I", std::to_string(cfg_.mem.icacheSets) + " sets x " +
                  std::to_string(cfg_.mem.icacheWays) + " ways"},
      {"L1D", std::to_string(cfg_.mem.dcacheSets) + " sets x " +
                  std::to_string(cfg_.mem.dcacheWays) + " ways"},
      {"line size", std::to_string(cfg_.mem.lineSize) + " B"},
      {"TLB", std::to_string(cfg_.mem.tlbEntries) + " entries"},
      {"physical memory", std::to_string(cfg_.mem.frames) + " frames (" +
                              std::to_string(cfg_.mem.frames * PAGE_SIZE / 1024) +
                              " KiB)"},
      {"miss penalty", std::to_string(cfg_.mem.missPenalty) + " cycles"},
      {"page fault penalty", std::to_string(cfg_.mem.pageFaultPenalty) + " cycles"},
  };
  tracer_ = std::make_unique<TraceRecorder>(std::move(meta), cfg_.traceMaxCycles);
}

bool Simulator::writeTrace() const {
  if (tracer_ == nullptr) return false;
  return tracer_->write(cfg_.tracePath);
}

Process* Simulator::load(const Program& prog, int priority) {
  auto proc = std::make_unique<Process>();
  Process* p = proc.get();
  p->pid = nextPid_++;
  p->name = prog.name;
  p->priority = priority;
  p->reset();
  p->pc = prog.entry;
  p->arrivalCycle = cycles_;

  const uint32_t textSpan = std::max<uint32_t>(
      PAGE_SIZE, alignUp(static_cast<uint32_t>(prog.text.size()), PAGE_SIZE));
  const uint32_t dataSpan =
      alignUp(static_cast<uint32_t>(prog.data.size()), PAGE_SIZE) + HEAP_SIZE;

  p->regions.push_back({prog.textBase, textSpan, /*writable=*/false, "text"});
  p->regions.push_back({prog.dataBase, dataSpan, /*writable=*/true, "data+heap"});
  p->regions.push_back({STACK_TOP - STACK_SIZE, STACK_SIZE, /*writable=*/true, "stack"});

  mem_.registerProcess(p);
  mem_.loadIntoBackingStore(*p, prog.textBase, prog.text, /*writable=*/false);
  if (!prog.data.empty()) {
    mem_.loadIntoBackingStore(*p, prog.dataBase, prog.data, /*writable=*/true);
  }

  p->state = ProcState::Ready;
  sched_->admit(p);
  if (tracer_ != nullptr) tracer_->addProcess(*p, prog);
  procs_.push_back(std::move(proc));
  return p;
}

bool Simulator::allTerminated() const {
  for (const auto& p : procs_) {
    if (p->state != ProcState::Terminated) return false;
  }
  return true;
}

void Simulator::run() {
  while (cycles_ < cfg_.maxCycles) {
    if (core_.current() == nullptr) {
      if (allTerminated()) {
        if (tracer_ != nullptr) tracer_->finish(cycles_, mem_, procs_);
        return;
      }
      Process* next = sched_->pick(cycles_);
      if (next == nullptr) {
        if (tracer_ != nullptr) tracer_->finish(cycles_, mem_, procs_);
        return;  // nothing runnable and nothing to wait for
      }

      if (lastPid_ != 0 && lastPid_ != next->pid) {
        // Pay for the switch: pipeline was already drained, now flush the TLB
        // and burn the kernel-side bookkeeping cycles.
        mem_.onContextSwitch();
        cycles_ += cfg_.contextSwitchPenalty;
        switchCycles_ += cfg_.contextSwitchPenalty;
        ++switchCount_;
        ++next->stats.contextSwitches;
        next->stats.contextSwitchCycles += cfg_.contextSwitchPenalty;
      }
      const bool switched = lastPid_ != next->pid;
      lastPid_ = next->pid;
      if (!next->started) {
        next->started = true;
        next->firstRunCycle = cycles_;
      }
      core_.attach(next);
      if (tracer_ != nullptr) {
        pendingEvent_ = (switched ? "scheduled " : "resumed ") + next->name;
      }
    }

    if (tracer_ != nullptr) {
      tracer_->snapshot(cycles_, core_, mem_, procs_);
      if (!pendingEvent_.empty()) {
        tracer_->note(cycles_, pendingEvent_);
        pendingEvent_.clear();
      }
    }

    core_.cycle(cycles_);
    ++cycles_;
    if (tracer_ != nullptr) tracer_->annotate(core_.lastNote());

    Process* running = core_.current();
    if (running == nullptr) continue;

    if (core_.exitReason() == Exit::Running &&
        running->quantumUsed >= sched_->quantum(running)) {
      core_.requestDrain(Exit::Preempt);
    }

    if (core_.exitReason() != Exit::Running && core_.drained()) {
      const Exit reason = core_.exitReason();
      Process* done = core_.detach();
      switch (reason) {
        case Exit::Halt:
        case Exit::Fault:
          done->state = ProcState::Terminated;
          done->finishCycle = cycles_;
          break;
        case Exit::Yield:
          done->state = ProcState::Ready;
          sched_->onYield(done);
          sched_->admit(done);
          break;
        case Exit::Preempt:
          done->state = ProcState::Ready;
          sched_->onPreempt(done);
          sched_->admit(done);
          break;
        case Exit::Running:
          break;
      }
    }
  }
  hitLimit_ = true;
  if (tracer_ != nullptr) tracer_->finish(cycles_, mem_, procs_);
}

Stats Simulator::totals() const {
  Stats t;
  for (const auto& p : procs_) t.merge(p->stats);
  t.cycles = cycles_;
  t.contextSwitches = switchCount_;
  t.contextSwitchCycles = switchCycles_;
  return t;
}

}  // namespace cpusim
