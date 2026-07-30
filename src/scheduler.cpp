#include "scheduler.h"

#include <algorithm>
#include <deque>

#include "process.h"

namespace cpusim {

bool parsePolicy(const std::string& text, SchedPolicy* out) {
  if (text == "rr" || text == "roundrobin") { *out = SchedPolicy::RoundRobin; return true; }
  if (text == "priority" || text == "prio") { *out = SchedPolicy::Priority; return true; }
  if (text == "mlfq") { *out = SchedPolicy::MLFQ; return true; }
  return false;
}

const char* policyName(SchedPolicy p) {
  switch (p) {
    case SchedPolicy::RoundRobin: return "round robin";
    case SchedPolicy::Priority: return "priority";
    case SchedPolicy::MLFQ: return "multi-level feedback queue";
  }
  return "?";
}

void Scheduler::setLevel(Process* p, int level) { p->mlfqLevel = level; }
int Scheduler::level(const Process* p) { return p->mlfqLevel; }
int Scheduler::priorityOf(const Process* p) { return p->priority; }

namespace {

class RoundRobin : public Scheduler {
 public:
  explicit RoundRobin(uint64_t quantum) : quantum_(quantum) {}
  const char* name() const override { return "round robin"; }
  void admit(Process* p) override { queue_.push_back(p); }
  Process* pick(uint64_t) override {
    if (queue_.empty()) return nullptr;
    Process* p = queue_.front();
    queue_.pop_front();
    return p;
  }
  uint64_t quantum(const Process*) const override { return quantum_; }

 private:
  uint64_t quantum_;
  std::deque<Process*> queue_;
};

// Preemptive priority scheduling. Lower `priority` wins; equal priorities fall
// back to first-come-first-served, which keeps it starvation-free within a
// priority band (though not across bands -- see the README).
class PriorityScheduler : public Scheduler {
 public:
  explicit PriorityScheduler(uint64_t quantum) : quantum_(quantum) {}
  const char* name() const override { return "priority"; }
  void admit(Process* p) override { ready_.push_back(p); }
  Process* pick(uint64_t) override {
    if (ready_.empty()) return nullptr;
    auto best = ready_.begin();
    for (auto it = ready_.begin(); it != ready_.end(); ++it) {
      if (priorityOf(*it) < priorityOf(*best)) best = it;
    }
    Process* p = *best;
    ready_.erase(best);
    return p;
  }
  uint64_t quantum(const Process*) const override { return quantum_; }

 private:
  uint64_t quantum_;
  std::vector<Process*> ready_;
};

// Three-level feedback queue. A process that burns a whole quantum drops a
// level (and earns a longer quantum); one that yields keeps its level. Every
// `boostInterval_` cycles everything is lifted back to level 0 so long-running
// jobs cannot be starved forever.
class Mlfq : public Scheduler {
 public:
  explicit Mlfq(uint64_t baseQuantum)
      : base_(baseQuantum == 0 ? 1 : baseQuantum),
        boostInterval_(base_ * 20) {}

  const char* name() const override { return "multi-level feedback queue"; }

  void admit(Process* p) override {
    const int lvl = std::min(level(p), kLevels - 1);
    queues_[lvl].push_back(p);
  }

  Process* pick(uint64_t cycle) override {
    if (cycle >= nextBoost_) {
      nextBoost_ = cycle + boostInterval_;
      std::deque<Process*> all;
      for (int l = 0; l < kLevels; ++l) {
        for (Process* p : queues_[l]) {
          setLevel(p, 0);
          all.push_back(p);
        }
        queues_[l].clear();
      }
      queues_[0] = all;
    }
    for (int l = 0; l < kLevels; ++l) {
      if (!queues_[l].empty()) {
        Process* p = queues_[l].front();
        queues_[l].pop_front();
        return p;
      }
    }
    return nullptr;
  }

  uint64_t quantum(const Process* p) const override {
    return base_ << std::min(level(p), kLevels - 1);
  }

  void onPreempt(Process* p) override {
    if (level(p) < kLevels - 1) setLevel(p, level(p) + 1);
  }

 private:
  static constexpr int kLevels = 3;
  uint64_t base_;
  uint64_t boostInterval_;
  uint64_t nextBoost_ = 0;
  std::deque<Process*> queues_[kLevels];
};

}  // namespace

std::unique_ptr<Scheduler> makeScheduler(SchedPolicy policy, uint64_t quantum) {
  switch (policy) {
    case SchedPolicy::RoundRobin: return std::make_unique<RoundRobin>(quantum);
    case SchedPolicy::Priority: return std::make_unique<PriorityScheduler>(quantum);
    case SchedPolicy::MLFQ: return std::make_unique<Mlfq>(quantum);
  }
  return std::make_unique<RoundRobin>(quantum);
}

}  // namespace cpusim
