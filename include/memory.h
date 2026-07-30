// memory.h -- the memory hierarchy: caches, TLB, page tables, physical frames.
//
// The hierarchy a single access walks through:
//
//   virtual address
//     -> region permission check      (segmentation faults, write protection)
//     -> TLB                          (pid-tagged, fully associative, LRU)
//     -> page table                   (per process, demand paged)
//        -> page fault handler        (frame allocation, FIFO eviction, swap)
//     -> physical address
//     -> L1 I-cache or D-cache        (set associative, LRU, write-back)
//     -> physical memory
//
// Caches are tag-only: they model timing, while the bytes always live in
// PhysicalMemory. That keeps the data path trivially correct while still
// producing realistic hit/miss behaviour, which is what the performance
// counters care about.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "stats.h"

namespace cpusim {

struct Process;

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t PAGE_SHIFT = 12;

// Fixed virtual layout shared by every process.
constexpr uint32_t TEXT_BASE = 0x00001000;   // page 0 stays unmapped
constexpr uint32_t STACK_TOP = 0x00040000;   // exclusive upper bound
constexpr uint32_t STACK_SIZE = 4 * PAGE_SIZE;
constexpr uint32_t HEAP_SIZE = 8 * PAGE_SIZE;

inline uint32_t pageNumber(uint32_t va) { return va >> PAGE_SHIFT; }
inline uint32_t pageOffset(uint32_t va) { return va & (PAGE_SIZE - 1); }
inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

struct PTE {
  uint32_t frame = 0;
  bool valid = false;    // resident in a physical frame
  bool dirty = false;
  bool writable = true;
  bool inStore = false;  // a copy exists in the backing store
};

using PageTable = std::unordered_map<uint32_t, PTE>;

struct MemConfig {
  uint32_t frames = 64;
  uint32_t tlbEntries = 16;
  uint32_t lineSize = 32;
  uint32_t icacheSets = 32;
  uint32_t icacheWays = 2;
  uint32_t dcacheSets = 32;
  uint32_t dcacheWays = 2;
  uint32_t missPenalty = 12;       // cycles to fill a line from memory
  uint32_t writebackPenalty = 6;   // extra cycles when the victim is dirty
  uint32_t tlbMissPenalty = 2;     // page table walk
  uint32_t pageFaultPenalty = 200; // backing store transfer
};

enum class Fault { None, Segmentation, Protection, Misaligned };

struct AccessResult {
  Fault fault = Fault::None;
  uint32_t extraCycles = 0;  // stall cycles beyond the single-cycle hit case
  bool ok() const { return fault == Fault::None; }
};

const char* faultName(Fault f);

// ---------------------------------------------------------------------------

class Cache {
 public:
  Cache(uint32_t sets, uint32_t ways, uint32_t lineSize, std::string label);

  // Returns extra stall cycles. Updates the two counters it is handed so the
  // same class can serve as either the instruction or the data cache.
  uint32_t access(uint32_t paddr, bool isWrite, const MemConfig& cfg,
                  uint64_t* accesses, uint64_t* misses, uint64_t* writebacks);

  void invalidateFrame(uint32_t frame);
  const std::string& label() const { return label_; }

 private:
  struct Line {
    bool valid = false;
    bool dirty = false;
    uint32_t tag = 0;
    uint64_t lastUse = 0;
  };

  Line* set(uint32_t index) { return &lines_[index * ways_]; }

  uint32_t sets_;
  uint32_t ways_;
  uint32_t lineSize_;
  std::string label_;
  uint64_t clock_ = 0;
  std::vector<Line> lines_;
};

// ---------------------------------------------------------------------------

class Tlb {
 public:
  explicit Tlb(uint32_t entries) : entries_(entries) { table_.resize(entries); }

  PTE* lookup(uint32_t pid, uint32_t vpn);
  void insert(uint32_t pid, uint32_t vpn, PTE* pte);
  void invalidate(uint32_t pid, uint32_t vpn);
  void flush();

 private:
  struct Entry {
    bool valid = false;
    uint32_t pid = 0;
    uint32_t vpn = 0;
    PTE* pte = nullptr;
    uint64_t lastUse = 0;
  };

  uint32_t entries_;
  uint64_t clock_ = 0;
  std::vector<Entry> table_;
};

// ---------------------------------------------------------------------------

class MemorySystem {
 public:
  explicit MemorySystem(const MemConfig& cfg);

  void registerProcess(Process* p);

  // Places `bytes` at virtual address `va` in the backing store of `p` and
  // marks the covered pages as non-resident, so the program is brought in by
  // demand paging exactly like real code would be.
  void loadIntoBackingStore(Process& p, uint32_t va,
                            const std::vector<uint8_t>& bytes, bool writable);

  AccessResult fetch32(Process& p, uint32_t va, uint32_t* out);
  AccessResult read32(Process& p, uint32_t va, uint32_t* out);
  AccessResult write32(Process& p, uint32_t va, uint32_t value);

  // Bypasses caches and timing; used by tests and the post-mortem dump.
  bool peek32(Process& p, uint32_t va, uint32_t* out);

  void onContextSwitch() { tlb_.flush(); }

  const MemConfig& config() const { return cfg_; }
  uint32_t framesInUse() const { return framesInUse_; }
  uint32_t frameCount() const { return cfg_.frames; }

 private:
  struct Translation {
    Fault fault = Fault::None;
    uint32_t paddr = 0;
    uint32_t extraCycles = 0;
  };

  Translation translate(Process& p, uint32_t va, bool isWrite, bool isFetch);
  uint32_t allocateFrame(Process& p, uint32_t vpn);
  void evictOneFrame();
  static uint64_t storeKey(uint32_t pid, uint32_t vpn) {
    return (static_cast<uint64_t>(pid) << 32) | vpn;
  }

  MemConfig cfg_;
  std::vector<uint8_t> physical_;
  std::vector<bool> frameBusy_;
  uint32_t framesInUse_ = 0;
  std::deque<std::pair<uint32_t, uint32_t>> fifo_;  // (pid, vpn), oldest first
  std::unordered_map<uint32_t, Process*> processes_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> backingStore_;
  Tlb tlb_;
  Cache icache_;
  Cache dcache_;
};

}  // namespace cpusim
