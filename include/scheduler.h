// scheduler.h -- pluggable process schedulers.
//
// The core runs exactly one process at a time. A scheduler decides which of the
// ready processes runs next and for how many cycles; the simulator drains the
// pipeline before every switch, so no architectural state ever leaks between
// processes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cpusim {

struct Process;

enum class SchedPolicy { RoundRobin, Priority, MLFQ };

bool parsePolicy(const std::string& text, SchedPolicy* out);
const char* policyName(SchedPolicy p);

class Scheduler {
 public:
  virtual ~Scheduler() = default;
  virtual const char* name() const = 0;

  // A process became runnable (freshly loaded, yielded, or preempted).
  virtual void admit(Process* p) = 0;
  // Choose the next process to run, or nullptr when nothing is ready.
  virtual Process* pick(uint64_t cycle) = 0;
  // How many cycles the process may run before it is preempted.
  virtual uint64_t quantum(const Process* p) const = 0;
  // Notifications used by the feedback queue.
  virtual void onPreempt(Process* p) { (void)p; }
  virtual void onYield(Process* p) { (void)p; }

 protected:
  static void setLevel(Process* p, int level);
  static int level(const Process* p);
  static int priorityOf(const Process* p);
};

std::unique_ptr<Scheduler> makeScheduler(SchedPolicy policy, uint64_t quantum);

}  // namespace cpusim
