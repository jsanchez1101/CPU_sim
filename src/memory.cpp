#include "memory.h"

#include <cassert>
#include <cstring>

#include "process.h"

namespace cpusim {

const char* faultName(Fault f) {
  switch (f) {
    case Fault::None: return "none";
    case Fault::Segmentation: return "segmentation fault";
    case Fault::Protection: return "protection fault";
    case Fault::Misaligned: return "misaligned access";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

Cache::Cache(uint32_t sets, uint32_t ways, uint32_t lineSize, std::string label)
    : sets_(sets ? sets : 1),
      ways_(ways ? ways : 1),
      lineSize_(lineSize ? lineSize : 4),
      label_(std::move(label)) {
  lines_.resize(static_cast<size_t>(sets_) * ways_);
}

uint32_t Cache::access(uint32_t paddr, bool isWrite, const MemConfig& cfg,
                       uint64_t* accesses, uint64_t* misses,
                       uint64_t* writebacks) {
  ++clock_;
  ++*accesses;

  const uint32_t blockId = paddr / lineSize_;
  const uint32_t index = blockId % sets_;
  const uint32_t tag = blockId / sets_;
  Line* ways = set(index);

  for (uint32_t w = 0; w < ways_; ++w) {
    if (ways[w].valid && ways[w].tag == tag) {
      ways[w].lastUse = clock_;
      if (isWrite) ways[w].dirty = true;
      return 0;  // hit, absorbed by the single-cycle MEM stage
    }
  }

  ++*misses;
  uint32_t cost = cfg.missPenalty;

  // Choose a victim: first invalid way, otherwise least recently used.
  uint32_t victim = 0;
  bool foundFree = false;
  for (uint32_t w = 0; w < ways_; ++w) {
    if (!ways[w].valid) {
      victim = w;
      foundFree = true;
      break;
    }
  }
  if (!foundFree) {
    uint64_t oldest = UINT64_MAX;
    for (uint32_t w = 0; w < ways_; ++w) {
      if (ways[w].lastUse < oldest) {
        oldest = ways[w].lastUse;
        victim = w;
      }
    }
    if (ways[victim].dirty) {
      cost += cfg.writebackPenalty;
      ++*writebacks;
    }
  }

  ways[victim].valid = true;
  ways[victim].tag = tag;
  ways[victim].dirty = isWrite;
  ways[victim].lastUse = clock_;
  return cost;
}

std::vector<CacheLineView> Cache::view() const {
  std::vector<CacheLineView> out;
  out.reserve(lines_.size());
  for (const Line& l : lines_) {
    out.push_back({l.valid, l.dirty, l.tag});
  }
  return out;
}

void Cache::invalidateFrame(uint32_t frame) {
  const uint32_t lo = frame * PAGE_SIZE;
  const uint32_t hi = lo + PAGE_SIZE;
  for (uint32_t index = 0; index < sets_; ++index) {
    Line* ways = set(index);
    for (uint32_t w = 0; w < ways_; ++w) {
      if (!ways[w].valid) continue;
      const uint32_t blockId = ways[w].tag * sets_ + index;
      const uint32_t addr = blockId * lineSize_;
      if (addr >= lo && addr < hi) {
        ways[w].valid = false;
        ways[w].dirty = false;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// TLB
// ---------------------------------------------------------------------------

PTE* Tlb::lookup(uint32_t pid, uint32_t vpn) {
  ++clock_;
  for (Entry& e : table_) {
    if (e.valid && e.pid == pid && e.vpn == vpn) {
      e.lastUse = clock_;
      return e.pte;
    }
  }
  return nullptr;
}

void Tlb::insert(uint32_t pid, uint32_t vpn, PTE* pte) {
  ++clock_;
  Entry* target = nullptr;
  for (Entry& e : table_) {
    if (e.valid && e.pid == pid && e.vpn == vpn) {
      target = &e;
      break;
    }
    if (!e.valid && target == nullptr) target = &e;
  }
  if (target == nullptr || target->valid) {
    uint64_t oldest = UINT64_MAX;
    for (Entry& e : table_) {
      if (!e.valid) {
        target = &e;
        break;
      }
      if (e.lastUse < oldest) {
        oldest = e.lastUse;
        target = &e;
      }
    }
  }
  if (target == nullptr) return;
  target->valid = true;
  target->pid = pid;
  target->vpn = vpn;
  target->pte = pte;
  target->lastUse = clock_;
}

void Tlb::invalidate(uint32_t pid, uint32_t vpn) {
  for (Entry& e : table_) {
    if (e.valid && e.pid == pid && e.vpn == vpn) e.valid = false;
  }
}

void Tlb::flush() {
  for (Entry& e : table_) e.valid = false;
}

std::vector<TlbEntryView> Tlb::view() const {
  std::vector<TlbEntryView> out;
  out.reserve(table_.size());
  for (const Entry& e : table_) {
    out.push_back({e.valid, e.pid, e.vpn,
                   e.pte != nullptr ? e.pte->frame : 0});
  }
  return out;
}

// ---------------------------------------------------------------------------
// MemorySystem
// ---------------------------------------------------------------------------

MemorySystem::MemorySystem(const MemConfig& cfg)
    : cfg_(cfg),
      physical_(static_cast<size_t>(cfg.frames) * PAGE_SIZE, 0),
      frameBusy_(cfg.frames, false),
      tlb_(cfg.tlbEntries),
      icache_(cfg.icacheSets, cfg.icacheWays, cfg.lineSize, "L1I"),
      dcache_(cfg.dcacheSets, cfg.dcacheWays, cfg.lineSize, "L1D") {}

void MemorySystem::registerProcess(Process* p) { processes_[p->pid] = p; }

void MemorySystem::loadIntoBackingStore(Process& p, uint32_t va,
                                        const std::vector<uint8_t>& bytes,
                                        bool writable) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    const uint32_t addr = va + static_cast<uint32_t>(i);
    const uint32_t vpn = pageNumber(addr);
    std::vector<uint8_t>& page = backingStore_[storeKey(p.pid, vpn)];
    if (page.empty()) page.assign(PAGE_SIZE, 0);
    page[pageOffset(addr)] = bytes[i];
  }
  const uint32_t firstPage = pageNumber(va);
  const uint32_t lastPage =
      bytes.empty() ? firstPage : pageNumber(va + static_cast<uint32_t>(bytes.size()) - 1);
  for (uint32_t vpn = firstPage; vpn <= lastPage; ++vpn) {
    PTE& pte = p.pageTable[vpn];
    pte.valid = false;
    pte.inStore = true;
    pte.writable = writable;
  }
}

uint32_t MemorySystem::prefault(Process& p) {
  uint32_t mapped = 0;
  for (auto& kv : p.pageTable) {
    PTE& pte = kv.second;
    if (pte.valid || !pte.inStore) continue;
    // Deliberately do not evict here: warming one process must not page out
    // another. If memory is too small, the rest is demand paged as before.
    if (framesInUse_ >= cfg_.frames) break;
    const uint32_t frame = allocateFrame(p, kv.first);
    uint8_t* dst = &physical_[static_cast<size_t>(frame) * PAGE_SIZE];
    auto it = backingStore_.find(storeKey(p.pid, kv.first));
    if (it != backingStore_.end()) {
      std::memcpy(dst, it->second.data(), PAGE_SIZE);
    } else {
      std::memset(dst, 0, PAGE_SIZE);
    }
    pte.frame = frame;
    pte.valid = true;
    pte.dirty = false;
    ++mapped;
  }
  return mapped;
}

void MemorySystem::evictOneFrame() {
  while (!fifo_.empty()) {
    const auto victim = fifo_.front();
    fifo_.pop_front();
    auto it = processes_.find(victim.first);
    if (it == processes_.end()) continue;
    Process& owner = *it->second;
    auto pteIt = owner.pageTable.find(victim.second);
    if (pteIt == owner.pageTable.end() || !pteIt->second.valid) continue;

    PTE& pte = pteIt->second;
    const uint32_t frame = pte.frame;
    if (pte.dirty || !pte.inStore) {
      std::vector<uint8_t>& page = backingStore_[storeKey(victim.first, victim.second)];
      page.assign(PAGE_SIZE, 0);
      std::memcpy(page.data(), &physical_[static_cast<size_t>(frame) * PAGE_SIZE],
                  PAGE_SIZE);
      pte.inStore = true;
    }
    pte.valid = false;
    pte.dirty = false;
    tlb_.invalidate(victim.first, victim.second);
    icache_.invalidateFrame(frame);
    dcache_.invalidateFrame(frame);
    frameBusy_[frame] = false;
    --framesInUse_;
    ++owner.stats.pageEvictions;
    return;
  }
}

uint32_t MemorySystem::allocateFrame(Process& p, uint32_t vpn) {
  if (framesInUse_ == cfg_.frames) evictOneFrame();
  for (uint32_t f = 0; f < cfg_.frames; ++f) {
    if (!frameBusy_[f]) {
      frameBusy_[f] = true;
      ++framesInUse_;
      fifo_.emplace_back(p.pid, vpn);
      return f;
    }
  }
  // Should be unreachable: eviction always frees a frame.
  assert(false && "out of physical frames");
  return 0;
}

MemorySystem::Translation MemorySystem::translate(Process& p, uint32_t va,
                                                 bool isWrite, bool isFetch) {
  Translation t;
  const Region* region = p.regionFor(va, 4);
  if (region == nullptr) {
    t.fault = Fault::Segmentation;
    return t;
  }
  if (isWrite && !region->writable) {
    t.fault = Fault::Protection;
    return t;
  }
  if ((va & 3u) != 0) {
    t.fault = Fault::Misaligned;
    return t;
  }
  (void)isFetch;

  const uint32_t vpn = pageNumber(va);
  ++p.stats.tlbAccesses;
  PTE* pte = tlb_.lookup(p.pid, vpn);
  if (pte == nullptr) {
    ++p.stats.tlbMisses;
    t.extraCycles += cfg_.tlbMissPenalty;
    pte = &p.pageTable[vpn];
    if (!pte->inStore && pte->frame == 0 && !pte->valid) {
      // First touch of an anonymous page (stack or heap): zero fill on demand.
      pte->writable = region->writable;
    }
  }

  if (!pte->valid) {
    ++p.stats.pageFaults;
    t.extraCycles += cfg_.pageFaultPenalty;
    p.stats.pageFaultCycles += cfg_.pageFaultPenalty;
    const uint32_t frame = allocateFrame(p, vpn);
    uint8_t* dst = &physical_[static_cast<size_t>(frame) * PAGE_SIZE];
    auto storeIt = backingStore_.find(storeKey(p.pid, vpn));
    if (pte->inStore && storeIt != backingStore_.end()) {
      std::memcpy(dst, storeIt->second.data(), PAGE_SIZE);
    } else {
      std::memset(dst, 0, PAGE_SIZE);
    }
    pte->frame = frame;
    pte->valid = true;
    pte->dirty = false;
  }

  tlb_.insert(p.pid, vpn, pte);
  if (isWrite) pte->dirty = true;
  t.paddr = pte->frame * PAGE_SIZE + pageOffset(va);
  return t;
}

AccessResult MemorySystem::fetch32(Process& p, uint32_t va, uint32_t* out) {
  AccessResult r;
  Translation t = translate(p, va, /*isWrite=*/false, /*isFetch=*/true);
  if (t.fault != Fault::None) {
    r.fault = t.fault;
    return r;
  }
  r.extraCycles = t.extraCycles +
                  icache_.access(t.paddr, false, cfg_, &p.stats.icacheAccesses,
                                 &p.stats.icacheMisses, &p.stats.writebacks);
  std::memcpy(out, &physical_[t.paddr], 4);
  return r;
}

AccessResult MemorySystem::read32(Process& p, uint32_t va, uint32_t* out) {
  AccessResult r;
  Translation t = translate(p, va, /*isWrite=*/false, /*isFetch=*/false);
  if (t.fault != Fault::None) {
    r.fault = t.fault;
    return r;
  }
  r.extraCycles = t.extraCycles +
                  dcache_.access(t.paddr, false, cfg_, &p.stats.dcacheAccesses,
                                 &p.stats.dcacheMisses, &p.stats.writebacks);
  std::memcpy(out, &physical_[t.paddr], 4);
  return r;
}

AccessResult MemorySystem::write32(Process& p, uint32_t va, uint32_t value) {
  AccessResult r;
  Translation t = translate(p, va, /*isWrite=*/true, /*isFetch=*/false);
  if (t.fault != Fault::None) {
    r.fault = t.fault;
    return r;
  }
  r.extraCycles = t.extraCycles +
                  dcache_.access(t.paddr, true, cfg_, &p.stats.dcacheAccesses,
                                 &p.stats.dcacheMisses, &p.stats.writebacks);
  std::memcpy(&physical_[t.paddr], &value, 4);
  return r;
}

bool MemorySystem::peek32(Process& p, uint32_t va, uint32_t* out) {
  if (p.regionFor(va, 4) == nullptr || (va & 3u) != 0) return false;
  const uint32_t vpn = pageNumber(va);
  auto it = p.pageTable.find(vpn);
  if (it != p.pageTable.end() && it->second.valid) {
    std::memcpy(out, &physical_[static_cast<size_t>(it->second.frame) * PAGE_SIZE +
                                pageOffset(va)],
                4);
    return true;
  }
  auto storeIt = backingStore_.find(storeKey(p.pid, vpn));
  if (storeIt != backingStore_.end()) {
    std::memcpy(out, &storeIt->second[pageOffset(va)], 4);
    return true;
  }
  *out = 0;  // untouched anonymous page reads as zero
  return true;
}

}  // namespace cpusim
